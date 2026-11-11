
@0 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1

declare i32 @printf(ptr, ...)

; Function Attrs: nounwind
define noundef i32 @cmp(i32 noundef %a, i32 noundef %b) #0 {
entry:
  %b2 = alloca i32, align 4
  %a1 = alloca i32, align 4
  store i32 %a, ptr %a1, align 4
  store i32 %b, ptr %b2, align 4
  %0 = load i32, ptr %a1, align 4
  %1 = load i32, ptr %b2, align 4
  %2 = icmp slt i32 %0, %1
  br i1 %2, label %then, label %merge

then:                                             ; preds = %entry
  ret i32 -1

merge:                                            ; preds = %entry
  ret i32 1
}

define noundef i32 @main() {
entry:
  %0 = call i32 @pick_int(i32 3, i32 7)
  %1 = call i32 (ptr, ...) @printf(ptr @0, i32 %0)
  ret i32 0
}

; Function Attrs: nounwind
define noundef i32 @pick_int(i32 noundef %a, i32 noundef %b) #0 {
entry:
  %b2 = alloca i32, align 4
  %a1 = alloca i32, align 4
  store i32 %a, ptr %a1, align 4
  store i32 %b, ptr %b2, align 4
  %0 = load i32, ptr %a1, align 4
  %1 = load i32, ptr %b2, align 4
  %2 = call i32 @cmp(i32 %0, i32 %1)
  %3 = icmp slt i32 %2, 0
  br i1 %3, label %then, label %merge

then:                                             ; preds = %entry
  %4 = load i32, ptr %a1, align 4
  ret i32 %4

merge:                                            ; preds = %entry
  %5 = load i32, ptr %b2, align 4
  ret i32 %5
}

attributes #0 = { nounwind }
