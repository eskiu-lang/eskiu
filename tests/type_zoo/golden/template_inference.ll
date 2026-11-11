
@0 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@1 = private unnamed_addr constant [6 x i8] c"%.1f\0A\00", align 1
@2 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@3 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@4 = private unnamed_addr constant [6 x i8] c"%.2f\0A\00", align 1

declare i32 @printf(ptr, ...)

define noundef i32 @main() {
entry:
  %0 = call i32 @max_int(i32 3, i32 5)
  %1 = call i32 (ptr, ...) @printf(ptr @0, i32 %0)
  %2 = call double @max_double(double 1.500000e+00, double 2.500000e+00)
  %3 = call i32 (ptr, ...) @printf(ptr @1, double %2)
  %4 = call i32 @identity_int(i32 42)
  %5 = call i32 (ptr, ...) @printf(ptr @2, i32 %4)
  %6 = call i32 @max_int(i32 7, i32 2)
  %7 = call i32 (ptr, ...) @printf(ptr @3, i32 %6)
  %8 = call float @max_float(float 0x3FF3333340000000, float 0x3FE99999A0000000)
  %9 = fpext float %8 to double
  %10 = call i32 (ptr, ...) @printf(ptr @4, double %9)
  ret i32 0
}

; Function Attrs: nounwind
define noundef i32 @max_int(i32 noundef %a, i32 noundef %b) #0 {
entry:
  %b2 = alloca i32, align 4
  %a1 = alloca i32, align 4
  store i32 %a, ptr %a1, align 4
  store i32 %b, ptr %b2, align 4
  %0 = load i32, ptr %a1, align 4
  %1 = load i32, ptr %b2, align 4
  %2 = icmp sgt i32 %0, %1
  br i1 %2, label %then, label %merge

then:                                             ; preds = %entry
  %3 = load i32, ptr %a1, align 4
  ret i32 %3

merge:                                            ; preds = %entry
  %4 = load i32, ptr %b2, align 4
  ret i32 %4
}

; Function Attrs: nounwind
define noundef double @max_double(double noundef %a, double noundef %b) #0 {
entry:
  %b2 = alloca double, align 8
  %a1 = alloca double, align 8
  store double %a, ptr %a1, align 8
  store double %b, ptr %b2, align 8
  %0 = load double, ptr %a1, align 8
  %1 = load double, ptr %b2, align 8
  %2 = fcmp ogt double %0, %1
  br i1 %2, label %then, label %merge

then:                                             ; preds = %entry
  %3 = load double, ptr %a1, align 8
  ret double %3

merge:                                            ; preds = %entry
  %4 = load double, ptr %b2, align 8
  ret double %4
}

; Function Attrs: nounwind
define noundef i32 @identity_int(i32 noundef %x) #0 {
entry:
  %x1 = alloca i32, align 4
  store i32 %x, ptr %x1, align 4
  %0 = load i32, ptr %x1, align 4
  ret i32 %0
}

; Function Attrs: nounwind
define noundef float @max_float(float noundef %a, float noundef %b) #0 {
entry:
  %b2 = alloca float, align 4
  %a1 = alloca float, align 4
  store float %a, ptr %a1, align 4
  store float %b, ptr %b2, align 4
  %0 = load float, ptr %a1, align 4
  %1 = load float, ptr %b2, align 4
  %2 = fcmp ogt float %0, %1
  br i1 %2, label %then, label %merge

then:                                             ; preds = %entry
  %3 = load float, ptr %a1, align 4
  ret float %3

merge:                                            ; preds = %entry
  %4 = load float, ptr %b2, align 4
  ret float %4
}

attributes #0 = { nounwind }
