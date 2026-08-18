; RUN: opt -S -passes="function(cha-devirtualization),default<O3>" -jeandle-vm-callback-log=%S/Inputs/virtual-call.cblog %s 2>&1 | FileCheck %s

; A recursive virtual call whose unique CHA target is the root function itself,
; and whose formal receiver is unused in the root body. The recursive invoke
; passes a different oop (loaded from @other_recv) so the formal receiver
; parameter %recv of @caller.root is dead to LLVM.
;
; The root function is named "caller.root" while CHA devirt uses the unsuffixed
; "caller" definition below. Its formal receiver is unused, but the opt-virtual
; runtime stub still needs the receiver before the direct target is patched.
; DeadArgumentElimination must therefore preserve the runtime-live operand.

@jeandle.personality = global ptr null
@other_recv = external global ptr addrspace(1)

; MethodType(0, T_METADATA) encoding used by the current inlinee layout.
; Keep this in sync with DeoptValueEncoding.
; METHOD_ENCODING = 393233

declare hotspotcc i1 @jeandle.check_instanceof(ptr, ptr addrspace(1))
declare hotspotcc i32 @Virtual_target(ptr addrspace(1)) #1 gc "hotspotgc"
declare void @opaque_side_effect()

define hotspotcc i32 @caller(ptr addrspace(1) %unused) #3 gc "hotspotgc" {
entry:
  call void @opaque_side_effect()
  ret i32 1
}

define hotspotcc i32 @caller.root(ptr addrspace(1) "java-klass"="500" %recv) #0 gc "hotspotgc" personality ptr @jeandle.personality {
entry:
  %other = load ptr addrspace(1), ptr @other_recv, !java-klass !0
  ; Root scope followed by a current-layout inlinee scope:
  ; method marker, method, should-reexecute, duplicated bci.
  %ret = invoke hotspotcc i32 @Virtual_target(ptr addrspace(1) %other) #2 [ "deopt"(i64 0, i32 0, i32 0, i64 393233, i64 100, i64 0, i32 7, i32 7) ]
          to label %normal unwind label %unwind

normal:
  ret i32 %ret

unwind:
  %lp = landingpad i64
          cleanup
  ret i32 -1
}

; CHECK-LABEL: define hotspotcc i32 @caller.root(
; CHECK: call hotspotcc i1 @jeandle.check_instanceof(ptr{{.*}}inttoptr (i64 600 to ptr), ptr addrspace(1) %other)
; CHECK: br i1
; CHECK-LABEL: cha_bci_7_check_receiver_fail:
; CHECK: call hotspotcc i32 (...) @llvm.experimental.deoptimize.i32(i32 -201)
; CHECK: ret i32
; CHECK-LABEL: cha_bci_7_check_receiver_pass:
; CHECK: invoke hotspotcc i32 @caller(ptr addrspace(1) noundef "runtime-live" %other) #[[CALLATTR:[0-9]+]]
; CHECK-SAME: [ "deopt"(
; CHECK-NOT: poison
; CHECK: attributes #[[CALLATTR]] = { {{.*}}"monomorphic-target"{{.*}}"statepoint-num-patch-bytes"="5"{{.*}} }

attributes #0 = { "java-method"="100" }
attributes #1 = { "java-method"="200" }
attributes #2 = { "bytecode"="invokevirtual" "call-site"="900" "declared-holder"="300" "statepoint-id"="42" "statepoint-num-patch-bytes"="15" }
attributes #3 = { noinline "java-method"="100" }

!java-method-compilation = !{}
!static-call-patch-size = !{!1}
!dynamic-call-patch-size = !{!2}

!0 = !{i64 500}
!1 = !{i32 5}
!2 = !{i32 15}
