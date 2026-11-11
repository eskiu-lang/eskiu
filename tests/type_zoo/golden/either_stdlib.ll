
%Either_int_string = type { i32, [1 x i64] }
%Option_int = type { i32, [1 x i64] }

@0 = private unnamed_addr constant [7 x i8] c"%d %d\0A\00", align 1
@1 = private unnamed_addr constant [7 x i8] c"%d %d\0A\00", align 1
@2 = private unnamed_addr constant [13 x i8] c"%d %d %d %d\0A\00", align 1

declare i32 @printf(ptr, ...)

define noundef i32 @main() {
entry:
  %__recv.2 = alloca ptr, align 8
  %__recv.1 = alloca ptr, align 8
  %__recv.0 = alloca ptr, align 8
  %pb = alloca ptr, align 8
  %variant.tmp3 = alloca %Either_int_string, align 8
  %e = alloca %Either_int_string, align 8
  %variant.tmp1 = alloca %Option_int, align 8
  %b = alloca %Option_int, align 8
  %variant.tmp = alloca %Option_int, align 8
  %a = alloca %Option_int, align 8
  store %Option_int zeroinitializer, ptr %variant.tmp, align 8
  %0 = getelementptr inbounds nuw %Option_int, ptr %variant.tmp, i32 0, i32 0
  store i32 1, ptr %0, align 4
  %1 = getelementptr inbounds nuw %Option_int, ptr %variant.tmp, i32 0, i32 1
  %2 = getelementptr inbounds nuw { i32 }, ptr %1, i32 0, i32 0
  store i32 7, ptr %2, align 4
  %variant.val = load %Option_int, ptr %variant.tmp, align 8
  store %Option_int %variant.val, ptr %a, align 8
  store %Option_int zeroinitializer, ptr %variant.tmp1, align 8
  %3 = getelementptr inbounds nuw %Option_int, ptr %variant.tmp1, i32 0, i32 0
  store i32 0, ptr %3, align 4
  %variant.val2 = load %Option_int, ptr %variant.tmp1, align 8
  store %Option_int %variant.val2, ptr %b, align 8
  %4 = call i1 @Option_is_some_int(ptr %a)
  %5 = call i1 @Option_is_some_int(ptr %b)
  %6 = zext i1 %4 to i32
  %7 = zext i1 %5 to i32
  %8 = call i32 (ptr, ...) @printf(ptr @0, i32 %6, i32 %7)
  %9 = call i32 @Option_unwrap_or_int(ptr %a, i32 -1)
  %10 = call i32 @Option_unwrap_or_int(ptr %b, i32 -1)
  %11 = call i32 (ptr, ...) @printf(ptr @1, i32 %9, i32 %10)
  store %Either_int_string zeroinitializer, ptr %variant.tmp3, align 8
  %12 = getelementptr inbounds nuw %Either_int_string, ptr %variant.tmp3, i32 0, i32 0
  store i32 0, ptr %12, align 4
  %13 = getelementptr inbounds nuw %Either_int_string, ptr %variant.tmp3, i32 0, i32 1
  %14 = getelementptr inbounds nuw { i32 }, ptr %13, i32 0, i32 0
  store i32 3, ptr %14, align 4
  %variant.val4 = load %Either_int_string, ptr %variant.tmp3, align 8
  store %Either_int_string %variant.val4, ptr %e, align 8
  store ptr %b, ptr %pb, align 8
  store ptr %a, ptr %__recv.0, align 8
  %15 = load ptr, ptr %__recv.0, align 8
  %16 = call i1 @Option_is_some_int(ptr %15)
  %17 = load ptr, ptr %pb, align 8
  %18 = call i1 @Option_is_none_int(ptr %17)
  store ptr %b, ptr %__recv.1, align 8
  %19 = load ptr, ptr %__recv.1, align 8
  %20 = call i32 @Option_unwrap_or_int(ptr %19, i32 4)
  store ptr %e, ptr %__recv.2, align 8
  %21 = load ptr, ptr %__recv.2, align 8
  %22 = call i1 @Either_is_left_int_string(ptr %21)
  %23 = zext i1 %16 to i32
  %24 = zext i1 %18 to i32
  %25 = zext i1 %22 to i32
  %26 = call i32 (ptr, ...) @printf(ptr @2, i32 %23, i32 %24, i32 %20, i32 %25)
  ret i32 0
}

; Function Attrs: nounwind
define noundef zeroext i1 @Option_is_some_int(ptr noundef %o) #0 {
entry:
  %v3 = alloca i32, align 4
  %match.subj = alloca %Option_int, align 8
  %o1 = alloca ptr, align 8
  store ptr %o, ptr %o1, align 8
  %0 = load ptr, ptr %o1, align 8
  %1 = load %Option_int, ptr %0, align 8
  store %Option_int %1, ptr %match.subj, align 8
  %2 = getelementptr inbounds nuw %Option_int, ptr %match.subj, i32 0, i32 0
  %match.tag = load i32, ptr %2, align 4
  switch i32 %match.tag, label %match.end [
    i32 1, label %match.arm
    i32 0, label %match.arm2
  ]

