
; Function Attrs: nounwind
define noundef i32 @pick(i32 noundef %a, i32 noundef %b) #0 {
entry:
  %b2 = alloca i32, align 4
  %a1 = alloca i32, align 4
  store i32 %a, ptr %a1, align 4
  store i32 %b, ptr %b2, align 4
  %0 = load i32, ptr %a1, align 4
  %1 = load i32, ptr %b2, align 4
  %2 = icmp sgt i32 %0, %1
  br i1 %2, label %tern.then, label %tern.else

tern.then:                                        ; preds = %entry
  %3 = load i32, ptr %a1, align 4
  br label %tern.cont

tern.else:                                        ; preds = %entry
  %4 = load i32, ptr %b2, align 4
  br label %tern.cont

tern.cont:                                        ; preds = %tern.else, %tern.then
  %5 = phi i32 [ %3, %tern.then ], [ %4, %tern.else ]
  ret i32 %5
}

; Function Attrs: nounwind
define noundef double @promote(i32 noundef %flag, i32 noundef %a, double noundef %b) #0 {
entry:
  %b3 = alloca double, align 8
  %a2 = alloca i32, align 4
  %flag1 = alloca i32, align 4
  store i32 %flag, ptr %flag1, align 4
  store i32 %a, ptr %a2, align 4
  store double %b, ptr %b3, align 8
  %0 = load i32, ptr %flag1, align 4
  %1 = icmp ne i32 %0, 0
  br i1 %1, label %tern.then, label %tern.else

tern.then:                                        ; preds = %entry
  %2 = load i32, ptr %a2, align 4
  %3 = sitofp i32 %2 to double
  br label %tern.cont

tern.else:                                        ; preds = %entry
  %4 = load double, ptr %b3, align 8
  br label %tern.cont

tern.cont:                                        ; preds = %tern.else, %tern.then
  %5 = phi double [ %3, %tern.then ], [ %4, %tern.else ]
  ret double %5
}

; Function Attrs: nounwind
define noundef i32 @main() #0 {
entry:
  %0 = call i32 @pick(i32 3, i32 7)
  %1 = sub i32 %0, 7
  ret i32 %1
}

attributes #0 = { nounwind }
