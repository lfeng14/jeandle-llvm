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
#include "llvm/Analysis/CallGraph.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Jeandle/Attributes.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <queue>
#include <set>
#include <utility>
using namespace llvm;

#define DEBUG_TYPE "java-operation-lower"

namespace {

bool isPhaseFunc(Function *F, int Phase) {
  if (!F->hasFnAttribute(jeandle::Attribute::LowerPhase))
    return false;
  int V = 0;
  bool Failed = F->getFnAttribute(jeandle::Attribute::LowerPhase)
                    .getValueAsString()
                    .getAsInteger(10, V);
  assert(!Failed && "wrong value of LowerPhase attribute");
  return V == Phase;
};

void buildTopoSortMaps(
    CallGraph &CG, DenseMap<const Function *, int> &InDegree,
    DenseMap<const Function *,
             DenseMap<const Function *, SmallVector<CallBase *>>>
        &AdjacencyList) {
  // Initialization: Set in-degree of all functions to 0 initially
  for (auto &CGNPair : CG) {
    const Function *Func = CGNPair.first;
    if (Func && !Func->isDeclaration()) {
      InDegree[Func] = 0;
    }
  }

  for (auto &CGNPair : CG) {
    const Function *CallerFunc = CGNPair.first;
    CallGraphNode *CallerNode = CGNPair.second.get();

    if (!CallerFunc || CallerFunc->isDeclaration())
      continue;

    for (auto &Edge : *CallerNode) {
      CallGraphNode *CalleeNode = Edge.second;
      const Function *CalleeFunc =
          CalleeNode ? CalleeNode->getFunction() : nullptr;

      if (!CalleeFunc || CalleeFunc->isDeclaration())
        continue;

      if (Edge.first.has_value()) {
        Value *CallVal = static_cast<Value *>(Edge.first.value());
        if (CallBase *CB = dyn_cast<CallBase>(CallVal)) {
          InDegree[CallerFunc]++;
          AdjacencyList[CalleeFunc][CallerFunc].push_back(CB);
        }
      }
    }
  }
}

bool BottomUpInliner(
    DenseMap<const Function *, int> &InDegree,
    DenseMap<const Function *,
             DenseMap<const Function *, SmallVector<CallBase *>>>
        &AdjacencyList,
    int Phase) {
  int NumFuncs = InDegree.size();
  int Cnt = 0;
  bool LocalChanged = false;

  std::queue<const Function *> Que;

  for (const auto &Pair : InDegree) {
    const Function *Func = Pair.first;
    int Value = Pair.second;
    if (Value == 0) {
      Que.push(Func);
    }
  }

  if (Que.empty())
    return false;

  while (!Que.empty()) {
    const Function *Callee = Que.front();
    Que.pop();
    Cnt++;

    auto It = AdjacencyList.find(Callee);
    if (It == AdjacencyList.end())
      continue;

    const auto &CallerMap = It->second;
    for (const auto &CallerPair : CallerMap) {
      const Function *Caller = CallerPair.first;
      const auto &CallBases = CallerPair.second;

      InlineFunctionInfo IFI;
      for (CallBase *CB : CallBases) {
        if (!CB)
          continue;
        InDegree[Caller]--;
        if (InDegree[Caller] == 0) {
          Que.push(Caller);
        }
        Function *CalledFunc = CB->getCalledFunction();
        if (!CalledFunc || CalledFunc != Callee)
          continue;

        if (!isPhaseFunc(CalledFunc, Phase)) {
          continue;
        }
        // Execute inlining
        InlineResult IR = InlineFunction(*CB, IFI);
        if (IR.isSuccess()) {
          LocalChanged = true;
          LLVM_DEBUG(dbgs() << "Successfully inlined: " << Callee->getName()
                            << " into " << Caller->getName()
                            << " in lower phase: " << Phase << "\n");
        } else {
          LLVM_DEBUG(dbgs()
                     << "Failed to inline: " << Callee->getName() << " into "
                     << Caller->getName() << " in lower phase: " << Phase
                     << " reason: " << IR.getFailureReason() << "\n");
        }
      }
    }
  }

  if (Cnt != NumFuncs) {
    LLVM_DEBUG(dbgs() << "Call cycle detected\n");
  } else {
    LLVM_DEBUG(dbgs() << "No call cycle detected\n");
  }

  return LocalChanged;
}

static bool runImpl(Module &M, int Phase, ModuleAnalysisManager &MAM) {
  bool Changed = false;
  auto &CG = MAM.getResult<CallGraphAnalysis>(M);
  DenseMap<const Function *,
           DenseMap<const Function *, SmallVector<CallBase *>>>
      AdjacencyList;
  DenseMap<const Function *, int> InDegree;
  SmallVector<Function *> FunctionsToRemove;

  buildTopoSortMaps(CG, InDegree, AdjacencyList);

  Changed |= BottomUpInliner(InDegree, AdjacencyList, Phase);

  for (Function &F : M) {
    Function *Func = &F;
    // InDegree[Func] > 0 indicates loop existence
    if (Func->user_empty() && isPhaseFunc(Func, Phase)) {
      FunctionsToRemove.push_back(Func);
    }
  }

  // Delete marked functions and update modification status
  for (Function *F : FunctionsToRemove) {
    LLVM_DEBUG(dbgs() << "Remove unused function: " << F->getName()
                      << " in lower phase: " << Phase << "\n");
    F->eraseFromParent();
    Changed = true;
  }

  return Changed;
}

} // end anonymous namespace

namespace llvm {

PreservedAnalyses JavaOperationLower::run(Module &M,
                                          ModuleAnalysisManager &MAM) {
  if (!runImpl(M, Phase, MAM))
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

} // end namespace llvm
