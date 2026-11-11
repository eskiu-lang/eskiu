
%List_int = type { ptr, i32, i32 }

@0 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@1 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@2 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1

declare i32 @printf(ptr, ...)

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

define noundef i32 @main() {
entry:
  %i = alloca i32, align 4
  %nums = alloca %List_int, align 8
  store %List_int zeroinitializer, ptr %nums, align 8
  call void @List_init_int(ptr %nums, i32 4)
  store i32 0, ptr %i, align 4
  br label %while

while:                                            ; preds = %while_body, %entry
  %0 = load i32, ptr %i, align 4
  %1 = icmp slt i32 %0, 5
  br i1 %1, label %while_body, label %while_exit

while_body:                                       ; preds = %while
  %2 = load i32, ptr %i, align 4
  %3 = load i32, ptr %i, align 4
  %4 = mul i32 %2, %3
  call void @List_push_int(ptr %nums, i32 %4)
  %5 = load i32, ptr %i, align 4
  %6 = add i32 %5, 1
  store i32 %6, ptr %i, align 4
  br label %while

while_exit:                                       ; preds = %while
  %7 = call i32 @List_len_int(ptr %nums)
  %8 = call i32 (ptr, ...) @printf(ptr @0, i32 %7)
  %9 = call i32 @List_get_int(ptr %nums, i32 0)
  %10 = call i32 (ptr, ...) @printf(ptr @1, i32 %9)
  %11 = call i32 @List_get_int(ptr %nums, i32 4)
  %12 = call i32 (ptr, ...) @printf(ptr @2, i32 %11)
  call void @List_free_int(ptr %nums)
  ret i32 0
}

define void @List_init_int(ptr noundef %self, i32 noundef %initial_cap) {
entry:
  %cap = alloca i32, align 4
  %initial_cap2 = alloca i32, align 4
  %self1 = alloca ptr, align 8
  store ptr %self, ptr %self1, align 8
  store i32 %initial_cap, ptr %initial_cap2, align 4
  %0 = load i32, ptr %initial_cap2, align 4
  %1 = icmp slt i32 %0, 4
  br i1 %1, label %tern.then, label %tern.else

tern.then:                                        ; preds = %entry
  br label %tern.cont

tern.else:                                        ; preds = %entry
  %2 = load i32, ptr %initial_cap2, align 4
  br label %tern.cont

tern.cont:                                        ; preds = %tern.else, %tern.then
  %3 = phi i32 [ 4, %tern.then ], [ %2, %tern.else ]
  store i32 %3, ptr %cap, align 4
  %4 = load ptr, ptr %self1, align 8
  %5 = getelementptr inbounds nuw %List_int, ptr %4, i32 0, i32 0
  %6 = load i32, ptr %cap, align 4
  %7 = sext i32 %6 to i64
  %8 = call ptr @alloc_int(i64 %7)
  store ptr %8, ptr %5, align 8
  %9 = load ptr, ptr %self1, align 8
  %10 = getelementptr inbounds nuw %List_int, ptr %9, i32 0, i32 1
  store i32 0, ptr %10, align 4
  %11 = load ptr, ptr %self1, align 8
  %12 = getelementptr inbounds nuw %List_int, ptr %11, i32 0, i32 2
  %13 = load i32, ptr %cap, align 4
  store i32 %13, ptr %12, align 4
  ret void
}

define noundef ptr @alloc_int(i64 noundef %n) {
entry:
  %n1 = alloca i64, align 8
  store i64 %n, ptr %n1, align 8
  %0 = load i64, ptr %n1, align 8
  %1 = call ptr @calloc(i64 %0, i64 4)
  ret ptr %1
}

