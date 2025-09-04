; RUN: llc -O0 -mtriple=x86_64-linux-gnu -filetype=obj -o call-site.o %s
; RUN: llvm-objdump -d call-site.o | FileCheck %s

define hotspotcc i32 @"Main_caller_()I"() local_unnamed_addr #0 gc "hotspotgc" {
; CHECK-LABEL:       1: be 01 00 00 00               	movl	$0x1, %esi
; CHECK-NEXT:        6: ba 02 00 00 00               	movl	$0x2, %edx
; CHECK-NEXT:        b: b9 03 00 00 00               	movl	$0x3, %ecx
; CHECK-NEXT:       10: 0f 1f 00                     	nopl	(%rax)
; CHECK-NEXT:       13: 0f 1f 44 00 08               	nopl	0x8(%rax,%rax)
; CHECK-NEXT:       18: 5d                           	popq	%rbp
; CHECK-NEXT:       19: c3                           	retq
entry:
  %statepoint_token = tail call hotspotcc token (i64, i32, ptr, i32, i32, ...) @llvm.experimental.gc.statepoint.p0(i64 0, i32 5, ptr elementtype(i32 (i32, i32, i32)) @"Main_callee_(III)I", i32 3, i32 0, i32 1, i32 2, i32 3, i32 0, i32 0)
  %0 = call i32 @llvm.experimental.gc.result.i32(token %statepoint_token)
  ret i32 %0
}

declare hotspotcc i32 @"Main_callee_(III)I"(i32, i32, i32) local_unnamed_addr #1 gc "hotspotgc"

declare token @llvm.experimental.gc.statepoint.p0(i64 immarg, i32 immarg, ptr, i32 immarg, i32 immarg, ...)

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(none)
declare i32 @llvm.experimental.gc.result.i32(token) #2

attributes #0 = { "java-method" "use-compressed-oops" }
attributes #1 = { "use-compressed-oops" }
attributes #2 = { nocallback nofree nosync nounwind willreturn memory(none) }
