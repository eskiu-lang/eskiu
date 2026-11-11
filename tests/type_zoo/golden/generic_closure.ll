
%__lambda0.env = type { i32, ptr, i32 }
%__lambda1.env = type { i32, ptr, i64 }

@0 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1
@1 = private unnamed_addr constant [4 x i8] c"%d\0A\00", align 1

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

define noundef i32 @call_it({ ptr, ptr } %op) {
entry:
  %op1 = alloca { ptr, ptr }, align 8
  store { ptr, ptr } %op, ptr %op1, align 8
  %0 = load { ptr, ptr }, ptr %op1, align 8
  %fn.ptr = extractvalue { ptr, ptr } %0, 0
  %env.ptr = extractvalue { ptr, ptr } %0, 1
  %fn.call = call i32 %fn.ptr(ptr %env.ptr)
  ret i32 %fn.call
}

define noundef i32 @main() {
entry:
  %0 = call i32 @box_int(i32 10, i32 1)
  %1 = call i32 (ptr, ...) @printf(ptr @0, i32 %0)
  %2 = call i32 @box_int64(i64 100, i32 5)
  %3 = call i32 (ptr, ...) @printf(ptr @1, i32 %2)
  ret i32 0
}

define noundef i32 @box_int(i32 noundef %v, i32 noundef %bump) {
entry:
  %h = alloca i32, align 4
  %r2 = alloca i32, align 4
  %r1 = alloca i32, align 4
  %__lambda0.fat = alloca { ptr, ptr }, align 8
  %f = alloca { ptr, ptr }, align 8
  %hits = alloca ptr, align 8
  %bump2 = alloca i32, align 4
  %v1 = alloca i32, align 4
  store i32 %v, ptr %v1, align 4
  store i32 %bump, ptr %bump2, align 4
  %0 = call ptr @alloc_int(i64 1)
  store ptr %0, ptr %hits, align 8
  %1 = load ptr, ptr %hits, align 8
  store i32 0, ptr %1, align 4
  %__lambda0.env.heap = call ptr @malloc(i64 24)
  %bump3 = load i32, ptr %bump2, align 4
  %2 = getelementptr inbounds nuw %__lambda0.env, ptr %__lambda0.env.heap, i32 0, i32 0
  store i32 %bump3, ptr %2, align 4
  %hits4 = load ptr, ptr %hits, align 8
  %3 = getelementptr inbounds nuw %__lambda0.env, ptr %__lambda0.env.heap, i32 0, i32 1
  store ptr %hits4, ptr %3, align 8
  %v5 = load i32, ptr %v1, align 4
  %4 = getelementptr inbounds nuw %__lambda0.env, ptr %__lambda0.env.heap, i32 0, i32 2
  store i32 %v5, ptr %4, align 4
  %5 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda0.fat, i32 0, i32 0
  %6 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda0.fat, i32 0, i32 1
  store ptr @__lambda0, ptr %5, align 8
  store ptr %__lambda0.env.heap, ptr %6, align 8
  %__lambda0.fat.val = load { ptr, ptr }, ptr %__lambda0.fat, align 8
  store { ptr, ptr } %__lambda0.fat.val, ptr %f, align 8
  %7 = load { ptr, ptr }, ptr %f, align 8
  %8 = call i32 @call_it({ ptr, ptr } %7)
  store i32 %8, ptr %r1, align 4
  %9 = load { ptr, ptr }, ptr %f, align 8
  %10 = call i32 @call_it({ ptr, ptr } %9)
  store i32 %10, ptr %r2, align 4
  %11 = load { ptr, ptr }, ptr %f, align 8
  %clos.env = extractvalue { ptr, ptr } %11, 1
  call void @free(ptr %clos.env)
  %12 = load ptr, ptr %hits, align 8
  %13 = load i32, ptr %12, align 4
  store i32 %13, ptr %h, align 4
  %14 = load ptr, ptr %hits, align 8
  call void @free(ptr %14)
  %15 = load i32, ptr %r1, align 4
  %16 = load i32, ptr %r2, align 4
  %17 = add i32 %15, %16
  %18 = load i32, ptr %h, align 4
  %19 = add i32 %17, %18
  ret i32 %19
}

define noundef ptr @alloc_int(i64 noundef %n) {
entry:
  %n1 = alloca i64, align 8
  store i64 %n, ptr %n1, align 8
  %0 = load i64, ptr %n1, align 8
  %1 = call ptr @calloc(i64 %0, i64 4)
  ret ptr %1
}

declare ptr @malloc(i64)

