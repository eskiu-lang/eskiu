
%Option_int = type { i32, [1 x i64] }
%Option_int64 = type { i32, [1 x i64] }
%Either_int_int = type { i32, [1 x i64] }

@0 = private unnamed_addr constant [7 x i8] c"%d %d\0A\00", align 1
@1 = private unnamed_addr constant [6 x i8] c"%lld\0A\00", align 1
@2 = private unnamed_addr constant [7 x i8] c"%d %d\0A\00", align 1

declare i32 @printf(ptr, ...)

; Function Attrs: nounwind
define noundef i32 @opt_or(%Option_int %o, i32 noundef %dflt) #0 {
entry:
  %v4 = alloca i32, align 4
  %match.subj = alloca %Option_int, align 8
  %dflt2 = alloca i32, align 4
  %o1 = alloca %Option_int, align 8
  store %Option_int %o, ptr %o1, align 8
  store i32 %dflt, ptr %dflt2, align 4
  %0 = load %Option_int, ptr %o1, align 8
  store %Option_int %0, ptr %match.subj, align 8
  %1 = getelementptr inbounds nuw %Option_int, ptr %match.subj, i32 0, i32 0
  %match.tag = load i32, ptr %1, align 4
  switch i32 %match.tag, label %match.end [
    i32 1, label %match.arm
    i32 0, label %match.arm3
  ]

match.end:                                        ; preds = %entry
  ret i32 0

match.arm:                                        ; preds = %entry
  %2 = getelementptr inbounds nuw %Option_int, ptr %match.subj, i32 0, i32 1
  %3 = getelementptr inbounds nuw { i32 }, ptr %2, i32 0, i32 0
  %v = load i32, ptr %3, align 4
  store i32 %v, ptr %v4, align 4
  %4 = load i32, ptr %v4, align 4
  ret i32 %4

match.arm3:                                       ; preds = %entry
  %5 = load i32, ptr %dflt2, align 4
  ret i32 %5
}

; Function Attrs: nounwind
define noundef i64 @opt64_or(%Option_int64 %o, i64 noundef %dflt) #0 {
entry:
  %v4 = alloca i64, align 8
  %match.subj = alloca %Option_int64, align 8
  %dflt2 = alloca i64, align 8
  %o1 = alloca %Option_int64, align 8
  store %Option_int64 %o, ptr %o1, align 8
  store i64 %dflt, ptr %dflt2, align 8
  %0 = load %Option_int64, ptr %o1, align 8
  store %Option_int64 %0, ptr %match.subj, align 8
  %1 = getelementptr inbounds nuw %Option_int64, ptr %match.subj, i32 0, i32 0
  %match.tag = load i32, ptr %1, align 4
  switch i32 %match.tag, label %match.end [
    i32 1, label %match.arm
    i32 0, label %match.arm3
  ]

match.end:                                        ; preds = %entry
  ret i64 0

match.arm:                                        ; preds = %entry
  %2 = getelementptr inbounds nuw %Option_int64, ptr %match.subj, i32 0, i32 1
  %3 = getelementptr inbounds nuw { i64 }, ptr %2, i32 0, i32 0
  %v = load i64, ptr %3, align 8
  store i64 %v, ptr %v4, align 8
  %4 = load i64, ptr %v4, align 8
  ret i64 %4

match.arm3:                                       ; preds = %entry
  %5 = load i64, ptr %dflt2, align 8
  ret i64 %5
}

; Function Attrs: nounwind
define noundef i32 @either_i(%Either_int_int %e) #0 {
entry:
  %b4 = alloca i32, align 4
  %a3 = alloca i32, align 4
  %match.subj = alloca %Either_int_int, align 8
  %e1 = alloca %Either_int_int, align 8
  store %Either_int_int %e, ptr %e1, align 8
  %0 = load %Either_int_int, ptr %e1, align 8
  store %Either_int_int %0, ptr %match.subj, align 8
  %1 = getelementptr inbounds nuw %Either_int_int, ptr %match.subj, i32 0, i32 0
  %match.tag = load i32, ptr %1, align 4
  switch i32 %match.tag, label %match.end [
    i32 0, label %match.arm
    i32 1, label %match.arm2
  ]

match.end:                                        ; preds = %entry
  ret i32 0

match.arm:                                        ; preds = %entry
  %2 = getelementptr inbounds nuw %Either_int_int, ptr %match.subj, i32 0, i32 1
  %3 = getelementptr inbounds nuw { i32 }, ptr %2, i32 0, i32 0
  %a = load i32, ptr %3, align 4
  store i32 %a, ptr %a3, align 4
  %4 = load i32, ptr %a3, align 4
  ret i32 %4

