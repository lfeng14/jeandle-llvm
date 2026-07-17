//===- ProfileDevirtualization.cpp - Jeandle profile devirtualization ----===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Jeandle/ProfileDevirtualization.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/DomTreeUpdater.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Jeandle/Attributes.h"
#include "llvm/IR/Jeandle/GCStrategy.h"
#include "llvm/IR/Jeandle/InvokeType.h"
#include "llvm/IR/Jeandle/Metadata.h"
#include "llvm/IR/Jeandle/VMCallback.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Jeandle/JeandleTransformUtils.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <algorithm>
#include <cstdint>
#include <optional>

#define DEBUG_TYPE "profile-devirtualization"

using namespace llvm;

static cl::opt<bool> EnableProfileDevirtInlining(
    "jeandle-enable-profile-devirt-inline", cl::init(true),
    cl::desc("Allow profile-devirtualized targets to be considered by the "
             "Jeandle inliner"));

namespace {

void setBranchWeights(BranchInst &Branch, uint64_t TakenCount,
                      uint64_t TotalCount) {
  uint64_t TakenWeight = std::max<uint64_t>(TakenCount, 1);
  uint64_t NotTakenWeight = std::max<uint64_t>(
      TotalCount > TakenCount ? TotalCount - TakenCount : 1, 1);
  uint64_t MaxWeight = std::max(TakenWeight, NotTakenWeight);
  if (MaxWeight > UINT32_MAX) {
    TakenWeight = std::max<uint64_t>(TakenWeight * UINT32_MAX / MaxWeight, 1);
    NotTakenWeight =
        std::max<uint64_t>(NotTakenWeight * UINT32_MAX / MaxWeight, 1);
  }
  MDBuilder MDB(Branch.getContext());
  Branch.setMetadata(
      LLVMContext::MD_prof,
      MDB.createBranchWeights(static_cast<uint32_t>(TakenWeight),
                              static_cast<uint32_t>(NotTakenWeight)));
}

void createVirtualMissPath(InvokeInst &CB, BasicBlock *MissBlock,
                           const jeandle::VMCallbacks &Callbacks,
                           uint64_t StatepointID, DomTreeUpdater *DTU) {
  SmallVector<Value *, 8> Args(CB.args());
  SmallVector<OperandBundleDef, 4> Bundles;
  CB.getOperandBundlesAsDefs(Bundles);

  BasicBlock *HitBlock = CB.getParent();
  BasicBlock *OriginalNormalDest = CB.getNormalDest();
  BasicBlock *UnwindDest = CB.getUnwindDest();
  BasicBlock *JoinBlock =
      BasicBlock::Create(CB.getContext(), CB.getName() + ".profile.devirt.join",
                         HitBlock->getParent(), OriginalNormalDest);
  BranchInst::Create(OriginalNormalDest, JoinBlock);

  for (PHINode &Phi : OriginalNormalDest->phis())
    for (unsigned I = 0, E = Phi.getNumIncomingValues(); I != E; ++I)
      if (Phi.getIncomingBlock(I) == HitBlock)
        Phi.setIncomingBlock(I, JoinBlock);

  CB.setNormalDest(JoinBlock);

  IRBuilder<> Builder(MissBlock);
  InvokeInst *Miss = Builder.CreateInvoke(CB.getCalledFunction(), JoinBlock,
                                          UnwindDest, Args, Bundles);
  Miss->setCallingConv(CB.getCallingConv());
  Miss->setAttributes(CB.getAttributes());
  Miss->copyMetadata(CB);
  // Prevent a later refinement round from guarding this fallback again.
  Miss->addFnAttr(Attribute::get(
      CB.getContext(), jeandle::Attribute::ProfileDevirtualizationMiss));

  for (PHINode &Phi : UnwindDest->phis()) {
    int HitIndex = Phi.getBasicBlockIndex(HitBlock);
    assert(HitIndex >= 0 && "unwind phi must contain the original invoke edge");
    Phi.addIncoming(Phi.getIncomingValue(HitIndex), MissBlock);
  }

  int64_t NewStatepointID =
      Callbacks.GetNewStatepointID(static_cast<int64_t>(StatepointID));
  if (NewStatepointID < 0)
    reportInvalidStatepointID(
        CB, "ProfileDevirtualization",
        "GetNewStatepointID returned a negative id");
  setStatepointID(*Miss, static_cast<uint64_t>(NewStatepointID));

  if (!CB.getType()->isVoidTy() && !CB.use_empty()) {
    PHINode *Result =
        PHINode::Create(CB.getType(), 2, CB.getName() + ".profile.devirt",
                        JoinBlock->getFirstInsertionPt());
    Result->addIncoming(&CB, HitBlock);
    Result->addIncoming(Miss, MissBlock);

    SmallVector<Use *, 8> Uses;
    for (Use &U : CB.uses())
      if (U.getUser() != Result)
        Uses.push_back(&U);
    for (Use *U : Uses)
      U->set(Result);
  }

  if (DTU) {
    DTU->applyUpdates({{DominatorTree::Delete, HitBlock, OriginalNormalDest},
                       {DominatorTree::Insert, HitBlock, JoinBlock},
                       {DominatorTree::Insert, MissBlock, JoinBlock},
                       {DominatorTree::Insert, MissBlock, UnwindDest},
                       {DominatorTree::Insert, JoinBlock, OriginalNormalDest}});
    DTU->flush();
  }
}

BasicBlock *insertExactReceiverCheck(Instruction &Inst, Value *Receiver,
                                     uintptr_t ReceiverKlass,
                                     uint64_t ProfileCount,
                                     uint64_t ProfileTotalCount,
                                     const StringRef &Prefix,
                                     DomTreeUpdater *DTU) {
  assert(Receiver->getType()->isPointerTy() && "Receiver must be a pointer");

  BasicBlock *BB = Inst.getParent();
  Module *M = Inst.getModule();
  Function *LoadKlassFn = M->getFunction("jeandle.load_klass");
  if (!LoadKlassFn)
    return nullptr;

  LLVMContext &Context = Inst.getContext();
  BasicBlock *CheckPass = SplitBlock(BB, &Inst, DTU, nullptr, nullptr,
                                     Prefix + "_exact_receiver_pass");
  BasicBlock *CheckFail = BasicBlock::Create(
      Context, Prefix + "_exact_receiver_fail", BB->getParent(), CheckPass);

  BB->getTerminator()->eraseFromParent();
  IRBuilder<> Builder(BB);

  // Profile receiver types are exact; a subtype check would admit subclasses.
  FunctionCallee LoadKlass(LoadKlassFn);
  CallInst *ActualKlass = Builder.CreateCall(LoadKlass, {Receiver});
  ActualKlass->setCallingConv(CallingConv::Hotspot_JIT);

  PointerType *KlassTy =
      PointerType::get(Context, jeandle::AddrSpace::CHeapAddrSpace);
  Value *ExpectedKlass =
      Builder.CreateIntToPtr(Builder.getInt64(ReceiverKlass), KlassTy);
  Value *IsProfiledReceiver =
      Builder.CreateICmpEQ(ActualKlass, ExpectedKlass, Prefix + "_is_exact_0");
  BranchInst *Guard =
      Builder.CreateCondBr(IsProfiledReceiver, CheckPass, CheckFail);
  setBranchWeights(*Guard, ProfileCount, ProfileTotalCount);

  if (DTU) {
    DTU->applyUpdates({{DominatorTree::Insert, BB, CheckFail}});
    DTU->flush();
  }
  return CheckFail;
}

struct BimorphicCheckBlocks {
  BasicBlock *SecondHitBlock = nullptr;
  BasicBlock *MissBlock = nullptr;
};

BimorphicCheckBlocks insertBimorphicReceiverChecks(
    Instruction &Inst, Value *Receiver, uintptr_t ReceiverKlass,
    uint64_t ProfileCount, uintptr_t ReceiverKlass2, uint64_t ProfileCount2,
    uint64_t ProfileTotalCount, const StringRef &Prefix, DomTreeUpdater *DTU) {
  assert(Receiver->getType()->isPointerTy() && "Receiver must be a pointer");
  assert(ReceiverKlass2 != 0 && "second receiver must be present");

  BasicBlock *BB = Inst.getParent();
  Module *M = Inst.getModule();
  Function *LoadKlassFn = M->getFunction("jeandle.load_klass");
  if (!LoadKlassFn)
    return {};

  LLVMContext &Context = Inst.getContext();
  BasicBlock *FirstHitBlock = SplitBlock(
      BB, &Inst, DTU, nullptr, nullptr, Prefix + "_exact_receiver_0_pass");
  BasicBlock *SecondCheckBlock = BasicBlock::Create(
      Context, Prefix + "_exact_receiver_1_check", BB->getParent(),
      FirstHitBlock);
  BasicBlock *SecondHitBlock = BasicBlock::Create(
      Context, Prefix + "_exact_receiver_1_pass", BB->getParent(),
      FirstHitBlock);
  BasicBlock *MissBlock = BasicBlock::Create(
      Context, Prefix + "_exact_receiver_fail", BB->getParent(), FirstHitBlock);

  BB->getTerminator()->eraseFromParent();
  IRBuilder<> Builder(BB);
  FunctionCallee LoadKlass(LoadKlassFn);
  CallInst *ActualKlass = Builder.CreateCall(LoadKlass, {Receiver});
  ActualKlass->setCallingConv(CallingConv::Hotspot_JIT);

  PointerType *KlassTy =
      PointerType::get(Context, jeandle::AddrSpace::CHeapAddrSpace);
  Value *ExpectedKlass =
      Builder.CreateIntToPtr(Builder.getInt64(ReceiverKlass), KlassTy);
  Value *IsReceiver =
      Builder.CreateICmpEQ(ActualKlass, ExpectedKlass, Prefix + "_is_exact_0");
  BranchInst *FirstGuard =
      Builder.CreateCondBr(IsReceiver, FirstHitBlock, SecondCheckBlock);
  setBranchWeights(*FirstGuard, ProfileCount, ProfileTotalCount);

  IRBuilder<> SecondBuilder(SecondCheckBlock);
  Value *ExpectedKlass2 = SecondBuilder.CreateIntToPtr(
      SecondBuilder.getInt64(ReceiverKlass2), KlassTy);
  Value *IsReceiver2 = SecondBuilder.CreateICmpEQ(ActualKlass, ExpectedKlass2,
                                                  Prefix + "_is_exact_1");
  BranchInst *SecondGuard = SecondBuilder.CreateCondBr(
      IsReceiver2, SecondHitBlock, MissBlock);
  uint64_t RemainingCount =
      ProfileTotalCount > ProfileCount ? ProfileTotalCount - ProfileCount : 1;
  setBranchWeights(*SecondGuard, ProfileCount2, RemainingCount);

  if (DTU) {
    DTU->applyUpdates({{DominatorTree::Insert, BB, SecondCheckBlock},
                       {DominatorTree::Insert, SecondCheckBlock,
                        SecondHitBlock},
                       {DominatorTree::Insert, SecondCheckBlock, MissBlock}});
    DTU->flush();
  }
  return {SecondHitBlock, MissBlock};
}

InvokeInst *createBimorphicCallPaths(InvokeInst &CB,
                                     const BimorphicCheckBlocks &Blocks,
                                     const jeandle::VMCallbacks &Callbacks,
                                     uint64_t StatepointID,
                                     bool CreateVirtualMiss,
                                     DomTreeUpdater *DTU) {
  SmallVector<Value *, 8> Args(CB.args());
  SmallVector<OperandBundleDef, 4> Bundles;
  CB.getOperandBundlesAsDefs(Bundles);

  BasicBlock *FirstHitBlock = CB.getParent();
  BasicBlock *OriginalNormalDest = CB.getNormalDest();
  BasicBlock *UnwindDest = CB.getUnwindDest();
  BasicBlock *JoinBlock =
      BasicBlock::Create(CB.getContext(), CB.getName() + ".profile.devirt.join",
                         FirstHitBlock->getParent(), OriginalNormalDest);
  BranchInst::Create(OriginalNormalDest, JoinBlock);

  for (PHINode &Phi : OriginalNormalDest->phis())
    for (unsigned I = 0, E = Phi.getNumIncomingValues(); I != E; ++I)
      if (Phi.getIncomingBlock(I) == FirstHitBlock)
        Phi.setIncomingBlock(I, JoinBlock);
  CB.setNormalDest(JoinBlock);

  IRBuilder<> SecondBuilder(Blocks.SecondHitBlock);
  InvokeInst *SecondHitCall = SecondBuilder.CreateInvoke(
      CB.getCalledFunction(), JoinBlock, UnwindDest, Args, Bundles);
  SecondHitCall->setCallingConv(CB.getCallingConv());
  SecondHitCall->setAttributes(CB.getAttributes());
  SecondHitCall->copyMetadata(CB);
  int64_t SecondStatepointID =
      Callbacks.GetNewStatepointID(static_cast<int64_t>(StatepointID));
  if (SecondStatepointID < 0)
    reportInvalidStatepointID(
        CB, "ProfileDevirtualization",
        "GetNewStatepointID returned a negative id");
  setStatepointID(*SecondHitCall, static_cast<uint64_t>(SecondStatepointID));

  InvokeInst *MissCall = nullptr;
  if (CreateVirtualMiss) {
    IRBuilder<> MissBuilder(Blocks.MissBlock);
    MissCall = MissBuilder.CreateInvoke(CB.getCalledFunction(), JoinBlock,
                                        UnwindDest, Args, Bundles);
    MissCall->setCallingConv(CB.getCallingConv());
    MissCall->setAttributes(CB.getAttributes());
    MissCall->copyMetadata(CB);
    MissCall->addFnAttr(Attribute::get(
        CB.getContext(), jeandle::Attribute::ProfileDevirtualizationMiss));
    int64_t MissStatepointID =
        Callbacks.GetNewStatepointID(static_cast<int64_t>(StatepointID));
    if (MissStatepointID < 0)
      reportInvalidStatepointID(
          CB, "ProfileDevirtualization",
          "GetNewStatepointID returned a negative id");
    setStatepointID(*MissCall, static_cast<uint64_t>(MissStatepointID));
  }

  for (PHINode &Phi : UnwindDest->phis()) {
    int FirstHitIndex = Phi.getBasicBlockIndex(FirstHitBlock);
    assert(FirstHitIndex >= 0 &&
           "unwind phi must contain the original invoke edge");
    Value *Incoming = Phi.getIncomingValue(FirstHitIndex);
    Phi.addIncoming(Incoming, Blocks.SecondHitBlock);
    if (MissCall)
      Phi.addIncoming(Incoming, Blocks.MissBlock);
  }

  if (!CB.getType()->isVoidTy() && !CB.use_empty()) {
    PHINode *Result = PHINode::Create(CB.getType(), MissCall ? 3 : 2,
                                      CB.getName() + ".profile.devirt",
                                      JoinBlock->getFirstInsertionPt());
    Result->addIncoming(&CB, FirstHitBlock);
    Result->addIncoming(SecondHitCall, Blocks.SecondHitBlock);
    if (MissCall)
      Result->addIncoming(MissCall, Blocks.MissBlock);

    SmallVector<Use *, 8> Uses;
    for (Use &U : CB.uses())
      if (U.getUser() != Result)
        Uses.push_back(&U);
    for (Use *U : Uses)
      U->set(Result);
  }

  if (DTU) {
    SmallVector<DominatorTree::UpdateType, 8> Updates = {
        {DominatorTree::Delete, FirstHitBlock, OriginalNormalDest},
        {DominatorTree::Insert, FirstHitBlock, JoinBlock},
        {DominatorTree::Insert, Blocks.SecondHitBlock, JoinBlock},
        {DominatorTree::Insert, Blocks.SecondHitBlock, UnwindDest},
        {DominatorTree::Insert, JoinBlock, OriginalNormalDest}};
    if (MissCall) {
      Updates.push_back(
          {DominatorTree::Insert, Blocks.MissBlock, JoinBlock});
      Updates.push_back(
          {DominatorTree::Insert, Blocks.MissBlock, UnwindDest});
    }
    DTU->applyUpdates(Updates);
    DTU->flush();
  }
  return SecondHitCall;
}

bool optimizeCallSite(InvokeInst &CB, DomTreeUpdater &DTU,
                      const jeandle::VMCallbacks &Callbacks, uintptr_t Caller,
                      int PatchSize,
                      ArrayRef<Function *> InlineScopeCallers) {
  // Do not recursively guard a fallback created by an earlier round.
  if (CB.getAttributes().hasFnAttr(
          jeandle::Attribute::ProfileDevirtualizationMiss))
    return false;

  Attribute CallStub = CB.getFnAttr(jeandle::Attribute::CallStub);
  if (!checkStringAttr(CallStub) ||
      CallStub.getValueAsString() != "virtual_call")
    return false;

  Attribute BC = CB.getFnAttr(jeandle::Attribute::Bytecode);
  assert(checkStringAttr(BC) && "virtual call must have bytecode attr");
  StringRef Bytecode = BC.getValueAsString();
  jeandle::InvokeType InvokeKind = jeandle::getInvokeType(Bytecode);
  if (InvokeKind != jeandle::InvokeVirtual &&
      InvokeKind != jeandle::InvokeInterface)
    return false;

  Value *Receiver = CB.getArgOperand(0);
  assert(Receiver->getType()->isPointerTy() &&
         "virtual call receiver must be a pointer");

  uintptr_t Callee = 0;
  uintptr_t Holder = 0;
  uint64_t Id = 0;
  Function *VirtualCallee = CB.getCalledFunction();
  if (!VirtualCallee)
    return false;
  getFunctionJavaMethod(*VirtualCallee, Callee);
  getUIntPtrFnAttr(CB, jeandle::Attribute::DeclaredHolder, Holder);
  getUIntFnAttr(CB, jeandle::Attribute::StatepointID, Id);
  assert(Id <= 0xffffffff && "must be 32 bits");
  assert(Callee != 0 && Holder != 0 && "should be a java call");

  uintptr_t ScopeCaller =
      getInlineScopeJavaMethod(CB, Caller, InlineScopeCallers);
  int BCI = getCurrentDeoptBCI(CB);
  jeandle::ProfileDevirtInfo OptInfo =
      jeandle::ProfileDevirtInfo::decode(Callbacks.GetProfileDevirtInfo(
          ScopeCaller, Callee, Holder, BCI, InvokeKind));
  if (OptInfo.ReceiverKlass == 0 || OptInfo.TargetMethod == 0)
    return false;
  // Keep profile devirtualization independent from the inliner. A guarded
  // direct target remains valid even if a later inline round declines it.
  bool IsBimorphic = OptInfo.ReceiverKlass2 != 0;
  if (IsBimorphic && OptInfo.TargetMethod2 == 0)
    return false;

  std::string TargetName = Callbacks.GetJavaMethodName(OptInfo.TargetMethod);
  if (TargetName.empty())
    return false;
  std::string TargetName2;
  if (IsBimorphic) {
    TargetName2 = Callbacks.GetJavaMethodName(OptInfo.TargetMethod2);
    if (TargetName2.empty())
      return false;
  }

  FunctionCallee StaticTargetCallee = CB.getModule()->getOrInsertFunction(
      TargetName, CB.getFunctionType());
  Function *Func = dyn_cast<Function>(StaticTargetCallee.getCallee());
  if (!Func)
    return false;
  FunctionCallee StaticTargetCallee2;
  Function *Func2 = nullptr;
  if (IsBimorphic) {
    StaticTargetCallee2 = CB.getModule()->getOrInsertFunction(
        TargetName2, CB.getFunctionType());
    Func2 = dyn_cast<Function>(StaticTargetCallee2.getCallee());
    if (!Func2)
      return false;
  }

  std::string Prefix = "bci_profile_devirt_" + std::to_string(BCI);
  std::optional<OperandBundleDef> PreCallDeopt;
  if (OptInfo.DeoptimizeOnMiss)
    PreCallDeopt = createPreCallDeoptBundle(CB);

  InvokeInst *SecondHitCall = nullptr;
  if (IsBimorphic) {
    BimorphicCheckBlocks Blocks = insertBimorphicReceiverChecks(
        CB, Receiver, OptInfo.ReceiverKlass, OptInfo.Count,
        OptInfo.ReceiverKlass2, OptInfo.Count2, OptInfo.TotalCount, Prefix,
        &DTU);
    if (!Blocks.SecondHitBlock || !Blocks.MissBlock)
      return false;
    SecondHitCall = createBimorphicCallPaths(
        CB, Blocks, Callbacks, Id, !OptInfo.DeoptimizeOnMiss, &DTU);
    if (OptInfo.DeoptimizeOnMiss) {
      IRBuilder<> MissBuilder(Blocks.MissBlock);
      buildDeoptimize(MissBuilder, *CB.getModule(), OptInfo.DeoptReason,
                      jeandle::Deoptimization::Action_maybe_recompile,
                      *PreCallDeopt);
    }
  } else {
    BasicBlock *MissBlock = insertExactReceiverCheck(
        CB, Receiver, OptInfo.ReceiverKlass, OptInfo.Count, OptInfo.TotalCount,
        Prefix, &DTU);
    if (!MissBlock)
      return false;
    if (OptInfo.DeoptimizeOnMiss) {
      IRBuilder<> MissBuilder(MissBlock);
      buildDeoptimize(MissBuilder, *CB.getModule(), OptInfo.DeoptReason,
                      jeandle::Deoptimization::Action_maybe_recompile,
                      *PreCallDeopt);
    } else {
      createVirtualMissPath(CB, MissBlock, Callbacks, Id, &DTU);
    }
  }

  updateStaticOptVirtualCallAttrs(CB, PatchSize,
                                  EnableProfileDevirtInlining);
  CB.setCalledFunction(StaticTargetCallee);
  Func->setCallingConv(CallingConv::Hotspot_JIT);
  Func->setGC(jeandle::JeandleGC);
  Func->addFnAttr(Attribute::get(CB.getContext(),
                                 jeandle::Attribute::JavaMethod,
                                 std::to_string(OptInfo.TargetMethod)));

  if (SecondHitCall) {
    updateStaticOptVirtualCallAttrs(*SecondHitCall, PatchSize,
                                    EnableProfileDevirtInlining);
    SecondHitCall->setCalledFunction(StaticTargetCallee2);
    Func2->setCallingConv(CallingConv::Hotspot_JIT);
    Func2->setGC(jeandle::JeandleGC);
    Func2->addFnAttr(Attribute::get(CB.getContext(),
                                    jeandle::Attribute::JavaMethod,
                                    std::to_string(OptInfo.TargetMethod2)));
    uint64_t SecondId = 0;
    getUIntFnAttr(*SecondHitCall, jeandle::Attribute::StatepointID, SecondId);
    Callbacks.UpdateToStaticOptVirtualCall(static_cast<int64_t>(SecondId));
  }

  Callbacks.UpdateToStaticOptVirtualCall(static_cast<int64_t>(Id));
  LLVM_DEBUG(dbgs() << "Profile devirtualized " << CB;
             if (SecondHitCall) dbgs() << " and " << *SecondHitCall;
             dbgs() << "\n";);
  DTU.flush();
  return true;
}

} // namespace

