
@0 = private unnamed_addr constant [10 x i8] c"%d %d %d\0A\00", align 1

declare i32 @printf(ptr, ...)

; Function Attrs: nounwind
define noundef i32 @sum(ptr noundef %p, i32 noundef %n) #0 {
entry:
  %__end_i = alloca i32, align 4
  %i = alloca i32, align 4
  %s = alloca i32, align 4
  %n2 = alloca i32, align 4
  %p1 = alloca ptr, align 8
  store ptr %p, ptr %p1, align 8
  store i32 %n, ptr %n2, align 4
  store i32 0, ptr %s, align 4
  store i32 0, ptr %i, align 4
  %0 = load i32, ptr %n2, align 4
  store i32 %0, ptr %__end_i, align 4
  br label %for

for:                                              ; preds = %for_step, %entry
  %1 = load i32, ptr %i, align 4
  %2 = load i32, ptr %__end_i, align 4
  %3 = icmp slt i32 %1, %2
  br i1 %3, label %for_body, label %for_exit

for_body:                                         ; preds = %for
  %4 = load i32, ptr %s, align 4
  %5 = load i32, ptr %i, align 4
  %6 = sext i32 %5 to i64
  %7 = load ptr, ptr %p1, align 8
  %8 = getelementptr i32, ptr %7, i64 %6
  %9 = load i32, ptr %8, align 4
  %10 = add i32 %4, %9
  store i32 %10, ptr %s, align 4
  br label %for_step

for_step:                                         ; preds = %for_body
  %11 = load i32, ptr %i, align 4
  %12 = add i32 %11, 1
  store i32 %12, ptr %i, align 4
  br label %for

for_exit:                                         ; preds = %for
  %13 = load i32, ptr %s, align 4
  ret i32 %13
}

define noundef i32 @main() {
entry:
  %cp = alloca ptr, align 8
  %total = alloca i32, align 4
  %first = alloca i32, align 4
  %q = alloca ptr, align 8
  %r = alloca ptr, align 8
  %a = alloca [3 x i32], align 4
  store [3 x i32] zeroinitializer, ptr %a, align 4
  %0 = getelementptr [3 x i32], ptr %a, i64 0, i64 0
  store i32 1, ptr %0, align 4
  %1 = getelementptr [3 x i32], ptr %a, i64 0, i64 1
  store i32 2, ptr %1, align 4
  %2 = getelementptr [3 x i32], ptr %a, i64 0, i64 2
  store i32 3, ptr %2, align 4
  %3 = getelementptr [3 x i32], ptr %a, i64 0, i64 0
  store ptr %3, ptr %r, align 8
  %4 = getelementptr [3 x i32], ptr %a, i64 0, i64 0
  store ptr %4, ptr %q, align 8
  %5 = load ptr, ptr %q, align 8
  store ptr %5, ptr %r, align 8
  %6 = load ptr, ptr %r, align 8
  %7 = load i32, ptr %6, align 4
  store i32 %7, ptr %first, align 4
  %8 = load ptr, ptr %q, align 8
  store i32 10, ptr %8, align 4
  %9 = load ptr, ptr %q, align 8
  %10 = call i32 @sum(ptr %9, i32 3)
  store i32 %10, ptr %total, align 4
  %11 = getelementptr [3 x i32], ptr %a, i64 0, i64 1
  store ptr %11, ptr %cp, align 8
  %12 = load ptr, ptr %cp, align 8
  store i32 20, ptr %12, align 4
  %13 = load i32, ptr %first, align 4
  %14 = load i32, ptr %total, align 4
  %15 = load ptr, ptr %cp, align 8
  %16 = load i32, ptr %15, align 4
  %17 = call i32 (ptr, ...) @printf(ptr @0, i32 %13, i32 %14, i32 %16)
  ret i32 0
}

attributes #0 = { nounwind }