match.arm2:                                       ; preds = %entry
  %5 = getelementptr inbounds nuw %Either_int_int, ptr %match.subj, i32 0, i32 1
  %6 = getelementptr inbounds nuw { i32 }, ptr %5, i32 0, i32 0
  %b = load i32, ptr %6, align 4
  store i32 %b, ptr %b4, align 4
  %7 = load i32, ptr %b4, align 4
  %8 = add i32 100, %7
  ret i32 %8
}

define noundef i32 @main() {
entry:
  %variant.tmp7 = alloca %Either_int_int, align 8
  %variant.tmp5 = alloca %Either_int_int, align 8
  %variant.tmp3 = alloca %Option_int64, align 8
  %big = alloca i64, align 8
  %variant.tmp1 = alloca %Option_int, align 8
  %variant.tmp = alloca %Option_int, align 8
  store %Option_int zeroinitializer, ptr %variant.tmp, align 8
  %0 = getelementptr inbounds nuw %Option_int, ptr %variant.tmp, i32 0, i32 0
  store i32 1, ptr %0, align 4
  %1 = getelementptr inbounds nuw %Option_int, ptr %variant.tmp, i32 0, i32 1
  %2 = getelementptr inbounds nuw { i32 }, ptr %1, i32 0, i32 0
  store i32 42, ptr %2, align 4
  %variant.val = load %Option_int, ptr %variant.tmp, align 8
  %3 = call i32 @opt_or(%Option_int %variant.val, i32 -1)
  store %Option_int zeroinitializer, ptr %variant.tmp1, align 8
  %4 = getelementptr inbounds nuw %Option_int, ptr %variant.tmp1, i32 0, i32 0
  store i32 0, ptr %4, align 4
  %variant.val2 = load %Option_int, ptr %variant.tmp1, align 8
  %5 = call i32 @opt_or(%Option_int %variant.val2, i32 -1)
  %6 = call i32 (ptr, ...) @printf(ptr @0, i32 %3, i32 %5)
  store %Option_int64 zeroinitializer, ptr %variant.tmp3, align 8
  %7 = getelementptr inbounds nuw %Option_int64, ptr %variant.tmp3, i32 0, i32 0
  store i32 1, ptr %7, align 4
  %8 = getelementptr inbounds nuw %Option_int64, ptr %variant.tmp3, i32 0, i32 1
  %9 = getelementptr inbounds nuw { i64 }, ptr %8, i32 0, i32 0
  store i64 9000000000, ptr %9, align 8
  %variant.val4 = load %Option_int64, ptr %variant.tmp3, align 8
  %10 = call i64 @opt64_or(%Option_int64 %variant.val4, i64 0)
  store i64 %10, ptr %big, align 8
  %11 = load i64, ptr %big, align 8
  %12 = call i32 (ptr, ...) @printf(ptr @1, i64 %11)
  store %Either_int_int zeroinitializer, ptr %variant.tmp5, align 8
  %13 = getelementptr inbounds nuw %Either_int_int, ptr %variant.tmp5, i32 0, i32 0
  store i32 0, ptr %13, align 4
  %14 = getelementptr inbounds nuw %Either_int_int, ptr %variant.tmp5, i32 0, i32 1
  %15 = getelementptr inbounds nuw { i32 }, ptr %14, i32 0, i32 0
  store i32 7, ptr %15, align 4
  %variant.val6 = load %Either_int_int, ptr %variant.tmp5, align 8
  %16 = call i32 @either_i(%Either_int_int %variant.val6)
  store %Either_int_int zeroinitializer, ptr %variant.tmp7, align 8
  %17 = getelementptr inbounds nuw %Either_int_int, ptr %variant.tmp7, i32 0, i32 0
  store i32 1, ptr %17, align 4
  %18 = getelementptr inbounds nuw %Either_int_int, ptr %variant.tmp7, i32 0, i32 1
  %19 = getelementptr inbounds nuw { i32 }, ptr %18, i32 0, i32 0
  store i32 9, ptr %19, align 4
  %variant.val8 = load %Either_int_int, ptr %variant.tmp7, align 8
  %20 = call i32 @either_i(%Either_int_int %variant.val8)
  %21 = call i32 (ptr, ...) @printf(ptr @2, i32 %16, i32 %20)
  ret i32 0
}

attributes #0 = { nounwind }
