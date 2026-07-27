//===- ProfileDevirtualization.h - Jeandle profile devirtualization ------===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_PROFILE_DEVIRTUALIZATION_H
#define LLVM_PROFILE_DEVIRTUALIZATION_H

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Jeandle/Deoptimization.h"
#include "llvm/IR/PassManager.h"

#include <array>
#include <limits>
#include <sstream>
#include <string>

namespace llvm {

namespace jeandle {

struct ProfileDevirtInfo {
  uintptr_t ReceiverKlass = 0;
  uintptr_t TargetMethod = 0;
  uint64_t Count = 0;
  uint64_t TotalCount = 0;
  Deoptimization::DeoptReason DeoptReason = Deoptimization::Reason_none;
  bool DeoptimizeOnMiss = false;
  uintptr_t ReceiverKlass2 = 0;
  uintptr_t TargetMethod2 = 0;
  uint64_t Count2 = 0;

  std::string encode() const {
    std::ostringstream Oss;
    Oss << ReceiverKlass << '#' << TargetMethod << '#' << Count << '#'
        << TotalCount << '#' << static_cast<int>(DeoptReason) << '#'
        << static_cast<int>(DeoptimizeOnMiss) << '#' << ReceiverKlass2 << '#'
        << Count2 << '#' << TargetMethod2;
    return Oss.str();
  }

  static ProfileDevirtInfo decode(const std::string &Encoding) {
    if (Encoding.empty())
      return {};

    // This is the first profile-devirtualization encoding, so accept exactly
    // its current schema rather than guessing old or partial formats.
    std::array<StringRef, 9> Fields;
    StringRef Rest(Encoding);
    for (unsigned I = 0; I + 1 < Fields.size(); ++I) {
      size_t Pos = Rest.find('#');
      if (Pos == StringRef::npos)
        return {};
      Fields[I] = Rest.take_front(Pos);
      Rest = Rest.drop_front(Pos + 1);
    }
    if (Rest.contains('#'))
      return {};
    Fields.back() = Rest;

    auto ParseUInt64 = [&](unsigned Index, uint64_t &Value) {
      return !Fields[Index].getAsInteger(10, Value);
    };
    auto ParseUIntPtr = [&](unsigned Index, uintptr_t &Value) {
      uint64_t Parsed = 0;
      if (!ParseUInt64(Index, Parsed) ||
          Parsed > std::numeric_limits<uintptr_t>::max())
        return false;
      Value = static_cast<uintptr_t>(Parsed);
      return true;
    };

    ProfileDevirtInfo Info;
    uint64_t DeoptReasonVal = 0;
    uint64_t DeoptimizeOnMissVal = 0;
    if (!ParseUIntPtr(0, Info.ReceiverKlass) ||
        !ParseUIntPtr(1, Info.TargetMethod) || !ParseUInt64(2, Info.Count) ||
        !ParseUInt64(3, Info.TotalCount) ||
        !ParseUInt64(4, DeoptReasonVal) ||
        !ParseUInt64(5, DeoptimizeOnMissVal) ||
        !ParseUIntPtr(6, Info.ReceiverKlass2) ||
        !ParseUInt64(7, Info.Count2) ||
        !ParseUIntPtr(8, Info.TargetMethod2))
      return {};
    if (DeoptReasonVal == Deoptimization::Reason_none ||
        DeoptReasonVal >= Deoptimization::Reason_LIMIT ||
        DeoptimizeOnMissVal > 1)
      return {};

    Info.DeoptReason =
        static_cast<Deoptimization::DeoptReason>(DeoptReasonVal);
    Info.DeoptimizeOnMiss = DeoptimizeOnMissVal != 0;
    return Info;
  }
};

} // namespace jeandle

class ProfileDevirtualization : public PassInfoMixin<ProfileDevirtualization> {
public:
  ProfileDevirtualization() {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);

private:
  int PatchSize = 0;
};

} // namespace llvm

#endif // LLVM_PROFILE_DEVIRTUALIZATION_H