; Function Attrs: nounwind
define internal i32 @__lambda0(ptr %env) #0 {
entry:
  %v = alloca i32, align 4
  %hits = alloca ptr, align 8
  %bump = alloca i32, align 4
  %bump.gep = getelementptr inbounds nuw %__lambda0.env, ptr %env, i32 0, i32 0
  %bump.val = load i32, ptr %bump.gep, align 4
  store i32 %bump.val, ptr %bump, align 4
  %hits.gep = getelementptr inbounds nuw %__lambda0.env, ptr %env, i32 0, i32 1
  %hits.val = load ptr, ptr %hits.gep, align 8
  store ptr %hits.val, ptr %hits, align 8
  %v.gep = getelementptr inbounds nuw %__lambda0.env, ptr %env, i32 0, i32 2
  %v.val = load i32, ptr %v.gep, align 4
  store i32 %v.val, ptr %v, align 4
  %0 = load ptr, ptr %hits, align 8
  %1 = load ptr, ptr %hits, align 8
  %2 = load i32, ptr %1, align 4
  %3 = load i32, ptr %bump, align 4
  %4 = add i32 %2, %3
  store i32 %4, ptr %0, align 4
  %5 = load i32, ptr %v, align 4
  %6 = load ptr, ptr %hits, align 8
  %7 = load i32, ptr %6, align 4
  %8 = add i32 %5, %7
  ret i32 %8
}

define noundef i32 @box_int64(i64 noundef %v, i32 noundef %bump) {
entry:
  %h = alloca i32, align 4
  %r2 = alloca i32, align 4
  %r1 = alloca i32, align 4
  %__lambda1.fat = alloca { ptr, ptr }, align 8
  %f = alloca { ptr, ptr }, align 8
  %hits = alloca ptr, align 8
  %bump2 = alloca i32, align 4
  %v1 = alloca i64, align 8
  store i64 %v, ptr %v1, align 8
  store i32 %bump, ptr %bump2, align 4
  %0 = call ptr @alloc_int(i64 1)
  store ptr %0, ptr %hits, align 8
  %1 = load ptr, ptr %hits, align 8
  store i32 0, ptr %1, align 4
  %__lambda1.env.heap = call ptr @malloc(i64 24)
  %bump3 = load i32, ptr %bump2, align 4
  %2 = getelementptr inbounds nuw %__lambda1.env, ptr %__lambda1.env.heap, i32 0, i32 0
  store i32 %bump3, ptr %2, align 4
  %hits4 = load ptr, ptr %hits, align 8
  %3 = getelementptr inbounds nuw %__lambda1.env, ptr %__lambda1.env.heap, i32 0, i32 1
  store ptr %hits4, ptr %3, align 8
  %v5 = load i64, ptr %v1, align 8
  %4 = getelementptr inbounds nuw %__lambda1.env, ptr %__lambda1.env.heap, i32 0, i32 2
  store i64 %v5, ptr %4, align 8
  %5 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda1.fat, i32 0, i32 0
  %6 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda1.fat, i32 0, i32 1
  store ptr @__lambda1, ptr %5, align 8
  store ptr %__lambda1.env.heap, ptr %6, align 8
  %__lambda1.fat.val = load { ptr, ptr }, ptr %__lambda1.fat, align 8
  store { ptr, ptr } %__lambda1.fat.val, ptr %f, align 8
  %7 = load { ptr, ptr }, ptr %f, align 8
  %8 = call i32 @call_it({ ptr, ptr } %7)
  store i32 %8, ptr %r1, align 4
  %9 = load { ptr, ptr }, ptr %f, align 8
  %10 = call i32 @call_it({ ptr, ptr } %9)
  store i32 %10, ptr %r2, align 4
  %11 = load { ptr, ptr }, ptr %f, align 8
  %clos.env = extractvalue { ptr, ptr } %11, 1
  call void @free(ptr %clos.env)
  %12 = load ptr, ptr %hits, align 8
  %13 = load i32, ptr %12, align 4
  store i32 %13, ptr %h, align 4
  %14 = load ptr, ptr %hits, align 8
  call void @free(ptr %14)
  %15 = load i32, ptr %r1, align 4
  %16 = load i32, ptr %r2, align 4
  %17 = add i32 %15, %16
  %18 = load i32, ptr %h, align 4
  %19 = add i32 %17, %18
  ret i32 %19
}

; Function Attrs: nounwind
define internal i32 @__lambda1(ptr %env) #0 {
entry:
  %v = alloca i64, align 8
  %hits = alloca ptr, align 8
  %bump = alloca i32, align 4
  %bump.gep = getelementptr inbounds nuw %__lambda1.env, ptr %env, i32 0, i32 0
  %bump.val = load i32, ptr %bump.gep, align 4
  store i32 %bump.val, ptr %bump, align 4
  %hits.gep = getelementptr inbounds nuw %__lambda1.env, ptr %env, i32 0, i32 1
  %hits.val = load ptr, ptr %hits.gep, align 8
  store ptr %hits.val, ptr %hits, align 8
  %v.gep = getelementptr inbounds nuw %__lambda1.env, ptr %env, i32 0, i32 2
  %v.val = load i64, ptr %v.gep, align 8
  store i64 %v.val, ptr %v, align 8
  %0 = load ptr, ptr %hits, align 8
  %1 = load ptr, ptr %hits, align 8
  %2 = load i32, ptr %1, align 4
  %3 = load i32, ptr %bump, align 4
  %4 = add i32 %2, %3
  store i32 %4, ptr %0, align 4
  %5 = load i64, ptr %v, align 8
  %6 = trunc i64 %5 to i32
  %7 = load ptr, ptr %hits, align 8
  %8 = load i32, ptr %7, align 4
  %9 = add i32 %6, %8
  ret i32 %9
}

attributes #0 = { nounwind }
