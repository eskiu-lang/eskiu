
; Function Attrs: nounwind
define noundef i32 @grid() #0 {
entry:
  %a = alloca [2 x [3 x i32]], align 4
  %0 = getelementptr [2 x [3 x i32]], ptr %a, i32 0, i32 0
  %1 = getelementptr [3 x i32], ptr %0, i32 0, i32 0
  store i32 1, ptr %1, align 4
  %2 = getelementptr [3 x i32], ptr %0, i32 0, i32 1
  store i32 2, ptr %2, align 4
  %3 = getelementptr [3 x i32], ptr %0, i32 0, i32 2
  store i32 3, ptr %3, align 4
  %4 = getelementptr [2 x [3 x i32]], ptr %a, i32 0, i32 1
  %5 = getelementptr [3 x i32], ptr %4, i32 0, i32 0
  store i32 4, ptr %5, align 4
  %6 = getelementptr [3 x i32], ptr %4, i32 0, i32 1
  store i32 5, ptr %6, align 4
  %7 = getelementptr [3 x i32], ptr %4, i32 0, i32 2
  store i32 6, ptr %7, align 4
  %8 = getelementptr [2 x [3 x i32]], ptr %a, i64 0, i64 1
  %9 = getelementptr [3 x i32], ptr %8, i64 0, i64 2
  %10 = load i32, ptr %9, align 4
  ret i32 %10
}

; Function Attrs: nounwind
define noundef i32 @col() #0 {
entry:
  %b = alloca [3 x [2 x i32]], align 4
  store [3 x [2 x i32]] zeroinitializer, ptr %b, align 4
  %0 = getelementptr [3 x [2 x i32]], ptr %b, i64 0, i64 0
  %1 = getelementptr [2 x i32], ptr %0, i64 0, i64 0
  store i32 7, ptr %1, align 4
  %2 = getelementptr [3 x [2 x i32]], ptr %b, i64 0, i64 2
  %3 = getelementptr [2 x i32], ptr %2, i64 0, i64 1
  store i32 9, ptr %3, align 4
  %4 = getelementptr [3 x [2 x i32]], ptr %b, i64 0, i64 2
  %5 = getelementptr [2 x i32], ptr %4, i64 0, i64 1
  %6 = load i32, ptr %5, align 4
  %7 = getelementptr [3 x [2 x i32]], ptr %b, i64 0, i64 0
  %8 = getelementptr [2 x i32], ptr %7, i64 0, i64 0
  %9 = load i32, ptr %8, align 4
  %10 = add i32 %6, %9
  ret i32 %10
}

; Function Attrs: nounwind
define noundef i32 @main() #0 {
entry:
  %0 = call i32 @grid()
  %1 = call i32 @col()
  %2 = add i32 %0, %1
  %3 = sub i32 %2, 22
  ret i32 %3
}

attributes #0 = { nounwind }
