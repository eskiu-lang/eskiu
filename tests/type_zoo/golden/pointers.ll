
@0 = private unnamed_addr constant [6 x i8] c"x=%d\0A\00", align 1
@1 = private unnamed_addr constant [11 x i8] c"arr[3]=%d\0A\00", align 1
@2 = private unnamed_addr constant [13 x i8] c"*(arr+2)=%d\0A\00", align 1
@3 = private unnamed_addr constant [11 x i8] c"arr[4]=%d\0A\00", align 1
@4 = private unnamed_addr constant [10 x i8] c"total=%d\0A\00", align 1

declare ptr @memcpy(ptr, ptr, i64)

declare ptr @memset(ptr, i32, i64)

declare ptr @memmove(ptr, ptr, i64)

declare i32 @memcmp(ptr, ptr, i64)

declare ptr @memchr(ptr, i32, i64)

declare i64 @strlen(ptr)

declare i32 @strcmp(ptr, ptr)

declare i32 @strncmp(ptr, ptr, i64)

declare ptr @strchr(ptr, i32)

declare ptr @strstr(ptr, ptr)

declare ptr @strerror(i32)

declare ptr @calloc(i64, i64)

declare void @free(ptr)

declare i32 @printf(ptr, ...)

define noundef i32 @main() {
entry:
  %j = alloca i32, align 4
  %total = alloca i32, align 4
  %q = alloca ptr, align 8
  %i = alloca i32, align 4
  %arr = alloca ptr, align 8
  %p = alloca ptr, align 8
  %x = alloca i32, align 4
  store i32 41, ptr %x, align 4
  store ptr %x, ptr %p, align 8
  %0 = load ptr, ptr %p, align 8
  %1 = load ptr, ptr %p, align 8
  %2 = load i32, ptr %1, align 4
  %3 = add i32 %2, 1
  store i32 %3, ptr %0, align 4
  %4 = load i32, ptr %x, align 4
  %5 = call i32 (ptr, ...) @printf(ptr @0, i32 %4)
  %6 = call ptr @alloc_int(i64 5)
  store ptr %6, ptr %arr, align 8
  store i32 0, ptr %i, align 4
  br label %for

for:                                              ; preds = %for_step, %entry
  %7 = load i32, ptr %i, align 4
  %8 = icmp slt i32 %7, 5
  br i1 %8, label %for_body, label %for_exit

for_body:                                         ; preds = %for
  %9 = load i32, ptr %i, align 4
  %10 = sext i32 %9 to i64
  %11 = load ptr, ptr %arr, align 8
  %12 = getelementptr i32, ptr %11, i64 %10
  %13 = load i32, ptr %i, align 4
  %14 = load i32, ptr %i, align 4
  %15 = mul i32 %13, %14
  store i32 %15, ptr %12, align 4
  br label %for_step

for_step:                                         ; preds = %for_body
  %16 = load i32, ptr %i, align 4
  %17 = add i32 %16, 1
  store i32 %17, ptr %i, align 4
  br label %for

for_exit:                                         ; preds = %for
  %18 = load ptr, ptr %arr, align 8
  %19 = getelementptr i32, ptr %18, i64 3
  %20 = load i32, ptr %19, align 4
  %21 = call i32 (ptr, ...) @printf(ptr @1, i32 %20)
  %22 = load ptr, ptr %arr, align 8
  %ptr.add = getelementptr i32, ptr %22, i64 2
  store ptr %ptr.add, ptr %q, align 8
  %23 = load ptr, ptr %q, align 8
  %24 = load i32, ptr %23, align 4
  %25 = call i32 (ptr, ...) @printf(ptr @2, i32 %24)
  %26 = load ptr, ptr %arr, align 8
  %ptr.add1 = getelementptr i32, ptr %26, i64 4
  %27 = load i32, ptr %ptr.add1, align 4
  %28 = call i32 (ptr, ...) @printf(ptr @3, i32 %27)
  store i32 0, ptr %total, align 4
  store i32 0, ptr %j, align 4
  br label %for2

for2:                                             ; preds = %for_step4, %for_exit
  %29 = load i32, ptr %j, align 4
  %30 = icmp slt i32 %29, 5
  br i1 %30, label %for_body3, label %for_exit5

for_body3:                                        ; preds = %for2
  %31 = load i32, ptr %total, align 4
  %32 = load i32, ptr %j, align 4
  %33 = sext i32 %32 to i64
  %34 = load ptr, ptr %arr, align 8
  %35 = getelementptr i32, ptr %34, i64 %33
  %36 = load i32, ptr %35, align 4
  %37 = add i32 %31, %36
  store i32 %37, ptr %total, align 4
  br label %for_step4

for_step4:                                        ; preds = %for_body3
  %38 = load i32, ptr %j, align 4
  %39 = add i32 %38, 1
  store i32 %39, ptr %j, align 4
  br label %for2

for_exit5:                                        ; preds = %for2
  %40 = load i32, ptr %total, align 4
  %41 = call i32 (ptr, ...) @printf(ptr @4, i32 %40)
  %42 = load ptr, ptr %arr, align 8
  call void @free(ptr %42)
  ret i32 0
}

define noundef ptr @alloc_int(i64 noundef %n) {
entry:
  %n1 = alloca i64, align 8
  store i64 %n, ptr %n1, align 8
  %0 = load i64, ptr %n1, align 8
  %1 = call ptr @calloc(i64 %0, i64 4)
  ret ptr %1
}
