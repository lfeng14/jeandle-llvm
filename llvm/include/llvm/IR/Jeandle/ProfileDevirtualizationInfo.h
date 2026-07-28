//===- ProfileDevirtualizationInfo.h - Profile devirtualization ABI -------===//
//
// Copyright (c) 2026, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_IR_JEANDLE_PROFILEDEVIRTUALIZATIONINFO_H
#define LLVM_IR_JEANDLE_PROFILEDEVIRTUALIZATIONINFO_H

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Jeandle/Deoptimization.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>

namespace llvm::jeandle {

/// Runtime information returned by the VM for a profiled virtual call site.
///
/// The VM owns receiver-profile interpretation, target resolution, class
/// unloading bookkeeping, and speculation policy. LLVM owns the guarded IR
/// transformation and consumes only this self-contained result.
struct ProfileDevirtualizationInfo {
  uintptr_t ReceiverKlass = 0;
  uintptr_t TargetMethod = 0;
  uint64_t Count = 0;
  uint64_t TotalCount = 0;
  Deoptimization::DeoptReason DeoptReason = Deoptimization::Reason_none;
  bool DeoptimizeOnMiss = false;
  uintptr_t ReceiverKlass2 = 0;
  uintptr_t TargetMethod2 = 0;
  uint64_t Count2 = 0;
  std::string TargetMethodName;
  std::string TargetMethodName2;

  bool isValid() const {
    return ReceiverKlass != 0 && TargetMethod != 0 && !TargetMethodName.empty();
  }

  bool isBimorphic() const { return ReceiverKlass2 != 0; }

  // Numeric fields are '#' separated. Method names are length-prefixed so
  // unusual but legal JVM names cannot make the callback payload ambiguous.
  std::string encode() const {
    std::ostringstream OS;
    OS << ReceiverKlass << '#' << TargetMethod << '#' << Count << '#'
       << TotalCount << '#' << static_cast<int>(DeoptReason) << '#'
       << static_cast<int>(DeoptimizeOnMiss) << '#' << ReceiverKlass2 << '#'
       << TargetMethod2 << '#' << Count2 << '#' << TargetMethodName.size()
       << '#' << TargetMethodName << '#' << TargetMethodName2.size() << '#'
       << TargetMethodName2;
    return OS.str();
  }

  static ProfileDevirtualizationInfo decode(StringRef Encoding) {
    if (Encoding.empty())
      return {};

    std::array<StringRef, 9> NumericFields;
    StringRef Rest = Encoding;
    for (StringRef &Field : NumericFields) {
      size_t Pos = Rest.find('#');
      if (Pos == StringRef::npos)
        return {};
      Field = Rest.take_front(Pos);
      Rest = Rest.drop_front(Pos + 1);
    }

    auto ParseUInt64 = [&](unsigned Index, uint64_t &Value) {
      return !NumericFields[Index].getAsInteger(10, Value);
    };
    auto ParseUIntPtr = [&](unsigned Index, uintptr_t &Value) {
      uint64_t Parsed = 0;
      if (!ParseUInt64(Index, Parsed) ||
          Parsed > std::numeric_limits<uintptr_t>::max())
        return false;
      Value = static_cast<uintptr_t>(Parsed);
      return true;
    };
    auto ParseString = [&](std::string &Value, bool ExpectSeparator) {
      size_t LengthEnd = Rest.find('#');
      if (LengthEnd == StringRef::npos)
        return false;
      uint64_t Length = 0;
      if (Rest.take_front(LengthEnd).getAsInteger(10, Length) ||
          Length > std::numeric_limits<size_t>::max())
        return false;
      Rest = Rest.drop_front(LengthEnd + 1);
      if (Length > Rest.size())
        return false;
      Value = Rest.take_front(static_cast<size_t>(Length)).str();
      Rest = Rest.drop_front(static_cast<size_t>(Length));
      if (ExpectSeparator) {
        if (!Rest.consume_front("#"))
          return false;
      } else if (!Rest.empty()) {
        return false;
      }
      return true;
    };

    ProfileDevirtualizationInfo Info;
    uint64_t DeoptReasonValue = 0;
    uint64_t DeoptimizeOnMissValue = 0;
    if (!ParseUIntPtr(0, Info.ReceiverKlass) ||
        !ParseUIntPtr(1, Info.TargetMethod) || !ParseUInt64(2, Info.Count) ||
        !ParseUInt64(3, Info.TotalCount) || !ParseUInt64(4, DeoptReasonValue) ||
        !ParseUInt64(5, DeoptimizeOnMissValue) ||
        !ParseUIntPtr(6, Info.ReceiverKlass2) ||
        !ParseUIntPtr(7, Info.TargetMethod2) || !ParseUInt64(8, Info.Count2) ||
        !ParseString(Info.TargetMethodName, true) ||
        !ParseString(Info.TargetMethodName2, false))
      return {};

    if (DeoptReasonValue == Deoptimization::Reason_none ||
        DeoptReasonValue >= Deoptimization::Reason_LIMIT ||
        DeoptimizeOnMissValue > 1)
      return {};

    Info.DeoptReason =
        static_cast<Deoptimization::DeoptReason>(DeoptReasonValue);
    Info.DeoptimizeOnMiss = DeoptimizeOnMissValue != 0;
    if (!Info.isValid() || (Info.isBimorphic() != (Info.TargetMethod2 != 0)) ||
        (Info.isBimorphic() != !Info.TargetMethodName2.empty()))
      return {};
    return Info;
  }
};

} // namespace llvm::jeandle

#endif // LLVM_IR_JEANDLE_PROFILEDEVIRTUALIZATIONINFO_H
