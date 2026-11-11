
%Expr = type { i32, [1 x i64] }

@0 = private unnamed_addr constant [13 x i8] c"%d %d %d %d\0A\00", align 1
@1 = private unnamed_addr constant [10 x i8] c"%d %d %d\0A\00", align 1
@2 = private unnamed_addr constant [10 x i8] c"%d %d %d\0A\00", align 1

declare i32 @printf(ptr, ...)

; Function Attrs: nounwind
define noundef i32 @eval(%Expr %e) #0 {
entry:
  %n8 = alloca i32, align 4
  %b7 = alloca i32, align 4
  %a6 = alloca i32, align 4
  %v5 = alloca i32, align 4
  %match.subj = alloca %Expr, align 8
  %e1 = alloca %Expr, align 8
  store %Expr %e, ptr %e1, align 8
  %0 = load %Expr, ptr %e1, align 8
  store %Expr %0, ptr %match.subj, align 8
  %1 = getelementptr inbounds nuw %Expr, ptr %match.subj, i32 0, i32 0
  %match.tag = load i32, ptr %1, align 4
  switch i32 %match.tag, label %match.end [
    i32 0, label %match.arm
    i32 1, label %match.arm2
    i32 2, label %match.arm3
    i32 3, label %match.arm4
  ]

match.end:                                        ; preds = %entry
  ret i32 -1

match.arm:                                        ; preds = %entry
  %2 = getelementptr inbounds nuw %Expr, ptr %match.subj, i32 0, i32 1
  %3 = getelementptr inbounds nuw { i32 }, ptr %2, i32 0, i32 0
  %v = load i32, ptr %3, align 4
  store i32 %v, ptr %v5, align 4
  %4 = load i32, ptr %v5, align 4
  ret i32 %4

match.arm2:                                       ; preds = %entry
  %5 = getelementptr inbounds nuw %Expr, ptr %match.subj, i32 0, i32 1
  %6 = getelementptr inbounds nuw { i32, i32 }, ptr %5, i32 0, i32 0
  %a = load i32, ptr %6, align 4
  store i32 %a, ptr %a6, align 4
  %7 = getelementptr inbounds nuw { i32, i32 }, ptr %5, i32 0, i32 1
  %b = load i32, ptr %7, align 4
  store i32 %b, ptr %b7, align 4
  %8 = load i32, ptr %a6, align 4
  %9 = load i32, ptr %b7, align 4
  %10 = add i32 %8, %9
  ret i32 %10

match.arm3:                                       ; preds = %entry
  %11 = getelementptr inbounds nuw %Expr, ptr %match.subj, i32 0, i32 1
  %12 = getelementptr inbounds nuw { i32 }, ptr %11, i32 0, i32 0
  %n = load i32, ptr %12, align 4
  store i32 %n, ptr %n8, align 4
  %13 = load i32, ptr %n8, align 4
  %14 = sub i32 0, %13
  ret i32 %14

match.arm4:                                       ; preds = %entry
  ret i32 0
}

; Function Attrs: nounwind
define noundef i32 @kind(%Expr %e) #0 {
entry:
  %v4 = alloca i32, align 4
  %match.subj = alloca %Expr, align 8
  %k = alloca i32, align 4
  %e1 = alloca %Expr, align 8
  store %Expr %e, ptr %e1, align 8
  store i32 0, ptr %k, align 4
  %0 = load %Expr, ptr %e1, align 8
  store %Expr %0, ptr %match.subj, align 8
  %1 = getelementptr inbounds nuw %Expr, ptr %match.subj, i32 0, i32 0
  %match.tag = load i32, ptr %1, align 4
  switch i32 %match.tag, label %match.arm3 [
    i32 3, label %match.arm
    i32 0, label %match.arm2
  ]

match.end:                                        ; preds = %match.arm3, %match.arm2, %match.arm
  %2 = load i32, ptr %k, align 4
  ret i32 %2

match.arm:                                        ; preds = %entry
  store i32 1, ptr %k, align 4
  br label %match.end

match.arm2:                                       ; preds = %entry
  %3 = getelementptr inbounds nuw %Expr, ptr %match.subj, i32 0, i32 1
  %4 = getelementptr inbounds nuw { i32 }, ptr %3, i32 0, i32 0
  %v = load i32, ptr %4, align 4
  store i32 %v, ptr %v4, align 4
  store i32 2, ptr %k, align 4
  br label %match.end

match.arm3:                                       ; preds = %entry
  store i32 9, ptr %k, align 4
  br label %match.end
}

