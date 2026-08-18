//===- ProfileDevirtualization.h - Jeandle profile devirtualization ------===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_JEANDLE_PROFILEDEVIRTUALIZATION_H
#define LLVM_TRANSFORMS_JEANDLE_PROFILEDEVIRTUALIZATION_H

#include "llvm/IR/PassManager.h"

namespace llvm {

/// Uses VM-owned receiver profiles to guard Java virtual calls and replace
/// their hot paths with direct optimized-virtual calls.
class ProfileDevirtualization : public PassInfoMixin<ProfileDevirtualization> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_JEANDLE_PROFILEDEVIRTUALIZATION_H
