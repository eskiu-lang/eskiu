
@0 = private unnamed_addr constant [33 x i8] c"uint8=%lld int=%lld double=%lld\0A\00", align 1

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
  %0 = call i64 @esz_uint8(i64 2)
  %1 = call i64 @esz_int(i64 2)
  %2 = call i64 @esz_double(i64 2)
  %3 = call i32 (ptr, ...) @printf(ptr @0, i64 %0, i64 %1, i64 %2)
  ret i32 0
}

define noundef i64 @esz_uint8(i64 noundef %n) {
entry:
  %p = alloca ptr, align 8
  %n1 = alloca i64, align 8
  store i64 %n, ptr %n1, align 8
  %0 = load i64, ptr %n1, align 8
  %1 = call ptr @alloc_uint8(i64 %0)
  store ptr %1, ptr %p, align 8
  %2 = load ptr, ptr %p, align 8
  call void @free(ptr %2)
  ret i64 1
}

define noundef ptr @alloc_uint8(i64 noundef %n) {
entry:
  %n1 = alloca i64, align 8
  store i64 %n, ptr %n1, align 8
  %0 = load i64, ptr %n1, align 8
  %1 = call ptr @calloc(i64 %0, i64 1)
  ret ptr %1
}

define noundef i64 @esz_int(i64 noundef %n) {
entry:
  %p = alloca ptr, align 8
  %n1 = alloca i64, align 8
  store i64 %n, ptr %n1, align 8
  %0 = load i64, ptr %n1, align 8
  %1 = call ptr @alloc_int(i64 %0)
  store ptr %1, ptr %p, align 8
  %2 = load ptr, ptr %p, align 8
  call void @free(ptr %2)
  ret i64 4
}

define noundef ptr @alloc_int(i64 noundef %n) {
entry:
  %n1 = alloca i64, align 8
  store i64 %n, ptr %n1, align 8
  %0 = load i64, ptr %n1, align 8
  %1 = call ptr @calloc(i64 %0, i64 4)
  ret ptr %1
}

define noundef i64 @esz_double(i64 noundef %n) {
entry:
  %p = alloca ptr, align 8
  %n1 = alloca i64, align 8
  store i64 %n, ptr %n1, align 8
  %0 = load i64, ptr %n1, align 8
  %1 = call ptr @alloc_double(i64 %0)
  store ptr %1, ptr %p, align 8
  %2 = load ptr, ptr %p, align 8
  call void @free(ptr %2)
  ret i64 8
}

define noundef ptr @alloc_double(i64 noundef %n) {
entry:
  %n1 = alloca i64, align 8
  store i64 %n, ptr %n1, align 8
  %0 = load i64, ptr %n1, align 8
  %1 = call ptr @calloc(i64 %0, i64 8)
  ret ptr %1
}