define noundef i32 @main() {
entry:
  %variant.tmp11 = alloca %Expr, align 8
  %variant.tmp9 = alloca %Expr, align 8
  %variant.tmp7 = alloca %Expr, align 8
  %variant.tmp5 = alloca %Expr, align 8
  %variant.tmp3 = alloca %Expr, align 8
  %variant.tmp1 = alloca %Expr, align 8
  %variant.tmp = alloca %Expr, align 8
  store %Expr zeroinitializer, ptr %variant.tmp, align 8
  %0 = getelementptr inbounds nuw %Expr, ptr %variant.tmp, i32 0, i32 0
  store i32 0, ptr %0, align 4
  %1 = getelementptr inbounds nuw %Expr, ptr %variant.tmp, i32 0, i32 1
  %2 = getelementptr inbounds nuw { i32 }, ptr %1, i32 0, i32 0
  store i32 7, ptr %2, align 4
  %variant.val = load %Expr, ptr %variant.tmp, align 8
  %3 = call i32 @eval(%Expr %variant.val)
  store %Expr zeroinitializer, ptr %variant.tmp1, align 8
  %4 = getelementptr inbounds nuw %Expr, ptr %variant.tmp1, i32 0, i32 0
  store i32 1, ptr %4, align 4
  %5 = getelementptr inbounds nuw %Expr, ptr %variant.tmp1, i32 0, i32 1
  %6 = getelementptr inbounds nuw { i32, i32 }, ptr %5, i32 0, i32 0
  store i32 3, ptr %6, align 4
  %7 = getelementptr inbounds nuw { i32, i32 }, ptr %5, i32 0, i32 1
  store i32 4, ptr %7, align 4
  %variant.val2 = load %Expr, ptr %variant.tmp1, align 8
  %8 = call i32 @eval(%Expr %variant.val2)
  store %Expr zeroinitializer, ptr %variant.tmp3, align 8
  %9 = getelementptr inbounds nuw %Expr, ptr %variant.tmp3, i32 0, i32 0
  store i32 2, ptr %9, align 4
  %10 = getelementptr inbounds nuw %Expr, ptr %variant.tmp3, i32 0, i32 1
  %11 = getelementptr inbounds nuw { i32 }, ptr %10, i32 0, i32 0
  store i32 9, ptr %11, align 4
  %variant.val4 = load %Expr, ptr %variant.tmp3, align 8
  %12 = call i32 @eval(%Expr %variant.val4)
  store %Expr zeroinitializer, ptr %variant.tmp5, align 8
  %13 = getelementptr inbounds nuw %Expr, ptr %variant.tmp5, i32 0, i32 0
  store i32 3, ptr %13, align 4
  %variant.val6 = load %Expr, ptr %variant.tmp5, align 8
  %14 = call i32 @eval(%Expr %variant.val6)
  %15 = call i32 (ptr, ...) @printf(ptr @0, i32 %3, i32 %8, i32 %12, i32 %14)
  store %Expr zeroinitializer, ptr %variant.tmp7, align 8
  %16 = getelementptr inbounds nuw %Expr, ptr %variant.tmp7, i32 0, i32 0
  store i32 3, ptr %16, align 4
  %variant.val8 = load %Expr, ptr %variant.tmp7, align 8
  %17 = call i32 @kind(%Expr %variant.val8)
  store %Expr zeroinitializer, ptr %variant.tmp9, align 8
  %18 = getelementptr inbounds nuw %Expr, ptr %variant.tmp9, i32 0, i32 0
  store i32 0, ptr %18, align 4
  %19 = getelementptr inbounds nuw %Expr, ptr %variant.tmp9, i32 0, i32 1
  %20 = getelementptr inbounds nuw { i32 }, ptr %19, i32 0, i32 0
  store i32 5, ptr %20, align 4
  %variant.val10 = load %Expr, ptr %variant.tmp9, align 8
  %21 = call i32 @kind(%Expr %variant.val10)
  store %Expr zeroinitializer, ptr %variant.tmp11, align 8
  %22 = getelementptr inbounds nuw %Expr, ptr %variant.tmp11, i32 0, i32 0
  store i32 1, ptr %22, align 4
  %23 = getelementptr inbounds nuw %Expr, ptr %variant.tmp11, i32 0, i32 1
  %24 = getelementptr inbounds nuw { i32, i32 }, ptr %23, i32 0, i32 0
  store i32 1, ptr %24, align 4
  %25 = getelementptr inbounds nuw { i32, i32 }, ptr %23, i32 0, i32 1
  store i32 2, ptr %25, align 4
  %variant.val12 = load %Expr, ptr %variant.tmp11, align 8
  %26 = call i32 @kind(%Expr %variant.val12)
  %27 = call i32 (ptr, ...) @printf(ptr @1, i32 %17, i32 %21, i32 %26)
  %28 = call i32 (ptr, ...) @printf(ptr @2, i32 0, i32 5, i32 6)
  ret i32 0
}

attributes #0 = { nounwind }