match.end:                                        ; preds = %entry
  ret i1 false

match.arm:                                        ; preds = %entry
  %3 = getelementptr inbounds nuw %Option_int, ptr %match.subj, i32 0, i32 1
  %4 = getelementptr inbounds nuw { i32 }, ptr %3, i32 0, i32 0
  %v = load i32, ptr %4, align 4
  store i32 %v, ptr %v3, align 4
  ret i1 true

match.arm2:                                       ; preds = %entry
  ret i1 false
}

; Function Attrs: nounwind
define noundef i32 @Option_unwrap_or_int(ptr noundef %o, i32 noundef %dflt) #0 {
entry:
  %v4 = alloca i32, align 4
  %match.subj = alloca %Option_int, align 8
  %dflt2 = alloca i32, align 4
  %o1 = alloca ptr, align 8
  store ptr %o, ptr %o1, align 8
  store i32 %dflt, ptr %dflt2, align 4
  %0 = load ptr, ptr %o1, align 8
  %1 = load %Option_int, ptr %0, align 8
  store %Option_int %1, ptr %match.subj, align 8
  %2 = getelementptr inbounds nuw %Option_int, ptr %match.subj, i32 0, i32 0
  %match.tag = load i32, ptr %2, align 4
  switch i32 %match.tag, label %match.end [
    i32 1, label %match.arm
    i32 0, label %match.arm3
  ]

match.end:                                        ; preds = %entry
  ret i32 0

match.arm:                                        ; preds = %entry
  %3 = getelementptr inbounds nuw %Option_int, ptr %match.subj, i32 0, i32 1
  %4 = getelementptr inbounds nuw { i32 }, ptr %3, i32 0, i32 0
  %v = load i32, ptr %4, align 4
  store i32 %v, ptr %v4, align 4
  %5 = load i32, ptr %v4, align 4
  ret i32 %5

match.arm3:                                       ; preds = %entry
  %6 = load i32, ptr %dflt2, align 4
  ret i32 %6
}

; Function Attrs: nounwind
define noundef zeroext i1 @Option_is_none_int(ptr noundef %o) #0 {
entry:
  %v3 = alloca i32, align 4
  %match.subj = alloca %Option_int, align 8
  %o1 = alloca ptr, align 8
  store ptr %o, ptr %o1, align 8
  %0 = load ptr, ptr %o1, align 8
  %1 = load %Option_int, ptr %0, align 8
  store %Option_int %1, ptr %match.subj, align 8
  %2 = getelementptr inbounds nuw %Option_int, ptr %match.subj, i32 0, i32 0
  %match.tag = load i32, ptr %2, align 4
  switch i32 %match.tag, label %match.end [
    i32 1, label %match.arm
    i32 0, label %match.arm2
  ]

match.end:                                        ; preds = %entry
  ret i1 false

match.arm:                                        ; preds = %entry
  %3 = getelementptr inbounds nuw %Option_int, ptr %match.subj, i32 0, i32 1
  %4 = getelementptr inbounds nuw { i32 }, ptr %3, i32 0, i32 0
  %v = load i32, ptr %4, align 4
  store i32 %v, ptr %v3, align 4
  ret i1 false

match.arm2:                                       ; preds = %entry
  ret i1 true
}

; Function Attrs: nounwind
define noundef zeroext i1 @Either_is_left_int_string(ptr noundef %e) #0 {
entry:
  %b4 = alloca ptr, align 8
  %a3 = alloca i32, align 4
  %match.subj = alloca %Either_int_string, align 8
  %e1 = alloca ptr, align 8
  store ptr %e, ptr %e1, align 8
  %0 = load ptr, ptr %e1, align 8
  %1 = load %Either_int_string, ptr %0, align 8
  store %Either_int_string %1, ptr %match.subj, align 8
  %2 = getelementptr inbounds nuw %Either_int_string, ptr %match.subj, i32 0, i32 0
  %match.tag = load i32, ptr %2, align 4
  switch i32 %match.tag, label %match.end [
    i32 0, label %match.arm
    i32 1, label %match.arm2
  ]

match.end:                                        ; preds = %entry
  ret i1 false

match.arm:                                        ; preds = %entry
  %3 = getelementptr inbounds nuw %Either_int_string, ptr %match.subj, i32 0, i32 1
  %4 = getelementptr inbounds nuw { i32 }, ptr %3, i32 0, i32 0
  %a = load i32, ptr %4, align 4
  store i32 %a, ptr %a3, align 4
  ret i1 true

match.arm2:                                       ; preds = %entry
  %5 = getelementptr inbounds nuw %Either_int_string, ptr %match.subj, i32 0, i32 1
  %6 = getelementptr inbounds nuw { ptr }, ptr %5, i32 0, i32 0
  %b = load ptr, ptr %6, align 8
  store ptr %b, ptr %b4, align 8
  ret i1 false
}

attributes #0 = { nounwind }
