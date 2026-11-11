
%N = type { i32 }

@0 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1

declare i32 @printf(ptr, ...)

; Function Attrs: nounwind
define noundef i32 @N_show(ptr noundef %self) #0 {
entry:
  %self1 = alloca ptr, align 8
  store ptr %self, ptr %self1, align 8
  %0 = load ptr, ptr %self1, align 8
  %v = getelementptr inbounds nuw %N, ptr %0, i32 0, i32 0
  %v2 = load i32, ptr %v, align 4
  ret i32 %v2
}

; Function Attrs: nounwind
define noundef i32 @N_eq(ptr noundef %self, ptr noundef %o) #0 {
entry:
  %o2 = alloca ptr, align 8
  %self1 = alloca ptr, align 8
  store ptr %self, ptr %self1, align 8
  store ptr %o, ptr %o2, align 8
  %0 = load ptr, ptr %self1, align 8
  %v = getelementptr inbounds nuw %N, ptr %0, i32 0, i32 0
  %v3 = load i32, ptr %v, align 4
  %1 = load ptr, ptr %o2, align 8
  %v4 = getelementptr inbounds nuw %N, ptr %1, i32 0, i32 0
  %v5 = load i32, ptr %v4, align 4
  %2 = icmp eq i32 %v3, %v5
  %3 = zext i1 %2 to i32
  ret i32 %3
}

define noundef i32 @main() {
entry:
  %n = alloca %N, align 8
  store %N zeroinitializer, ptr %n, align 4
  %0 = getelementptr inbounds nuw %N, ptr %n, i32 0, i32 0
  store i32 5, ptr %0, align 4
  %1 = call i32 @dump_N(ptr %n)
  %2 = call i32 (ptr, ...) @printf(ptr @0, i32 %1)
  ret i32 0
}

; Function Attrs: nounwind
define noundef i32 @dump_N(ptr noundef %x) #0 {
entry:
  %x1 = alloca ptr, align 8
  store ptr %x, ptr %x1, align 8
  %0 = load ptr, ptr %x1, align 8
  %1 = call i32 @N_show(ptr %0)
  ret i32 %1
}

attributes #0 = { nounwind }
