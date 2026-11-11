
%__lambda3.env = type { float }
%__lambda2.env = type { i32, i32 }
%__lambda1.env = type { i32 }

@0 = private unnamed_addr constant [16 x i8] c"square(6) = %d\0A\00", align 1
@1 = private unnamed_addr constant [18 x i8] c"add_base(5) = %d\0A\00", align 1
@2 = private unnamed_addr constant [25 x i8] c"apply(add_base, 7) = %d\0A\00", align 1
@3 = private unnamed_addr constant [17 x i8] c"sum_ab(10) = %d\0A\00", align 1
@4 = private unnamed_addr constant [15 x i8] c"scale(4) = %f\0A\00", align 1

declare i32 @printf(ptr, ...)

define noundef i32 @apply({ ptr, ptr } %f, i32 noundef %x) {
entry:
  %x2 = alloca i32, align 4
  %f1 = alloca { ptr, ptr }, align 8
  store { ptr, ptr } %f, ptr %f1, align 8
  store i32 %x, ptr %x2, align 4
  %0 = load { ptr, ptr }, ptr %f1, align 8
  %fn.ptr = extractvalue { ptr, ptr } %0, 0
  %env.ptr = extractvalue { ptr, ptr } %0, 1
  %1 = load i32, ptr %x2, align 4
  %fn.call = call i32 %fn.ptr(ptr %env.ptr, i32 %1)
  ret i32 %fn.call
}

define noundef i32 @main() {
entry:
  %__lambda3.fat = alloca { ptr, ptr }, align 8
  %__lambda3.env = alloca %__lambda3.env, align 8
  %scale = alloca { ptr, ptr }, align 8
  %factor = alloca float, align 4
  %__lambda2.fat = alloca { ptr, ptr }, align 8
  %__lambda2.env = alloca %__lambda2.env, align 8
  %sum_ab = alloca { ptr, ptr }, align 8
  %b = alloca i32, align 4
  %a = alloca i32, align 4
  %__lambda1.fat = alloca { ptr, ptr }, align 8
  %add_base = alloca { ptr, ptr }, align 8
  %base = alloca i32, align 4
  %__lambda0.fat = alloca { ptr, ptr }, align 8
  %square = alloca { ptr, ptr }, align 8
  %0 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda0.fat, i32 0, i32 0
  %1 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda0.fat, i32 0, i32 1
  store ptr @__lambda0, ptr %0, align 8
  store ptr null, ptr %1, align 8
  %__lambda0.fat.val = load { ptr, ptr }, ptr %__lambda0.fat, align 8
  store { ptr, ptr } %__lambda0.fat.val, ptr %square, align 8
  %2 = load { ptr, ptr }, ptr %square, align 8
  %fn.ptr = extractvalue { ptr, ptr } %2, 0
  %env.ptr = extractvalue { ptr, ptr } %2, 1
  %fn.call = call i32 %fn.ptr(ptr %env.ptr, i32 6)
  %3 = call i32 (ptr, ...) @printf(ptr @0, i32 %fn.call)
  store i32 10, ptr %base, align 4
  %__lambda1.env.heap = call ptr @malloc(i64 4)
  %base1 = load i32, ptr %base, align 4
  %4 = getelementptr inbounds nuw %__lambda1.env, ptr %__lambda1.env.heap, i32 0, i32 0
  store i32 %base1, ptr %4, align 4
  %5 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda1.fat, i32 0, i32 0
  %6 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda1.fat, i32 0, i32 1
  store ptr @__lambda1, ptr %5, align 8
  store ptr %__lambda1.env.heap, ptr %6, align 8
  %__lambda1.fat.val = load { ptr, ptr }, ptr %__lambda1.fat, align 8
  store { ptr, ptr } %__lambda1.fat.val, ptr %add_base, align 8
  %7 = load { ptr, ptr }, ptr %add_base, align 8
  %fn.ptr2 = extractvalue { ptr, ptr } %7, 0
  %env.ptr3 = extractvalue { ptr, ptr } %7, 1
  %fn.call4 = call i32 %fn.ptr2(ptr %env.ptr3, i32 5)
  %8 = call i32 (ptr, ...) @printf(ptr @1, i32 %fn.call4)
  %9 = load { ptr, ptr }, ptr %add_base, align 8
  %10 = call i32 @apply({ ptr, ptr } %9, i32 7)
  %11 = call i32 (ptr, ...) @printf(ptr @2, i32 %10)
  store i32 3, ptr %a, align 4
  store i32 4, ptr %b, align 4
  %a5 = load i32, ptr %a, align 4
  %12 = getelementptr inbounds nuw %__lambda2.env, ptr %__lambda2.env, i32 0, i32 0
  store i32 %a5, ptr %12, align 4
  %b6 = load i32, ptr %b, align 4
  %13 = getelementptr inbounds nuw %__lambda2.env, ptr %__lambda2.env, i32 0, i32 1
  store i32 %b6, ptr %13, align 4
  %14 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda2.fat, i32 0, i32 0
  %15 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda2.fat, i32 0, i32 1
  store ptr @__lambda2, ptr %14, align 8
  store ptr %__lambda2.env, ptr %15, align 8
  %__lambda2.fat.val = load { ptr, ptr }, ptr %__lambda2.fat, align 8
  store { ptr, ptr } %__lambda2.fat.val, ptr %sum_ab, align 8
  %16 = load { ptr, ptr }, ptr %sum_ab, align 8
  %fn.ptr7 = extractvalue { ptr, ptr } %16, 0
  %env.ptr8 = extractvalue { ptr, ptr } %16, 1
  %fn.call9 = call i32 %fn.ptr7(ptr %env.ptr8, i32 10)
  %17 = call i32 (ptr, ...) @printf(ptr @3, i32 %fn.call9)
  store float 2.500000e+00, ptr %factor, align 4
  %factor10 = load float, ptr %factor, align 4
  %18 = getelementptr inbounds nuw %__lambda3.env, ptr %__lambda3.env, i32 0, i32 0
  store float %factor10, ptr %18, align 4
  %19 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda3.fat, i32 0, i32 0
  %20 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda3.fat, i32 0, i32 1
  store ptr @__lambda3, ptr %19, align 8
  store ptr %__lambda3.env, ptr %20, align 8
  %__lambda3.fat.val = load { ptr, ptr }, ptr %__lambda3.fat, align 8
  store { ptr, ptr } %__lambda3.fat.val, ptr %scale, align 8
  %21 = load { ptr, ptr }, ptr %scale, align 8
  %fn.ptr11 = extractvalue { ptr, ptr } %21, 0
  %env.ptr12 = extractvalue { ptr, ptr } %21, 1
  %fn.call13 = call float %fn.ptr11(ptr %env.ptr12, i32 4)
  %22 = fpext float %fn.call13 to double
  %23 = call i32 (ptr, ...) @printf(ptr @4, double %22)
  ret i32 0
}