define void @List_push_int(ptr noundef %self, i32 noundef %item) {
entry:
  %__end_i = alloca i32, align 4
  %i = alloca i32, align 4
  %new_data = alloca ptr, align 8
  %new_cap = alloca i32, align 4
  %item2 = alloca i32, align 4
  %self1 = alloca ptr, align 8
  store ptr %self, ptr %self1, align 8
  store i32 %item, ptr %item2, align 4
  %0 = load ptr, ptr %self1, align 8
  %size = getelementptr inbounds nuw %List_int, ptr %0, i32 0, i32 1
  %size3 = load i32, ptr %size, align 4
  %1 = load ptr, ptr %self1, align 8
  %cap = getelementptr inbounds nuw %List_int, ptr %1, i32 0, i32 2
  %cap4 = load i32, ptr %cap, align 4
  %2 = icmp sge i32 %size3, %cap4
  br i1 %2, label %then, label %merge

then:                                             ; preds = %entry
  %3 = load ptr, ptr %self1, align 8
  %cap5 = getelementptr inbounds nuw %List_int, ptr %3, i32 0, i32 2
  %cap6 = load i32, ptr %cap5, align 4
  %4 = mul i32 %cap6, 2
  %5 = icmp slt i32 %4, 4
  br i1 %5, label %tern.then, label %tern.else

merge:                                            ; preds = %for_exit, %entry
  %6 = load ptr, ptr %self1, align 8
  %size14 = getelementptr inbounds nuw %List_int, ptr %6, i32 0, i32 1
  %size15 = load i32, ptr %size14, align 4
  %7 = sext i32 %size15 to i64
  %8 = load ptr, ptr %self1, align 8
  %data16 = getelementptr inbounds nuw %List_int, ptr %8, i32 0, i32 0
  %data17 = load ptr, ptr %data16, align 8
  %9 = getelementptr i32, ptr %data17, i64 %7
  %10 = load i32, ptr %item2, align 4
  store i32 %10, ptr %9, align 4
  %11 = load ptr, ptr %self1, align 8
  %12 = getelementptr inbounds nuw %List_int, ptr %11, i32 0, i32 1
  %13 = load i32, ptr %12, align 4
  %14 = add i32 %13, 1
  store i32 %14, ptr %12, align 4
  ret void

tern.then:                                        ; preds = %then
  br label %tern.cont

tern.else:                                        ; preds = %then
  %15 = load ptr, ptr %self1, align 8
  %cap7 = getelementptr inbounds nuw %List_int, ptr %15, i32 0, i32 2
  %cap8 = load i32, ptr %cap7, align 4
  %16 = mul i32 %cap8, 2
  br label %tern.cont

tern.cont:                                        ; preds = %tern.else, %tern.then
  %17 = phi i32 [ 4, %tern.then ], [ %16, %tern.else ]
  store i32 %17, ptr %new_cap, align 4
  %18 = load i32, ptr %new_cap, align 4
  %19 = sext i32 %18 to i64
  %20 = call ptr @alloc_int(i64 %19)
  store ptr %20, ptr %new_data, align 8
  store i32 0, ptr %i, align 4
  %21 = load ptr, ptr %self1, align 8
  %size9 = getelementptr inbounds nuw %List_int, ptr %21, i32 0, i32 1
  %size10 = load i32, ptr %size9, align 4
  store i32 %size10, ptr %__end_i, align 4
  br label %for

for:                                              ; preds = %for_step, %tern.cont
  %22 = load i32, ptr %i, align 4
  %23 = load i32, ptr %__end_i, align 4
  %24 = icmp slt i32 %22, %23
  br i1 %24, label %for_body, label %for_exit

for_body:                                         ; preds = %for
  %25 = load i32, ptr %i, align 4
  %26 = sext i32 %25 to i64
  %27 = load ptr, ptr %new_data, align 8
  %28 = getelementptr i32, ptr %27, i64 %26
  %29 = load i32, ptr %i, align 4
  %30 = sext i32 %29 to i64
  %31 = load ptr, ptr %self1, align 8
  %data = getelementptr inbounds nuw %List_int, ptr %31, i32 0, i32 0
  %data11 = load ptr, ptr %data, align 8
  %32 = getelementptr i32, ptr %data11, i64 %30
  %33 = load i32, ptr %32, align 4
  store i32 %33, ptr %28, align 4
  br label %for_step

for_step:                                         ; preds = %for_body
  %34 = load i32, ptr %i, align 4
  %35 = add i32 %34, 1
  store i32 %35, ptr %i, align 4
  br label %for

for_exit:                                         ; preds = %for
  %36 = load ptr, ptr %self1, align 8
  %data12 = getelementptr inbounds nuw %List_int, ptr %36, i32 0, i32 0
  %data13 = load ptr, ptr %data12, align 8
  call void @free(ptr %data13)
  %37 = load ptr, ptr %self1, align 8
  %38 = getelementptr inbounds nuw %List_int, ptr %37, i32 0, i32 0
  %39 = load ptr, ptr %new_data, align 8
  store ptr %39, ptr %38, align 8
  %40 = load ptr, ptr %self1, align 8
  %41 = getelementptr inbounds nuw %List_int, ptr %40, i32 0, i32 2
  %42 = load i32, ptr %new_cap, align 4
  store i32 %42, ptr %41, align 4
  br label %merge
}

; Function Attrs: nounwind
define noundef i32 @List_len_int(ptr noundef %self) #0 {
entry:
  %self1 = alloca ptr, align 8
  store ptr %self, ptr %self1, align 8
  %0 = load ptr, ptr %self1, align 8
  %size = getelementptr inbounds nuw %List_int, ptr %0, i32 0, i32 1
  %size2 = load i32, ptr %size, align 4
  ret i32 %size2
}

; Function Attrs: nounwind
define noundef i32 @List_get_int(ptr noundef %self, i32 noundef %i) #0 {
entry:
  %i2 = alloca i32, align 4
  %self1 = alloca ptr, align 8
  store ptr %self, ptr %self1, align 8
  store i32 %i, ptr %i2, align 4
  %0 = load i32, ptr %i2, align 4
  %1 = sext i32 %0 to i64
  %2 = load ptr, ptr %self1, align 8
  %data = getelementptr inbounds nuw %List_int, ptr %2, i32 0, i32 0
  %data3 = load ptr, ptr %data, align 8
  %3 = getelementptr i32, ptr %data3, i64 %1
  %4 = load i32, ptr %3, align 4
  ret i32 %4
}

define void @List_free_int(ptr noundef %self) {
entry:
  %self1 = alloca ptr, align 8
  store ptr %self, ptr %self1, align 8
  %0 = load ptr, ptr %self1, align 8
  %data = getelementptr inbounds nuw %List_int, ptr %0, i32 0, i32 0
  %data2 = load ptr, ptr %data, align 8
  call void @free(ptr %data2)
  %1 = load ptr, ptr %self1, align 8
  %2 = getelementptr inbounds nuw %List_int, ptr %1, i32 0, i32 0
  store ptr null, ptr %2, align 8
  %3 = load ptr, ptr %self1, align 8
  %4 = getelementptr inbounds nuw %List_int, ptr %3, i32 0, i32 1
  store i32 0, ptr %4, align 4
  %5 = load ptr, ptr %self1, align 8
  %6 = getelementptr inbounds nuw %List_int, ptr %5, i32 0, i32 2
  store i32 0, ptr %6, align 4
  ret void
}

attributes #0 = { nounwind }
