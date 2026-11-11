
%__va_list = type { ptr, ptr, ptr, i32, i32 }

@0 = private unnamed_addr constant [10 x i8] c"%d %d %d\0A\00", align 1
@1 = private unnamed_addr constant [6 x i8] c"%.1f\0A\00", align 1

declare i32 @printf(ptr, ...)

; Function Attrs: nounwind
define noundef i32 @sumv(i32 noundef %n, ...) #0 {
entry:
  %__end_i = alloca i32, align 4
  %i = alloca i32, align 4
  %total = alloca i32, align 4
  %ap = alloca %__va_list, align 8
  %n1 = alloca i32, align 4
  store i32 %n, ptr %n1, align 4
  store %__va_list zeroinitializer, ptr %ap, align 8
  call void @llvm.va_start.p0(ptr %ap)
  store i32 0, ptr %total, align 4
  store i32 0, ptr %i, align 4
  %0 = load i32, ptr %n1, align 4
  store i32 %0, ptr %__end_i, align 4
  br label %for

for:                                              ; preds = %for_step, %entry
  %1 = load i32, ptr %i, align 4
  %2 = load i32, ptr %__end_i, align 4
  %3 = icmp slt i32 %1, %2
  br i1 %3, label %for_body, label %for_exit

for_body:                                         ; preds = %for
  %4 = load i32, ptr %total, align 4
  %va.arg = va_arg ptr %ap, i32
  %5 = add i32 %4, %va.arg
  store i32 %5, ptr %total, align 4
  br label %for_step

for_step:                                         ; preds = %for_body
  %6 = load i32, ptr %i, align 4
  %7 = add i32 %6, 1
  store i32 %7, ptr %i, align 4
  br label %for

for_exit:                                         ; preds = %for
  call void @llvm.va_end.p0(ptr %ap)
  %8 = load i32, ptr %total, align 4
  ret i32 %8
}

; Function Attrs: nounwind
define noundef double @avg(i32 noundef %n, ...) #0 {
entry:
  %__end_i = alloca i32, align 4
  %i = alloca i32, align 4
  %s = alloca double, align 8
  %ap = alloca %__va_list, align 8
  %n1 = alloca i32, align 4
  store i32 %n, ptr %n1, align 4
  store %__va_list zeroinitializer, ptr %ap, align 8
  call void @llvm.va_start.p0(ptr %ap)
  store double 0.000000e+00, ptr %s, align 8
  store i32 0, ptr %i, align 4
  %0 = load i32, ptr %n1, align 4
  store i32 %0, ptr %__end_i, align 4
  br label %for

for:                                              ; preds = %for_step, %entry
  %1 = load i32, ptr %i, align 4
  %2 = load i32, ptr %__end_i, align 4
  %3 = icmp slt i32 %1, %2
  br i1 %3, label %for_body, label %for_exit

for_body:                                         ; preds = %for
  %4 = load double, ptr %s, align 8
  %va.arg = va_arg ptr %ap, double
  %5 = fadd double %4, %va.arg
  store double %5, ptr %s, align 8
  br label %for_step

for_step:                                         ; preds = %for_body
  %6 = load i32, ptr %i, align 4
  %7 = add i32 %6, 1
  store i32 %7, ptr %i, align 4
  br label %for

for_exit:                                         ; preds = %for
  call void @llvm.va_end.p0(ptr %ap)
  %8 = load double, ptr %s, align 8
  %9 = load i32, ptr %n1, align 4
  %10 = sitofp i32 %9 to double
  %11 = fdiv double %8, %10
  ret double %11
}

define noundef i32 @main() {
entry:
  %0 = call i32 (i32, ...) @sumv(i32 3, i32 10, i32 20, i32 30)
  %1 = call i32 (i32, ...) @sumv(i32 4, i32 1, i32 2, i32 3, i32 4)
  %2 = call i32 (i32, ...) @sumv(i32 0)
  %3 = call i32 (ptr, ...) @printf(ptr @0, i32 %0, i32 %1, i32 %2)
  %4 = call double (i32, ...) @avg(i32 4, double 2.000000e+00, double 4.000000e+00, double 6.000000e+00, double 8.000000e+00)
  %5 = call i32 (ptr, ...) @printf(ptr @1, double %4)
  ret i32 0
}

; Function Attrs: nocallback nofree nosync nounwind willreturn
declare void @llvm.va_start.p0(ptr) #1

; Function Attrs: nocallback nofree nosync nounwind willreturn
declare void @llvm.va_end.p0(ptr) #1

attributes #0 = { nounwind }
attributes #1 = { nocallback nofree nosync nounwind willreturn }