; Function Attrs: nounwind
define internal i32 @__lambda0(ptr %env, i32 %n) #0 {
entry:
  %n1 = alloca i32, align 4
  store i32 %n, ptr %n1, align 4
  %0 = load i32, ptr %n1, align 4
  %1 = load i32, ptr %n1, align 4
  %2 = mul i32 %0, %1
  ret i32 %2
}

declare ptr @malloc(i64)

; Function Attrs: nounwind
define internal i32 @__lambda1(ptr %env, i32 %x) #0 {
entry:
  %x1 = alloca i32, align 4
  %base = alloca i32, align 4
  %base.gep = getelementptr inbounds nuw %__lambda1.env, ptr %env, i32 0, i32 0
  %base.val = load i32, ptr %base.gep, align 4
  store i32 %base.val, ptr %base, align 4
  store i32 %x, ptr %x1, align 4
  %0 = load i32, ptr %x1, align 4
  %1 = load i32, ptr %base, align 4
  %2 = add i32 %0, %1
  ret i32 %2
}

; Function Attrs: nounwind
define internal i32 @__lambda2(ptr %env, i32 %x) #0 {
entry:
  %x1 = alloca i32, align 4
  %b = alloca i32, align 4
  %a = alloca i32, align 4
  %a.gep = getelementptr inbounds nuw %__lambda2.env, ptr %env, i32 0, i32 0
  %a.val = load i32, ptr %a.gep, align 4
  store i32 %a.val, ptr %a, align 4
  %b.gep = getelementptr inbounds nuw %__lambda2.env, ptr %env, i32 0, i32 1
  %b.val = load i32, ptr %b.gep, align 4
  store i32 %b.val, ptr %b, align 4
  store i32 %x, ptr %x1, align 4
  %0 = load i32, ptr %x1, align 4
  %1 = load i32, ptr %a, align 4
  %2 = add i32 %0, %1
  %3 = load i32, ptr %b, align 4
  %4 = add i32 %2, %3
  ret i32 %4
}

; Function Attrs: nounwind
define internal float @__lambda3(ptr %env, i32 %x) #0 {
entry:
  %x1 = alloca i32, align 4
  %factor = alloca float, align 4
  %factor.gep = getelementptr inbounds nuw %__lambda3.env, ptr %env, i32 0, i32 0
  %factor.val = load float, ptr %factor.gep, align 4
  store float %factor.val, ptr %factor, align 4
  store i32 %x, ptr %x1, align 4
  %0 = load i32, ptr %x1, align 4
  %1 = sitofp i32 %0 to float
  %2 = load float, ptr %factor, align 4
  %3 = fmul float %1, %2
  ret float %3
}

attributes #0 = { nounwind }
