
%Big = type { i32, i32, i32, i32, i32 }

@0 = private unnamed_addr constant [10 x i8] c"%d %d %d\0A\00", align 1

declare i32 @printf(ptr, ...)

; Function Attrs: nounwind
define void @make(ptr %sret.ptr) #0 {
entry:
  %x = alloca %Big, align 8
  store %Big zeroinitializer, ptr %x, align 4
  %0 = getelementptr inbounds nuw %Big, ptr %x, i32 0, i32 0
  store i32 1, ptr %0, align 4
  %1 = getelementptr inbounds nuw %Big, ptr %x, i32 0, i32 1
  store i32 2, ptr %1, align 4
  %2 = getelementptr inbounds nuw %Big, ptr %x, i32 0, i32 2
  store i32 3, ptr %2, align 4
  %3 = getelementptr inbounds nuw %Big, ptr %x, i32 0, i32 3
  store i32 4, ptr %3, align 4
  %4 = getelementptr inbounds nuw %Big, ptr %x, i32 0, i32 4
  store i32 5, ptr %4, align 4
  %5 = load %Big, ptr %x, align 4
  store %Big %5, ptr %sret.ptr, align 4
  ret void
}

define noundef i32 @main() {
entry:
  %sret.tmp = alloca %Big, align 8
  %g = alloca %Big, align 8
  call void @make(ptr %sret.tmp)
  %0 = load %Big, ptr %sret.tmp, align 4
  store %Big %0, ptr %g, align 4
  %a = getelementptr inbounds nuw %Big, ptr %g, i32 0, i32 0
  %a1 = load i32, ptr %a, align 4
  %c = getelementptr inbounds nuw %Big, ptr %g, i32 0, i32 2
  %c2 = load i32, ptr %c, align 4
  %e = getelementptr inbounds nuw %Big, ptr %g, i32 0, i32 4
  %e3 = load i32, ptr %e, align 4
  %1 = call i32 (ptr, ...) @printf(ptr @0, i32 %a1, i32 %c2, i32 %e3)
  ret i32 0
}

attributes #0 = { nounwind }
