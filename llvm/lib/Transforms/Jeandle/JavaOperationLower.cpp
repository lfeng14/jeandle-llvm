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
#include "llvm/ADT/SetVector.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/InlineAdvisor.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Jeandle/Attributes.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <set>
#include <utility>

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

static bool runImpl(Module &M, int Phase, ModuleAnalysisManager &MAM,
                    FunctionAnalysisManager *FAM) {

  bool Changed = false;
  SmallSetVector<CallBase *, 16> Calls;

  // Traverse in the order of function definitions in the code,
  // but there's no need to care about the traversal order
  // when finding call sites via F.users() for inlining
  for (Function &F : make_early_inc_range(M)) {
    if (F.isPresplitCoroutine())
      continue;

    if (!isPhaseFunc(F, Phase) || F.isDeclaration())
      continue;
    assert(isInlineViable(F).isSuccess() && "function is not inline viable");

    Calls.clear();
    for (User *U : F.users())
      if (auto *CB = dyn_cast<CallBase>(U)) {
        if (CB->getCalledFunction() == &F)
          Calls.insert(CB);
      }

    for (CallBase *CB : Calls) {
      Function *Caller = CB->getCaller();
      InlineFunctionInfo IFI;
      InlineResult Res = InlineFunction(*CB, IFI);
      if (!Res.isSuccess()) {
        LLVM_DEBUG(dbgs() << "failed to inline: " << Caller->getName()
                          << " in lower phase: " << Phase << "\n");
        continue;
      }

      Changed = true;
      if (FAM)
        FAM->invalidate(*Caller, PreservedAnalyses::none());
    }

    if (FAM)
      FAM->clear(F, F.getName());
    LLVM_DEBUG(dbgs() << "remove unused function: " << F.getName()
                      << " in lower phase: " << Phase << "\n");
    M.getFunctionList().erase(F);
    Changed = true;
  }

  return Changed;
}

} // end anonymous namespace

namespace llvm {

PreservedAnalyses JavaOperationLower::run(Module &M,
                                          ModuleAnalysisManager &MAM) {
  FunctionAnalysisManager &FAM =
      MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

  if (!runImpl(M, Phase, MAM, &FAM))
    return PreservedAnalyses::all();
  PreservedAnalyses PA;
  // We have already invalidated all analyses on modified functions.
  PA.preserveSet<AllAnalysesOn<Function>>();
  return PA;
}

} // end namespace llvm
