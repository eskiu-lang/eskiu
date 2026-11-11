
@0 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1

declare i32 @printf(ptr, ...)

define noundef i32 @main() {
entry:
  %y = alloca i8, align 1
  %__lambda0.fat = alloca { ptr, ptr }, align 8
  %f = alloca { ptr, ptr }, align 8
  %0 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda0.fat, i32 0, i32 0
  %1 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda0.fat, i32 0, i32 1
  store ptr @__lambda0, ptr %0, align 8
  store ptr null, ptr %1, align 8
  %__lambda0.fat.val = load { ptr, ptr }, ptr %__lambda0.fat, align 8
  store { ptr, ptr } %__lambda0.fat.val, ptr %f, align 8
  store i8 41, ptr %y, align 1
  %2 = load { ptr, ptr }, ptr %f, align 8
  %fn.ptr = extractvalue { ptr, ptr } %2, 0
  %env.ptr = extractvalue { ptr, ptr } %2, 1
  %3 = load i8, ptr %y, align 1
  %4 = zext i8 %3 to i32
  %fn.call = call i32 %fn.ptr(ptr %env.ptr, i32 %4)
  %5 = call i32 (ptr, ...) @printf(ptr @0, i32 %fn.call)
  ret i32 0
}

; Function Attrs: nounwind
define internal i32 @__lambda0(ptr %env, i32 %x) #0 {
entry:
  %x1 = alloca i32, align 4
  store i32 %x, ptr %x1, align 4
  %0 = load i32, ptr %x1, align 4
  %1 = add i32 %0, 1
  ret i32 %1
}

attributes #0 = { nounwind }