PreservedAnalyses ProfileDevirtualization::run(Function &F,
                                               FunctionAnalysisManager &FAM) {
  Module *M = F.getParent();
  if (!M->getNamedMetadata(jeandle::Metadata::JavaMethodCompilation))
    return PreservedAnalyses::all();

  const jeandle::VMCallbacks *Callbacks = jeandle::getVMCallbacks();
  assert(Callbacks && Callbacks->GetJavaMethodName &&
         Callbacks->GetProfileDevirtInfo &&
         Callbacks->GetNewStatepointID &&
         Callbacks->UpdateToStaticOptVirtualCall && "VMCallbacks must be set");

  DominatorTree &DT = FAM.getResult<DominatorTreeAnalysis>(F);
  DomTreeUpdater DTU(DT, DomTreeUpdater::UpdateStrategy::Lazy);

  SmallVector<InvokeInst *, 16> Calls;
  for (Instruction &I : instructions(F)) {
    if (auto *CB = dyn_cast<InvokeInst>(&I))
      Calls.push_back(CB);
  }

  bool Changed = false;
  uintptr_t Caller = 0;
  PatchSize =
      PatchSize == 0 ? getStaticCallPatchSize(*F.getParent()) : PatchSize;
  getFunctionJavaMethod(F, Caller);
  for (InvokeInst *CB : Calls)
    Changed |= optimizeCallSite(*CB, DTU, *Callbacks, Caller, PatchSize,
                                InlineScopeCallers);

  if (!Changed)
    return PreservedAnalyses::all();

  return PreservedAnalyses::none();
}
