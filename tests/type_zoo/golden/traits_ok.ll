
%Num = type { i32 }
%Box_Num = type { %Num }

@0 = private unnamed_addr constant [7 x i8] c"%d %d\0A\00", align 1

declare i32 @printf(ptr, ...)

; Function Attrs: nounwind
define noundef i32 @Num_cmp(ptr noundef %self, ptr noundef %o) #0 {
entry:
  %o2 = alloca ptr, align 8
  %self1 = alloca ptr, align 8
  store ptr %self, ptr %self1, align 8
  store ptr %o, ptr %o2, align 8
  %0 = load ptr, ptr %self1, align 8
  %v = getelementptr inbounds nuw %Num, ptr %0, i32 0, i32 0
  %v3 = load i32, ptr %v, align 4
  %1 = load ptr, ptr %o2, align 8
  %v4 = getelementptr inbounds nuw %Num, ptr %1, i32 0, i32 0
  %v5 = load i32, ptr %v4, align 4
  %2 = icmp slt i32 %v3, %v5
  br i1 %2, label %then, label %merge

then:                                             ; preds = %entry
  ret i32 -1

merge:                                            ; preds = %entry
  ret i32 1
}

define noundef i32 @main() {
entry:
  %b = alloca %Box_Num, align 8
  %lo = alloca %Num, align 8
  %y = alloca %Num, align 8
  %x = alloca %Num, align 8
  store %Num zeroinitializer, ptr %x, align 4
  %0 = getelementptr inbounds nuw %Num, ptr %x, i32 0, i32 0
  store i32 3, ptr %0, align 4
  store %Num zeroinitializer, ptr %y, align 4
  %1 = getelementptr inbounds nuw %Num, ptr %y, i32 0, i32 0
  store i32 7, ptr %1, align 4
  %2 = call %Num @pick_Num(ptr %x, ptr %y)
  store %Num %2, ptr %lo, align 4
  store %Box_Num zeroinitializer, ptr %b, align 4
  %3 = getelementptr inbounds nuw %Box_Num, ptr %b, i32 0, i32 0
  %4 = getelementptr inbounds nuw %Num, ptr %3, i32 0, i32 0
  store i32 42, ptr %4, align 4
  %v = getelementptr inbounds nuw %Num, ptr %lo, i32 0, i32 0
  %v1 = load i32, ptr %v, align 4
  %5 = getelementptr inbounds nuw %Box_Num, ptr %b, i32 0, i32 0
  %v2 = getelementptr inbounds nuw %Num, ptr %5, i32 0, i32 0
  %v3 = load i32, ptr %v2, align 4
  %6 = call i32 (ptr, ...) @printf(ptr @0, i32 %v1, i32 %v3)
  ret i32 0
}

; Function Attrs: nounwind
define %Num @pick_Num(ptr noundef %a, ptr noundef %b) #0 {
entry:
  %b2 = alloca ptr, align 8
  %a1 = alloca ptr, align 8
  store ptr %a, ptr %a1, align 8
  store ptr %b, ptr %b2, align 8
  %0 = load ptr, ptr %a1, align 8
  %1 = load ptr, ptr %b2, align 8
  %2 = call i32 @Num_cmp(ptr %0, ptr %1)
  %3 = icmp slt i32 %2, 0
  br i1 %3, label %then, label %merge

then:                                             ; preds = %entry
  %4 = load ptr, ptr %a1, align 8
  %5 = getelementptr %Num, ptr %4, i64 0
  %6 = load %Num, ptr %5, align 4
  ret %Num %6

merge:                                            ; preds = %entry
  %7 = load ptr, ptr %b2, align 8
  %8 = getelementptr %Num, ptr %7, i64 0
  %9 = load %Num, ptr %8, align 4
  ret %Num %9
}

attributes #0 = { nounwind }
