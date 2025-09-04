//===- JavaOperationLower.cpp - Lower Java Operations ---------------------===//
//
// Copyright (c) 2025, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Jeandle/JavaOperationLower.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Jeandle/Attributes.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/Analysis/InlineAdvisor.h"
#include "llvm/Analysis/InlineCost.h"
#include <utility>
#include <set>

using namespace llvm;

#define DEBUG_TYPE "java-operation-lower"

namespace {

bool isPhaseFunc(const Function &F, int Phase) {
  if (!F.hasFnAttribute(jeandle::Attribute::LowerPhase))
    return false;
  int V = 0;
  bool Failed = F.getFnAttribute(jeandle::Attribute::LowerPhase)
                    .getValueAsString()
                    .getAsInteger(10, V);
  assert(!Failed && "wrong value of LowerPhase attribute");
  return V == Phase;
};

static bool runImpl(Module &M, int Phase, ModuleAnalysisManager &MAM, ProfileSummaryInfo &PSI,
    FunctionAnalysisManager *FAM,
    function_ref<AssumptionCache &(Function &)> GetAssumptionCache) {

  SmallSetVector<CallBase *, 16> Calls;
  bool Changed = false;
  SmallVector<Function *, 16> InlinedComdatFunctions;

  for (Function &F : make_early_inc_range(M)) {
    if (F.isPresplitCoroutine())
      continue;

    if (!isPhaseFunc(F, Phase) || F.isDeclaration() || !isInlineViable(F).isSuccess())
      continue;

    Calls.clear();
    for (User *U : F.users())
      if (auto *CB = dyn_cast<CallBase>(U)) {
        if (CB->getCalledFunction() == &F &&
            !CB->getAttributes().hasFnAttr(Attribute::NoInline))
          Calls.insert(CB);
      }

    for (CallBase *CB : Calls) {
      Function *Caller = CB->getCaller();
      OptimizationRemarkEmitter ORE(Caller);
      DebugLoc DLoc = CB->getDebugLoc();
      BasicBlock *Block = CB->getParent();

      InlineFunctionInfo IFI(GetAssumptionCache, &PSI, nullptr, nullptr);
      InlineResult Res = InlineFunction(*CB, IFI);
      if (!Res.isSuccess()) {
        LLVM_DEBUG(dbgs() << "failed to inline: " << Caller->getName()
                          << " in lower phase: " << Phase << "\n");
        continue;
      }

      emitInlinedIntoBasedOnCost(
          ORE, DLoc, Block, F, *Caller,
          InlineCost::getAlways("always inline attribute"),
          /*ForProfileContext=*/false, DEBUG_TYPE);

      Changed = true;
      if (FAM)
        FAM->invalidate(*Caller, PreservedAnalyses::none());
    }

    F.removeDeadConstantUsers();
    // Remember to try and delete this function afterward. This allows to call
    // filterDeadComdatFunctions() only once.
    if (F.hasComdat()) {
      InlinedComdatFunctions.push_back(&F);
    } else {
      if (FAM)
        FAM->clear(F, F.getName());
      LLVM_DEBUG(dbgs() << "remove unused function: " << F->getName()
                  << " in lower phase: " << Phase << "\n");
      M.getFunctionList().erase(F);
      Changed = true;
    }
  }

  if (!InlinedComdatFunctions.empty()) {
    // Now we just have the comdat functions. Filter out the ones whose comdats
    // are not actually dead.
    filterDeadComdatFunctions(InlinedComdatFunctions);
    // The remaining functions are actually dead.
    for (Function *F : InlinedComdatFunctions) {
      if (FAM)
        FAM->clear(*F, F->getName());
      LLVM_DEBUG(dbgs() << "remove unused function: " << F->getName()
                  << " in lower phase: " << Phase << "\n");
      M.getFunctionList().erase(F);
      Changed = true;
    }
  }

  return Changed;
}

} // end anonymous namespace

namespace llvm {

PreservedAnalyses JavaOperationLower::run(Module &M,
                                          ModuleAnalysisManager &MAM) {
                                              FunctionAnalysisManager &FAM =
  MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  auto GetAssumptionCache = [&](Function &F) -> AssumptionCache & {
  return FAM.getResult<AssumptionAnalysis>(F);
  };
  auto &PSI = MAM.getResult<ProfileSummaryAnalysis>(M);

  if (!runImpl(M, Phase, MAM, PSI, &FAM, GetAssumptionCache))
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

} // end namespace llvm
