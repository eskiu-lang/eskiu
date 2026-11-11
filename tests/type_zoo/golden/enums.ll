
@0 = private unnamed_addr constant [10 x i8] c"%d %d %d\0A\00", align 1
@1 = private unnamed_addr constant [10 x i8] c"%d %d %d\0A\00", align 1
@2 = private unnamed_addr constant [6 x i8] c"c=%d\0A\00", align 1
@3 = private unnamed_addr constant [18 x i8] c"classify(Red)=%d\0A\00", align 1
@4 = private unnamed_addr constant [19 x i8] c"classify(Blue)=%d\0A\00", align 1
@5 = private unnamed_addr constant [5 x i8] c"red\0A\00", align 1
@6 = private unnamed_addr constant [7 x i8] c"green\0A\00", align 1
@7 = private unnamed_addr constant [7 x i8] c"other\0A\00", align 1

declare i32 @printf(ptr, ...)

; Function Attrs: nounwind
define noundef i32 @classify(i32 noundef %c) #0 {
entry:
  %c1 = alloca i32, align 4
  store i32 %c, ptr %c1, align 4
  %0 = load i32, ptr %c1, align 4
  %1 = icmp eq i32 %0, 0
  br i1 %1, label %then, label %merge

then:                                             ; preds = %entry
  ret i32 100

merge:                                            ; preds = %entry
  ret i32 200
}

define noundef i32 @main() {
entry:
  %c = alloca i32, align 4
  %0 = call i32 (ptr, ...) @printf(ptr @0, i32 0, i32 1, i32 2)
  %1 = call i32 (ptr, ...) @printf(ptr @1, i32 0, i32 2, i32 3)
  store i32 1, ptr %c, align 4
  %2 = load i32, ptr %c, align 4
  %3 = call i32 (ptr, ...) @printf(ptr @2, i32 %2)
  %4 = call i32 @classify(i32 0)
  %5 = call i32 (ptr, ...) @printf(ptr @3, i32 %4)
  %6 = call i32 @classify(i32 2)
  %7 = call i32 (ptr, ...) @printf(ptr @4, i32 %6)
  %8 = load i32, ptr %c, align 4
  switch i32 %8, label %default [
    i32 0, label %case0
    i32 1, label %case1
  ]

switch.end:                                       ; preds = %default, %case1, %case0
  ret i32 0

case0:                                            ; preds = %entry
  %9 = call i32 (ptr, ...) @printf(ptr @5)
  br label %switch.end

case1:                                            ; preds = %entry
  %10 = call i32 (ptr, ...) @printf(ptr @6)
  br label %switch.end

default:                                          ; preds = %entry
  %11 = call i32 (ptr, ...) @printf(ptr @7)
  br label %switch.end
}

attributes #0 = { nounwind }
