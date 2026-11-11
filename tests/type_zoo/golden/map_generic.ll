
%Pt = type { i32, i32 }
%HashMap_Pt_int = type { ptr, ptr, ptr, i32, i32, { ptr, ptr }, { ptr, ptr } }
%HashMap_int_int = type { ptr, ptr, ptr, i32, i32, { ptr, ptr }, { ptr, ptr } }

@0 = private unnamed_addr constant [7 x i8] c"%d %d\0A\00", align 1
@1 = private unnamed_addr constant [7 x i8] c"%d %d\0A\00", align 1

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

; Function Attrs: nounwind
define noundef i32 @_map_hash(ptr noundef %s) #0 {
entry:
  %i = alloca i32, align 4
  %h = alloca i32, align 4
  %s1 = alloca ptr, align 8
  store ptr %s, ptr %s1, align 8
  store i32 5381, ptr %h, align 4
  store i32 0, ptr %i, align 4
  br label %for

for:                                              ; preds = %for_step, %entry
  %0 = load i32, ptr %i, align 4
  %1 = sext i32 %0 to i64
  %2 = load ptr, ptr %s1, align 8
  %3 = getelementptr i8, ptr %2, i64 %1
  %4 = load i8, ptr %3, align 1
  %5 = zext i8 %4 to i32
  %6 = icmp ne i32 %5, 0
  br i1 %6, label %for_body, label %for_exit

for_body:                                         ; preds = %for
  %7 = load i32, ptr %h, align 4
  %8 = mul i32 %7, 33
  %9 = load i32, ptr %i, align 4
  %10 = sext i32 %9 to i64
  %11 = load ptr, ptr %s1, align 8
  %12 = getelementptr i8, ptr %11, i64 %10
  %13 = load i8, ptr %12, align 1
  %14 = zext i8 %13 to i32
  %15 = and i32 %14, 255
  %16 = add i32 %8, %15
  store i32 %16, ptr %h, align 4
  br label %for_step

for_step:                                         ; preds = %for_body
  %17 = load i32, ptr %i, align 4
  %18 = add i32 %17, 1
  store i32 %18, ptr %i, align 4
  br label %for

for_exit:                                         ; preds = %for
  %19 = load i32, ptr %h, align 4
  %20 = and i32 %19, -2147483648
  %21 = icmp ne i32 %20, 0
  br i1 %21, label %then, label %merge

then:                                             ; preds = %for_exit
  %22 = load i32, ptr %h, align 4
  %23 = sub i32 0, %22
  store i32 %23, ptr %h, align 4
  br label %merge

merge:                                            ; preds = %then, %for_exit
  %24 = load i32, ptr %h, align 4
  %25 = and i32 %24, 2147483647
  ret i32 %25
}

; Function Attrs: nounwind
define noundef i32 @_map_streq(ptr noundef %a, ptr noundef %b) #0 {
entry:
  %i = alloca i32, align 4
  %b2 = alloca ptr, align 8
  %a1 = alloca ptr, align 8
  store ptr %a, ptr %a1, align 8
  store ptr %b, ptr %b2, align 8
  store i32 0, ptr %i, align 4
  br label %while

while:                                            ; preds = %merge, %entry
  %0 = load i32, ptr %i, align 4
  %1 = sext i32 %0 to i64
  %2 = load ptr, ptr %a1, align 8
  %3 = getelementptr i8, ptr %2, i64 %1
  %4 = load i8, ptr %3, align 1
  %5 = zext i8 %4 to i32
  %6 = icmp ne i32 %5, 0
  br i1 %6, label %sc.rhs, label %sc.cont

while_body:                                       ; preds = %sc.cont
  %7 = load i32, ptr %i, align 4
  %8 = sext i32 %7 to i64
  %9 = load ptr, ptr %a1, align 8
  %10 = getelementptr i8, ptr %9, i64 %8
  %11 = load i8, ptr %10, align 1
  %12 = load i32, ptr %i, align 4
  %13 = sext i32 %12 to i64
  %14 = load ptr, ptr %b2, align 8
  %15 = getelementptr i8, ptr %14, i64 %13
  %16 = load i8, ptr %15, align 1
  %17 = zext i8 %11 to i32
  %18 = zext i8 %16 to i32
  %19 = icmp ne i32 %17, %18
  br i1 %19, label %then, label %merge

while_exit:                                       ; preds = %sc.cont
  %20 = load i32, ptr %i, align 4
  %21 = sext i32 %20 to i64
  %22 = load ptr, ptr %a1, align 8
  %23 = getelementptr i8, ptr %22, i64 %21
  %24 = load i8, ptr %23, align 1
  %25 = load i32, ptr %i, align 4
  %26 = sext i32 %25 to i64
  %27 = load ptr, ptr %b2, align 8
  %28 = getelementptr i8, ptr %27, i64 %26
  %29 = load i8, ptr %28, align 1
  %30 = zext i8 %24 to i32
  %31 = zext i8 %29 to i32
  %32 = icmp eq i32 %30, %31
  br i1 %32, label %tern.then, label %tern.else

sc.rhs:                                           ; preds = %while
  %33 = load i32, ptr %i, align 4
  %34 = sext i32 %33 to i64
  %35 = load ptr, ptr %b2, align 8
  %36 = getelementptr i8, ptr %35, i64 %34
  %37 = load i8, ptr %36, align 1
  %38 = zext i8 %37 to i32
  %39 = icmp ne i32 %38, 0
  br label %sc.cont

sc.cont:                                          ; preds = %sc.rhs, %while
  %40 = phi i1 [ false, %while ], [ %39, %sc.rhs ]
  br i1 %40, label %while_body, label %while_exit

then:                                             ; preds = %while_body
  ret i32 0

merge:                                            ; preds = %while_body
  %41 = load i32, ptr %i, align 4
  %42 = add i32 %41, 1
  store i32 %42, ptr %i, align 4
  br label %while

tern.then:                                        ; preds = %while_exit
  br label %tern.cont

tern.else:                                        ; preds = %while_exit
  br label %tern.cont

tern.cont:                                        ; preds = %tern.else, %tern.then
  %43 = phi i32 [ 1, %tern.then ], [ 0, %tern.else ]
  ret i32 %43
}

define noundef ptr @map_dup(ptr noundef %s) {
entry:
  %d = alloca ptr, align 8
  %n = alloca i32, align 4
  %s1 = alloca ptr, align 8
  store ptr %s, ptr %s1, align 8
  %0 = load ptr, ptr %s1, align 8
  %1 = call i64 @strlen(ptr %0)
  %2 = trunc i64 %1 to i32
  store i32 %2, ptr %n, align 4
  %3 = load i32, ptr %n, align 4
  %4 = add i32 %3, 1
  %5 = sext i32 %4 to i64
  %6 = call ptr @alloc_char(i64 %5)
  store ptr %6, ptr %d, align 8
  %7 = load ptr, ptr %d, align 8
  %8 = load ptr, ptr %s1, align 8
  %9 = load i32, ptr %n, align 4
  %10 = add i32 %9, 1
  %11 = sext i32 %10 to i64
  %12 = call ptr @memcpy(ptr %7, ptr %8, i64 %11)
  %13 = load ptr, ptr %d, align 8
  ret ptr %13
}

; Function Attrs: nounwind
define noundef i32 @_map_home_between(i32 noundef %home, i32 noundef %hole, i32 noundef %j) #0 {
entry:
  %j3 = alloca i32, align 4
  %hole2 = alloca i32, align 4
  %home1 = alloca i32, align 4
  store i32 %home, ptr %home1, align 4
  store i32 %hole, ptr %hole2, align 4
  store i32 %j, ptr %j3, align 4
  %0 = load i32, ptr %hole2, align 4
  %1 = load i32, ptr %j3, align 4
  %2 = icmp sle i32 %0, %1
  br i1 %2, label %then, label %merge

then:                                             ; preds = %entry
  %3 = load i32, ptr %home1, align 4
  %4 = load i32, ptr %hole2, align 4
  %5 = icmp sgt i32 %3, %4
  br i1 %5, label %sc.rhs, label %sc.cont

merge:                                            ; preds = %entry
  %6 = load i32, ptr %home1, align 4
  %7 = load i32, ptr %hole2, align 4
  %8 = icmp sgt i32 %6, %7
  br i1 %8, label %sc.cont5, label %sc.rhs4

sc.rhs:                                           ; preds = %then
  %9 = load i32, ptr %home1, align 4
  %10 = load i32, ptr %j3, align 4
  %11 = icmp sle i32 %9, %10
  br label %sc.cont

sc.cont:                                          ; preds = %sc.rhs, %then
  %12 = phi i1 [ false, %then ], [ %11, %sc.rhs ]
  br i1 %12, label %tern.then, label %tern.else

tern.then:                                        ; preds = %sc.cont
  br label %tern.cont

tern.else:                                        ; preds = %sc.cont
  br label %tern.cont

tern.cont:                                        ; preds = %tern.else, %tern.then
  %13 = phi i32 [ 1, %tern.then ], [ 0, %tern.else ]
  ret i32 %13

sc.rhs4:                                          ; preds = %merge
  %14 = load i32, ptr %home1, align 4
  %15 = load i32, ptr %j3, align 4
  %16 = icmp sle i32 %14, %15
  br label %sc.cont5

sc.cont5:                                         ; preds = %sc.rhs4, %merge
  %17 = phi i1 [ true, %merge ], [ %16, %sc.rhs4 ]
  br i1 %17, label %tern.then6, label %tern.else7

tern.then6:                                       ; preds = %sc.cont5
  br label %tern.cont8

tern.else7:                                       ; preds = %sc.cont5
  br label %tern.cont8

tern.cont8:                                       ; preds = %tern.else7, %tern.then6
  %18 = phi i32 [ 1, %tern.then6 ], [ 0, %tern.else7 ]
  ret i32 %18
}

; Function Attrs: nounwind
define noundef i64 @int_hash(ptr noundef %k) #0 {
entry:
  %k1 = alloca ptr, align 8
  store ptr %k, ptr %k1, align 8
  %0 = load ptr, ptr %k1, align 8
  %1 = getelementptr i32, ptr %0, i64 0
  %2 = load i32, ptr %1, align 4
  %3 = sext i32 %2 to i64
  %4 = mul i64 %3, 2654435761
  ret i64 %4
}

; Function Attrs: nounwind
define noundef i32 @int_eq(ptr noundef %a, ptr noundef %b) #0 {
entry:
  %b2 = alloca ptr, align 8
  %a1 = alloca ptr, align 8
  store ptr %a, ptr %a1, align 8
  store ptr %b, ptr %b2, align 8
  %0 = load ptr, ptr %a1, align 8
  %1 = getelementptr i32, ptr %0, i64 0
  %2 = load i32, ptr %1, align 4
  %3 = load ptr, ptr %b2, align 8
  %4 = getelementptr i32, ptr %3, i64 0
  %5 = load i32, ptr %4, align 4
  %6 = icmp eq i32 %2, %5
  br i1 %6, label %tern.then, label %tern.else

tern.then:                                        ; preds = %entry
  br label %tern.cont

tern.else:                                        ; preds = %entry
  br label %tern.cont

tern.cont:                                        ; preds = %tern.else, %tern.then
  %7 = phi i32 [ 1, %tern.then ], [ 0, %tern.else ]
  ret i32 %7
}

; Function Attrs: nounwind
define noundef i64 @cstr_hash(ptr noundef %k) #0 {
entry:
  %i = alloca i32, align 4
  %h = alloca i64, align 8
  %s = alloca ptr, align 8
  %k1 = alloca ptr, align 8
  store ptr %k, ptr %k1, align 8
  %0 = load ptr, ptr %k1, align 8
  %1 = getelementptr ptr, ptr %0, i64 0
  %2 = load ptr, ptr %1, align 8
  store ptr %2, ptr %s, align 8
  store i64 -3750763034362895579, ptr %h, align 8
  store i32 0, ptr %i, align 4
  br label %for

for:                                              ; preds = %for_step, %entry
  %3 = load i32, ptr %i, align 4
  %4 = sext i32 %3 to i64
  %5 = load ptr, ptr %s, align 8
  %6 = getelementptr i8, ptr %5, i64 %4
  %7 = load i8, ptr %6, align 1
  %8 = zext i8 %7 to i32
  %9 = icmp ne i32 %8, 0
  br i1 %9, label %for_body, label %for_exit

for_body:                                         ; preds = %for
  %10 = load i64, ptr %h, align 8
  %11 = load i32, ptr %i, align 4
  %12 = sext i32 %11 to i64
  %13 = load ptr, ptr %s, align 8
  %14 = getelementptr i8, ptr %13, i64 %12
  %15 = load i8, ptr %14, align 1
  %16 = zext i8 %15 to i32
  %17 = and i32 %16, 255
  %18 = sext i32 %17 to i64
  %19 = xor i64 %10, %18
  %20 = mul i64 %19, 1099511628211
  store i64 %20, ptr %h, align 8
  br label %for_step

for_step:                                         ; preds = %for_body
  %21 = load i32, ptr %i, align 4
  %22 = add i32 %21, 1
  store i32 %22, ptr %i, align 4
  br label %for

for_exit:                                         ; preds = %for
  %23 = load i64, ptr %h, align 8
  ret i64 %23
}

; Function Attrs: nounwind
define noundef i32 @cstr_eq(ptr noundef %a, ptr noundef %b) #0 {
entry:
  %b2 = alloca ptr, align 8
  %a1 = alloca ptr, align 8
  store ptr %a, ptr %a1, align 8
  store ptr %b, ptr %b2, align 8
  %0 = load ptr, ptr %a1, align 8
  %1 = getelementptr ptr, ptr %0, i64 0
  %2 = load ptr, ptr %1, align 8
  %3 = load ptr, ptr %b2, align 8
  %4 = getelementptr ptr, ptr %3, i64 0
  %5 = load ptr, ptr %4, align 8
  %6 = call i32 @_map_streq(ptr %2, ptr %5)
  ret i32 %6
}

; Function Attrs: nounwind
define noundef i64 @pt_hash(ptr noundef %p) #0 {
entry:
  %p1 = alloca ptr, align 8
  store ptr %p, ptr %p1, align 8
  %0 = load ptr, ptr %p1, align 8
  %x = getelementptr inbounds nuw %Pt, ptr %0, i32 0, i32 0
  %x2 = load i32, ptr %x, align 4
  %1 = mul i32 %x2, 31
  %2 = load ptr, ptr %p1, align 8
  %y = getelementptr inbounds nuw %Pt, ptr %2, i32 0, i32 1
  %y3 = load i32, ptr %y, align 4
  %3 = add i32 %1, %y3
  %4 = sext i32 %3 to i64
  ret i64 %4
}

; Function Attrs: nounwind
define noundef i32 @pt_eq(ptr noundef %a, ptr noundef %b) #0 {
entry:
  %b2 = alloca ptr, align 8
  %a1 = alloca ptr, align 8
  store ptr %a, ptr %a1, align 8
  store ptr %b, ptr %b2, align 8
  %0 = load ptr, ptr %a1, align 8
  %x = getelementptr inbounds nuw %Pt, ptr %0, i32 0, i32 0
  %x3 = load i32, ptr %x, align 4
  %1 = load ptr, ptr %b2, align 8
  %x4 = getelementptr inbounds nuw %Pt, ptr %1, i32 0, i32 0
  %x5 = load i32, ptr %x4, align 4
  %2 = icmp eq i32 %x3, %x5
  br i1 %2, label %sc.rhs, label %sc.cont

sc.rhs:                                           ; preds = %entry
  %3 = load ptr, ptr %a1, align 8
  %y = getelementptr inbounds nuw %Pt, ptr %3, i32 0, i32 1
  %y6 = load i32, ptr %y, align 4
  %4 = load ptr, ptr %b2, align 8
  %y7 = getelementptr inbounds nuw %Pt, ptr %4, i32 0, i32 1
  %y8 = load i32, ptr %y7, align 4
  %5 = icmp eq i32 %y6, %y8
  br label %sc.cont

sc.cont:                                          ; preds = %sc.rhs, %entry
  %6 = phi i1 [ false, %entry ], [ %5, %sc.rhs ]
  br i1 %6, label %then, label %merge

then:                                             ; preds = %sc.cont
  ret i32 1

merge:                                            ; preds = %sc.cont
  ret i32 0
}

define noundef i32 @main() {
entry:
  %got = alloca i32, align 4
  %q = alloca %Pt, align 8
  %s2 = alloca ptr, align 8
  %s1 = alloca ptr, align 8
  %c2 = alloca i32, align 4
  %p2 = alloca %Pt, align 8
  %p1 = alloca %Pt, align 8
  %pm = alloca %HashMap_Pt_int, align 8
  %v = alloca i32, align 4
  %s = alloca ptr, align 8
  %c = alloca i32, align 4
  %k = alloca i32, align 4
  %i = alloca i32, align 4
  %m = alloca %HashMap_int_int, align 8
  store %HashMap_int_int zeroinitializer, ptr %m, align 8
  call void @HashMap_init_int_int(ptr %m, i32 8, { ptr, ptr } { ptr @__fnptr_int_hash, ptr null }, { ptr, ptr } { ptr @__fnptr_int_eq, ptr null })
  store i32 0, ptr %i, align 4
  br label %while

while:                                            ; preds = %merge, %entry
  %0 = load i32, ptr %i, align 4
  %1 = icmp slt i32 %0, 50
  br i1 %1, label %while_body, label %while_exit

while_body:                                       ; preds = %while
  %2 = load i32, ptr %i, align 4
  %3 = srem i32 %2, 10
  store i32 %3, ptr %k, align 4
  store i32 0, ptr %c, align 4
  %4 = load i32, ptr %k, align 4
  %5 = call ptr @HashMap_at_int_int(ptr %m, i32 %4, ptr %c)
  store ptr %5, ptr %s, align 8
  %6 = load i32, ptr %c, align 4
  %7 = icmp eq i32 %6, 1
  br i1 %7, label %then, label %merge

while_exit:                                       ; preds = %while
  store i32 0, ptr %v, align 4
  %8 = call i32 @HashMap_get_int_int(ptr %m, i32 3, ptr %v)
  %9 = load i32, ptr %v, align 4
  %count = getelementptr inbounds nuw %HashMap_int_int, ptr %m, i32 0, i32 4
  %count1 = load i32, ptr %count, align 4
  %10 = call i32 (ptr, ...) @printf(ptr @0, i32 %9, i32 %count1)
  store %HashMap_Pt_int zeroinitializer, ptr %pm, align 8
  call void @HashMap_init_Pt_int(ptr %pm, i32 8, { ptr, ptr } { ptr @__fnptr_pt_hash, ptr null }, { ptr, ptr } { ptr @__fnptr_pt_eq, ptr null })
  store %Pt zeroinitializer, ptr %p1, align 4
  %11 = getelementptr inbounds nuw %Pt, ptr %p1, i32 0, i32 0
  store i32 1, ptr %11, align 4
  %12 = getelementptr inbounds nuw %Pt, ptr %p1, i32 0, i32 1
  store i32 2, ptr %12, align 4
  store %Pt zeroinitializer, ptr %p2, align 4
  %13 = getelementptr inbounds nuw %Pt, ptr %p2, i32 0, i32 0
  store i32 3, ptr %13, align 4
  %14 = getelementptr inbounds nuw %Pt, ptr %p2, i32 0, i32 1
  store i32 4, ptr %14, align 4
  store i32 0, ptr %c2, align 4
  %15 = load %Pt, ptr %p1, align 4
  %16 = call ptr @HashMap_at_Pt_int(ptr %pm, %Pt %15, ptr %c2)
  store ptr %16, ptr %s1, align 8
  %17 = load ptr, ptr %s1, align 8
  %18 = getelementptr i32, ptr %17, i64 0
  store i32 100, ptr %18, align 4
  %19 = load %Pt, ptr %p2, align 4
  %20 = call ptr @HashMap_at_Pt_int(ptr %pm, %Pt %19, ptr %c2)
  store ptr %20, ptr %s2, align 8
  %21 = load ptr, ptr %s2, align 8
  %22 = getelementptr i32, ptr %21, i64 0
  store i32 200, ptr %22, align 4
  store %Pt zeroinitializer, ptr %q, align 4
  %23 = getelementptr inbounds nuw %Pt, ptr %q, i32 0, i32 0
  store i32 1, ptr %23, align 4
  %24 = getelementptr inbounds nuw %Pt, ptr %q, i32 0, i32 1
  store i32 2, ptr %24, align 4
  store i32 0, ptr %got, align 4
  %25 = load %Pt, ptr %q, align 4
  %26 = call i32 @HashMap_get_Pt_int(ptr %pm, %Pt %25, ptr %got)
  %27 = load i32, ptr %got, align 4
  %count3 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %pm, i32 0, i32 4
  %count4 = load i32, ptr %count3, align 4
  %28 = call i32 (ptr, ...) @printf(ptr @1, i32 %27, i32 %count4)
  call void @HashMap_free_int_int(ptr %m)
  call void @HashMap_free_Pt_int(ptr %pm)
  ret i32 0

then:                                             ; preds = %while_body
  %29 = load ptr, ptr %s, align 8
  %30 = getelementptr i32, ptr %29, i64 0
  store i32 0, ptr %30, align 4
  br label %merge

merge:                                            ; preds = %then, %while_body
  %31 = load ptr, ptr %s, align 8
  %32 = getelementptr i32, ptr %31, i64 0
  %33 = load ptr, ptr %s, align 8
  %34 = getelementptr i32, ptr %33, i64 0
  %35 = load i32, ptr %34, align 4
  %36 = add i32 %35, 1
  store i32 %36, ptr %32, align 4
  %37 = load i32, ptr %i, align 4
  %38 = add i32 %37, 1
  store i32 %38, ptr %i, align 4
  br label %while
}

define noundef ptr @alloc_char(i64 noundef %n) {
entry:
  %n1 = alloca i64, align 8
  store i64 %n, ptr %n1, align 8
  %0 = load i64, ptr %n1, align 8
  %1 = call ptr @calloc(i64 %0, i64 1)
  ret ptr %1
}

define void @HashMap_init_int_int(ptr noundef %m, i32 noundef %cap, { ptr, ptr } %hash, { ptr, ptr } %eq) {
entry:
  %eq4 = alloca { ptr, ptr }, align 8
  %hash3 = alloca { ptr, ptr }, align 8
  %cap2 = alloca i32, align 4
  %m1 = alloca ptr, align 8
  store ptr %m, ptr %m1, align 8
  store i32 %cap, ptr %cap2, align 4
  store { ptr, ptr } %hash, ptr %hash3, align 8
  store { ptr, ptr } %eq, ptr %eq4, align 8
  %0 = load i32, ptr %cap2, align 4
  %1 = icmp slt i32 %0, 8
  br i1 %1, label %then, label %merge

then:                                             ; preds = %entry
  store i32 8, ptr %cap2, align 4
  br label %merge

merge:                                            ; preds = %then, %entry
  %2 = load ptr, ptr %m1, align 8
  %3 = getelementptr inbounds nuw %HashMap_int_int, ptr %2, i32 0, i32 0
  %4 = load i32, ptr %cap2, align 4
  %5 = sext i32 %4 to i64
  %6 = call ptr @alloc_int(i64 %5)
  store ptr %6, ptr %3, align 8
  %7 = load ptr, ptr %m1, align 8
  %8 = getelementptr inbounds nuw %HashMap_int_int, ptr %7, i32 0, i32 1
  %9 = load i32, ptr %cap2, align 4
  %10 = sext i32 %9 to i64
  %11 = call ptr @alloc_int(i64 %10)
  store ptr %11, ptr %8, align 8
  %12 = load ptr, ptr %m1, align 8
  %13 = getelementptr inbounds nuw %HashMap_int_int, ptr %12, i32 0, i32 2
  %14 = load i32, ptr %cap2, align 4
  %15 = sext i32 %14 to i64
  %16 = call ptr @alloc_uint8(i64 %15)
  store ptr %16, ptr %13, align 8
  %17 = load ptr, ptr %m1, align 8
  %18 = getelementptr inbounds nuw %HashMap_int_int, ptr %17, i32 0, i32 3
  %19 = load i32, ptr %cap2, align 4
  store i32 %19, ptr %18, align 4
  %20 = load ptr, ptr %m1, align 8
  %21 = getelementptr inbounds nuw %HashMap_int_int, ptr %20, i32 0, i32 4
  store i32 0, ptr %21, align 4
  %22 = load ptr, ptr %m1, align 8
  %23 = getelementptr inbounds nuw %HashMap_int_int, ptr %22, i32 0, i32 5
  %24 = load { ptr, ptr }, ptr %hash3, align 8
  store { ptr, ptr } %24, ptr %23, align 8
  %25 = load ptr, ptr %m1, align 8
  %26 = getelementptr inbounds nuw %HashMap_int_int, ptr %25, i32 0, i32 6
  %27 = load { ptr, ptr }, ptr %eq4, align 8
  store { ptr, ptr } %27, ptr %26, align 8
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

define noundef ptr @alloc_uint8(i64 noundef %n) {
entry:
  %n1 = alloca i64, align 8
  store i64 %n, ptr %n1, align 8
  %0 = load i64, ptr %n1, align 8
  %1 = call ptr @calloc(i64 %0, i64 1)
  ret ptr %1
}

; Function Attrs: nounwind
define internal i64 @__fnptr_int_hash(ptr %0, ptr %1) #0 {
entry:
  %2 = call i64 @int_hash(ptr %1)
  ret i64 %2
}

; Function Attrs: nounwind
define internal i32 @__fnptr_int_eq(ptr %0, ptr %1, ptr %2) #0 {
entry:
  %3 = call i32 @int_eq(ptr %1, ptr %2)
  ret i32 %3
}

define noundef ptr @HashMap_at_int_int(ptr noundef %m, i32 noundef %key, ptr noundef %created) {
entry:
  %i = alloca i32, align 4
  %h = alloca i64, align 8
  %eqfn = alloca { ptr, ptr }, align 8
  %hashfn = alloca { ptr, ptr }, align 8
  %created3 = alloca ptr, align 8
  %key2 = alloca i32, align 4
  %m1 = alloca ptr, align 8
  store ptr %m, ptr %m1, align 8
  store i32 %key, ptr %key2, align 4
  store ptr %created, ptr %created3, align 8
  %0 = load ptr, ptr %m1, align 8
  %count = getelementptr inbounds nuw %HashMap_int_int, ptr %0, i32 0, i32 4
  %count4 = load i32, ptr %count, align 4
  %1 = add i32 %count4, 1
  %2 = mul i32 %1, 4
  %3 = load ptr, ptr %m1, align 8
  %cap = getelementptr inbounds nuw %HashMap_int_int, ptr %3, i32 0, i32 3
  %cap5 = load i32, ptr %cap, align 4
  %4 = mul i32 %cap5, 3
  %5 = icmp sge i32 %2, %4
  br i1 %5, label %then, label %merge

then:                                             ; preds = %entry
  %6 = load ptr, ptr %m1, align 8
  call void @_HashMap_grow_int_int(ptr %6)
  br label %merge

merge:                                            ; preds = %then, %entry
  %7 = load ptr, ptr %m1, align 8
  %hash = getelementptr inbounds nuw %HashMap_int_int, ptr %7, i32 0, i32 5
  %hash6 = load { ptr, ptr }, ptr %hash, align 8
  store { ptr, ptr } %hash6, ptr %hashfn, align 8
  %8 = load ptr, ptr %m1, align 8
  %eq = getelementptr inbounds nuw %HashMap_int_int, ptr %8, i32 0, i32 6
  %eq7 = load { ptr, ptr }, ptr %eq, align 8
  store { ptr, ptr } %eq7, ptr %eqfn, align 8
  %9 = load { ptr, ptr }, ptr %hashfn, align 8
  %fn.ptr = extractvalue { ptr, ptr } %9, 0
  %env.ptr = extractvalue { ptr, ptr } %9, 1
  %fn.call = call i64 %fn.ptr(ptr %env.ptr, ptr %key2)
  store i64 %fn.call, ptr %h, align 8
  %10 = load i64, ptr %h, align 8
  %11 = load ptr, ptr %m1, align 8
  %cap8 = getelementptr inbounds nuw %HashMap_int_int, ptr %11, i32 0, i32 3
  %cap9 = load i32, ptr %cap8, align 4
  %12 = sext i32 %cap9 to i64
  %13 = icmp eq i64 %12, 0
  br i1 %13, label %div.zero, label %div.ok

div.zero:                                         ; preds = %merge
  call void @llvm.trap()
  unreachable

div.ok:                                           ; preds = %merge
  %14 = urem i64 %10, %12
  %15 = trunc i64 %14 to i32
  store i32 %15, ptr %i, align 4
  br label %while

while:                                            ; preds = %div.ok21, %div.ok
  %16 = load i32, ptr %i, align 4
  %17 = sext i32 %16 to i64
  %18 = load ptr, ptr %m1, align 8
  %used = getelementptr inbounds nuw %HashMap_int_int, ptr %18, i32 0, i32 2
  %used10 = load ptr, ptr %used, align 8
  %19 = getelementptr i8, ptr %used10, i64 %17
  %20 = load i8, ptr %19, align 1
  %21 = zext i8 %20 to i32
  %22 = icmp eq i32 %21, 1
  br i1 %22, label %while_body, label %while_exit

while_body:                                       ; preds = %while
  %23 = load { ptr, ptr }, ptr %eqfn, align 8
  %fn.ptr11 = extractvalue { ptr, ptr } %23, 0
  %env.ptr12 = extractvalue { ptr, ptr } %23, 1
  %24 = load i32, ptr %i, align 4
  %25 = sext i32 %24 to i64
  %26 = load ptr, ptr %m1, align 8
  %keys = getelementptr inbounds nuw %HashMap_int_int, ptr %26, i32 0, i32 0
  %keys13 = load ptr, ptr %keys, align 8
  %27 = getelementptr i32, ptr %keys13, i64 %25
  %fn.call14 = call i32 %fn.ptr11(ptr %env.ptr12, ptr %key2, ptr %27)
  %28 = icmp eq i32 %fn.call14, 1
  br i1 %28, label %then15, label %merge16

while_exit:                                       ; preds = %while
  %29 = load i32, ptr %i, align 4
  %30 = sext i32 %29 to i64
  %31 = load ptr, ptr %m1, align 8
  %used22 = getelementptr inbounds nuw %HashMap_int_int, ptr %31, i32 0, i32 2
  %used23 = load ptr, ptr %used22, align 8
  %32 = getelementptr i8, ptr %used23, i64 %30
  store i8 1, ptr %32, align 1
  %33 = load i32, ptr %i, align 4
  %34 = sext i32 %33 to i64
  %35 = load ptr, ptr %m1, align 8
  %keys24 = getelementptr inbounds nuw %HashMap_int_int, ptr %35, i32 0, i32 0
  %keys25 = load ptr, ptr %keys24, align 8
  %36 = getelementptr i32, ptr %keys25, i64 %34
  %37 = load i32, ptr %key2, align 4
  store i32 %37, ptr %36, align 4
  %38 = load ptr, ptr %m1, align 8
  %39 = getelementptr inbounds nuw %HashMap_int_int, ptr %38, i32 0, i32 4
  %40 = load i32, ptr %39, align 4
  %41 = add i32 %40, 1
  store i32 %41, ptr %39, align 4
  %42 = load ptr, ptr %created3, align 8
  %43 = getelementptr i32, ptr %42, i64 0
  store i32 1, ptr %43, align 4
  %44 = load i32, ptr %i, align 4
  %45 = sext i32 %44 to i64
  %46 = load ptr, ptr %m1, align 8
  %vals26 = getelementptr inbounds nuw %HashMap_int_int, ptr %46, i32 0, i32 1
  %vals27 = load ptr, ptr %vals26, align 8
  %47 = getelementptr i32, ptr %vals27, i64 %45
  ret ptr %47

then15:                                           ; preds = %while_body
  %48 = load ptr, ptr %created3, align 8
  %49 = getelementptr i32, ptr %48, i64 0
  store i32 0, ptr %49, align 4
  %50 = load i32, ptr %i, align 4
  %51 = sext i32 %50 to i64
  %52 = load ptr, ptr %m1, align 8
  %vals = getelementptr inbounds nuw %HashMap_int_int, ptr %52, i32 0, i32 1
  %vals17 = load ptr, ptr %vals, align 8
  %53 = getelementptr i32, ptr %vals17, i64 %51
  ret ptr %53

merge16:                                          ; preds = %while_body
  %54 = load i32, ptr %i, align 4
  %55 = add i32 %54, 1
  %56 = load ptr, ptr %m1, align 8
  %cap18 = getelementptr inbounds nuw %HashMap_int_int, ptr %56, i32 0, i32 3
  %cap19 = load i32, ptr %cap18, align 4
  %57 = icmp eq i32 %cap19, 0
  br i1 %57, label %div.zero20, label %div.ok21

div.zero20:                                       ; preds = %merge16
  call void @llvm.trap()
  unreachable

div.ok21:                                         ; preds = %merge16
  %58 = icmp eq i32 %cap19, -1
  %59 = select i1 %58, i32 1, i32 %cap19
  %60 = srem i32 %55, %59
  store i32 %60, ptr %i, align 4
  br label %while
}

define void @_HashMap_grow_int_int(ptr noundef %m) {
entry:
  %j = alloca i32, align 4
  %h = alloca i64, align 8
  %__end_i = alloca i32, align 4
  %i = alloca i32, align 4
  %hashfn = alloca { ptr, ptr }, align 8
  %nu = alloca ptr, align 8
  %nv = alloca ptr, align 8
  %nk = alloca ptr, align 8
  %newcap = alloca i32, align 4
  %ou = alloca ptr, align 8
  %ov = alloca ptr, align 8
  %ok = alloca ptr, align 8
  %oldcap = alloca i32, align 4
  %m1 = alloca ptr, align 8
  store ptr %m, ptr %m1, align 8
  %0 = load ptr, ptr %m1, align 8
  %cap = getelementptr inbounds nuw %HashMap_int_int, ptr %0, i32 0, i32 3
  %cap2 = load i32, ptr %cap, align 4
  store i32 %cap2, ptr %oldcap, align 4
  %1 = load ptr, ptr %m1, align 8
  %keys = getelementptr inbounds nuw %HashMap_int_int, ptr %1, i32 0, i32 0
  %keys3 = load ptr, ptr %keys, align 8
  store ptr %keys3, ptr %ok, align 8
  %2 = load ptr, ptr %m1, align 8
  %vals = getelementptr inbounds nuw %HashMap_int_int, ptr %2, i32 0, i32 1
  %vals4 = load ptr, ptr %vals, align 8
  store ptr %vals4, ptr %ov, align 8
  %3 = load ptr, ptr %m1, align 8
  %used = getelementptr inbounds nuw %HashMap_int_int, ptr %3, i32 0, i32 2
  %used5 = load ptr, ptr %used, align 8
  store ptr %used5, ptr %ou, align 8
  %4 = load i32, ptr %oldcap, align 4
  %5 = icmp slt i32 %4, 4
  br i1 %5, label %tern.then, label %tern.else

tern.then:                                        ; preds = %entry
  br label %tern.cont

tern.else:                                        ; preds = %entry
  %6 = load i32, ptr %oldcap, align 4
  %7 = mul i32 %6, 2
  br label %tern.cont

tern.cont:                                        ; preds = %tern.else, %tern.then
  %8 = phi i32 [ 8, %tern.then ], [ %7, %tern.else ]
  store i32 %8, ptr %newcap, align 4
  %9 = load i32, ptr %newcap, align 4
  %10 = sext i32 %9 to i64
  %11 = call ptr @alloc_int(i64 %10)
  store ptr %11, ptr %nk, align 8
  %12 = load i32, ptr %newcap, align 4
  %13 = sext i32 %12 to i64
  %14 = call ptr @alloc_int(i64 %13)
  store ptr %14, ptr %nv, align 8
  %15 = load i32, ptr %newcap, align 4
  %16 = sext i32 %15 to i64
  %17 = call ptr @alloc_uint8(i64 %16)
  store ptr %17, ptr %nu, align 8
  %18 = load ptr, ptr %m1, align 8
  %hash = getelementptr inbounds nuw %HashMap_int_int, ptr %18, i32 0, i32 5
  %hash6 = load { ptr, ptr }, ptr %hash, align 8
  store { ptr, ptr } %hash6, ptr %hashfn, align 8
  store i32 0, ptr %i, align 4
  %19 = load i32, ptr %oldcap, align 4
  store i32 %19, ptr %__end_i, align 4
  br label %for

for:                                              ; preds = %for_step, %tern.cont
  %20 = load i32, ptr %i, align 4
  %21 = load i32, ptr %__end_i, align 4
  %22 = icmp slt i32 %20, %21
  br i1 %22, label %for_body, label %for_exit

for_body:                                         ; preds = %for
  %23 = load i32, ptr %i, align 4
  %24 = sext i32 %23 to i64
  %25 = load ptr, ptr %ou, align 8
  %26 = getelementptr i8, ptr %25, i64 %24
  %27 = load i8, ptr %26, align 1
  %28 = zext i8 %27 to i32
  %29 = icmp eq i32 %28, 1
  br i1 %29, label %then, label %merge

for_step:                                         ; preds = %merge
  %30 = load i32, ptr %i, align 4
  %31 = add i32 %30, 1
  store i32 %31, ptr %i, align 4
  br label %for

for_exit:                                         ; preds = %for
  %32 = load ptr, ptr %ok, align 8
  call void @free(ptr %32)
  %33 = load ptr, ptr %ov, align 8
  call void @free(ptr %33)
  %34 = load ptr, ptr %ou, align 8
  call void @free(ptr %34)
  %35 = load ptr, ptr %m1, align 8
  %36 = getelementptr inbounds nuw %HashMap_int_int, ptr %35, i32 0, i32 0
  %37 = load ptr, ptr %nk, align 8
  store ptr %37, ptr %36, align 8
  %38 = load ptr, ptr %m1, align 8
  %39 = getelementptr inbounds nuw %HashMap_int_int, ptr %38, i32 0, i32 1
  %40 = load ptr, ptr %nv, align 8
  store ptr %40, ptr %39, align 8
  %41 = load ptr, ptr %m1, align 8
  %42 = getelementptr inbounds nuw %HashMap_int_int, ptr %41, i32 0, i32 2
  %43 = load ptr, ptr %nu, align 8
  store ptr %43, ptr %42, align 8
  %44 = load ptr, ptr %m1, align 8
  %45 = getelementptr inbounds nuw %HashMap_int_int, ptr %44, i32 0, i32 3
  %46 = load i32, ptr %newcap, align 4
  store i32 %46, ptr %45, align 4
  ret void

then:                                             ; preds = %for_body
  %47 = load { ptr, ptr }, ptr %hashfn, align 8
  %fn.ptr = extractvalue { ptr, ptr } %47, 0
  %env.ptr = extractvalue { ptr, ptr } %47, 1
  %48 = load i32, ptr %i, align 4
  %49 = sext i32 %48 to i64
  %50 = load ptr, ptr %ok, align 8
  %51 = getelementptr i32, ptr %50, i64 %49
  %fn.call = call i64 %fn.ptr(ptr %env.ptr, ptr %51)
  store i64 %fn.call, ptr %h, align 8
  %52 = load i64, ptr %h, align 8
  %53 = load i32, ptr %newcap, align 4
  %54 = sext i32 %53 to i64
  %55 = icmp eq i64 %54, 0
  br i1 %55, label %div.zero, label %div.ok

merge:                                            ; preds = %while_exit, %for_body
  br label %for_step

div.zero:                                         ; preds = %then
  call void @llvm.trap()
  unreachable

div.ok:                                           ; preds = %then
  %56 = urem i64 %52, %54
  %57 = trunc i64 %56 to i32
  store i32 %57, ptr %j, align 4
  br label %while

while:                                            ; preds = %div.ok8, %div.ok
  %58 = load i32, ptr %j, align 4
  %59 = sext i32 %58 to i64
  %60 = load ptr, ptr %nu, align 8
  %61 = getelementptr i8, ptr %60, i64 %59
  %62 = load i8, ptr %61, align 1
  %63 = zext i8 %62 to i32
  %64 = icmp eq i32 %63, 1
  br i1 %64, label %while_body, label %while_exit

while_body:                                       ; preds = %while
  %65 = load i32, ptr %j, align 4
  %66 = add i32 %65, 1
  %67 = load i32, ptr %newcap, align 4
  %68 = icmp eq i32 %67, 0
  br i1 %68, label %div.zero7, label %div.ok8

while_exit:                                       ; preds = %while
  %69 = load i32, ptr %j, align 4
  %70 = sext i32 %69 to i64
  %71 = load ptr, ptr %nu, align 8
  %72 = getelementptr i8, ptr %71, i64 %70
  store i8 1, ptr %72, align 1
  %73 = load i32, ptr %j, align 4
  %74 = sext i32 %73 to i64
  %75 = load ptr, ptr %nk, align 8
  %76 = getelementptr i32, ptr %75, i64 %74
  %77 = load i32, ptr %i, align 4
  %78 = sext i32 %77 to i64
  %79 = load ptr, ptr %ok, align 8
  %80 = getelementptr i32, ptr %79, i64 %78
  %81 = load i32, ptr %80, align 4
  store i32 %81, ptr %76, align 4
  %82 = load i32, ptr %j, align 4
  %83 = sext i32 %82 to i64
  %84 = load ptr, ptr %nv, align 8
  %85 = getelementptr i32, ptr %84, i64 %83
  %86 = load i32, ptr %i, align 4
  %87 = sext i32 %86 to i64
  %88 = load ptr, ptr %ov, align 8
  %89 = getelementptr i32, ptr %88, i64 %87
  %90 = load i32, ptr %89, align 4
  store i32 %90, ptr %85, align 4
  br label %merge

div.zero7:                                        ; preds = %while_body
  call void @llvm.trap()
  unreachable

div.ok8:                                          ; preds = %while_body
  %91 = icmp eq i32 %67, -1
  %92 = select i1 %91, i32 1, i32 %67
  %93 = srem i32 %66, %92
  store i32 %93, ptr %j, align 4
  br label %while
}

; Function Attrs: cold noreturn nounwind memory(inaccessiblemem: write)
declare void @llvm.trap() #1

define noundef i32 @HashMap_get_int_int(ptr noundef %m, i32 noundef %key, ptr noundef %out) {
entry:
  %n = alloca i32, align 4
  %i = alloca i32, align 4
  %h = alloca i64, align 8
  %eqfn = alloca { ptr, ptr }, align 8
  %hashfn = alloca { ptr, ptr }, align 8
  %out3 = alloca ptr, align 8
  %key2 = alloca i32, align 4
  %m1 = alloca ptr, align 8
  store ptr %m, ptr %m1, align 8
  store i32 %key, ptr %key2, align 4
  store ptr %out, ptr %out3, align 8
  %0 = load ptr, ptr %m1, align 8
  %cap = getelementptr inbounds nuw %HashMap_int_int, ptr %0, i32 0, i32 3
  %cap4 = load i32, ptr %cap, align 4
  %1 = icmp eq i32 %cap4, 0
  br i1 %1, label %then, label %merge

then:                                             ; preds = %entry
  ret i32 0

merge:                                            ; preds = %entry
  %2 = load ptr, ptr %m1, align 8
  %hash = getelementptr inbounds nuw %HashMap_int_int, ptr %2, i32 0, i32 5
  %hash5 = load { ptr, ptr }, ptr %hash, align 8
  store { ptr, ptr } %hash5, ptr %hashfn, align 8
  %3 = load ptr, ptr %m1, align 8
  %eq = getelementptr inbounds nuw %HashMap_int_int, ptr %3, i32 0, i32 6
  %eq6 = load { ptr, ptr }, ptr %eq, align 8
  store { ptr, ptr } %eq6, ptr %eqfn, align 8
  %4 = load { ptr, ptr }, ptr %hashfn, align 8
  %fn.ptr = extractvalue { ptr, ptr } %4, 0
  %env.ptr = extractvalue { ptr, ptr } %4, 1
  %fn.call = call i64 %fn.ptr(ptr %env.ptr, ptr %key2)
  store i64 %fn.call, ptr %h, align 8
  %5 = load i64, ptr %h, align 8
  %6 = load ptr, ptr %m1, align 8
  %cap7 = getelementptr inbounds nuw %HashMap_int_int, ptr %6, i32 0, i32 3
  %cap8 = load i32, ptr %cap7, align 4
  %7 = sext i32 %cap8 to i64
  %8 = icmp eq i64 %7, 0
  br i1 %8, label %div.zero, label %div.ok

div.zero:                                         ; preds = %merge
  call void @llvm.trap()
  unreachable

div.ok:                                           ; preds = %merge
  %9 = urem i64 %5, %7
  %10 = trunc i64 %9 to i32
  store i32 %10, ptr %i, align 4
  store i32 0, ptr %n, align 4
  br label %for

for:                                              ; preds = %for_step, %div.ok
  %11 = load i32, ptr %i, align 4
  %12 = sext i32 %11 to i64
  %13 = load ptr, ptr %m1, align 8
  %used = getelementptr inbounds nuw %HashMap_int_int, ptr %13, i32 0, i32 2
  %used9 = load ptr, ptr %used, align 8
  %14 = getelementptr i8, ptr %used9, i64 %12
  %15 = load i8, ptr %14, align 1
  %16 = zext i8 %15 to i32
  %17 = icmp eq i32 %16, 1
  br i1 %17, label %sc.rhs, label %sc.cont

for_body:                                         ; preds = %sc.cont
  %18 = load { ptr, ptr }, ptr %eqfn, align 8
  %fn.ptr12 = extractvalue { ptr, ptr } %18, 0
  %env.ptr13 = extractvalue { ptr, ptr } %18, 1
  %19 = load i32, ptr %i, align 4
  %20 = sext i32 %19 to i64
  %21 = load ptr, ptr %m1, align 8
  %keys = getelementptr inbounds nuw %HashMap_int_int, ptr %21, i32 0, i32 0
  %keys14 = load ptr, ptr %keys, align 8
  %22 = getelementptr i32, ptr %keys14, i64 %20
  %fn.call15 = call i32 %fn.ptr12(ptr %env.ptr13, ptr %key2, ptr %22)
  %23 = icmp eq i32 %fn.call15, 1
  br i1 %23, label %then16, label %merge17

for_step:                                         ; preds = %div.ok22
  %24 = load i32, ptr %n, align 4
  %25 = add i32 %24, 1
  store i32 %25, ptr %n, align 4
  br label %for

for_exit:                                         ; preds = %sc.cont
  ret i32 0

sc.rhs:                                           ; preds = %for
  %26 = load i32, ptr %n, align 4
  %27 = load ptr, ptr %m1, align 8
  %cap10 = getelementptr inbounds nuw %HashMap_int_int, ptr %27, i32 0, i32 3
  %cap11 = load i32, ptr %cap10, align 4
  %28 = icmp slt i32 %26, %cap11
  br label %sc.cont

sc.cont:                                          ; preds = %sc.rhs, %for
  %29 = phi i1 [ false, %for ], [ %28, %sc.rhs ]
  br i1 %29, label %for_body, label %for_exit

then16:                                           ; preds = %for_body
  %30 = load ptr, ptr %out3, align 8
  %31 = getelementptr i32, ptr %30, i64 0
  %32 = load i32, ptr %i, align 4
  %33 = sext i32 %32 to i64
  %34 = load ptr, ptr %m1, align 8
  %vals = getelementptr inbounds nuw %HashMap_int_int, ptr %34, i32 0, i32 1
  %vals18 = load ptr, ptr %vals, align 8
  %35 = getelementptr i32, ptr %vals18, i64 %33
  %36 = load i32, ptr %35, align 4
  store i32 %36, ptr %31, align 4
  ret i32 1

merge17:                                          ; preds = %for_body
  %37 = load i32, ptr %i, align 4
  %38 = add i32 %37, 1
  %39 = load ptr, ptr %m1, align 8
  %cap19 = getelementptr inbounds nuw %HashMap_int_int, ptr %39, i32 0, i32 3
  %cap20 = load i32, ptr %cap19, align 4
  %40 = icmp eq i32 %cap20, 0
  br i1 %40, label %div.zero21, label %div.ok22

div.zero21:                                       ; preds = %merge17
  call void @llvm.trap()
  unreachable

div.ok22:                                         ; preds = %merge17
  %41 = icmp eq i32 %cap20, -1
  %42 = select i1 %41, i32 1, i32 %cap20
  %43 = srem i32 %38, %42
  store i32 %43, ptr %i, align 4
  br label %for_step
}

define void @HashMap_init_Pt_int(ptr noundef %m, i32 noundef %cap, { ptr, ptr } %hash, { ptr, ptr } %eq) {
entry:
  %eq4 = alloca { ptr, ptr }, align 8
  %hash3 = alloca { ptr, ptr }, align 8
  %cap2 = alloca i32, align 4
  %m1 = alloca ptr, align 8
  store ptr %m, ptr %m1, align 8
  store i32 %cap, ptr %cap2, align 4
  store { ptr, ptr } %hash, ptr %hash3, align 8
  store { ptr, ptr } %eq, ptr %eq4, align 8
  %0 = load i32, ptr %cap2, align 4
  %1 = icmp slt i32 %0, 8
  br i1 %1, label %then, label %merge

then:                                             ; preds = %entry
  store i32 8, ptr %cap2, align 4
  br label %merge

merge:                                            ; preds = %then, %entry
  %2 = load ptr, ptr %m1, align 8
  %3 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %2, i32 0, i32 0
  %4 = load i32, ptr %cap2, align 4
  %5 = sext i32 %4 to i64
  %6 = call ptr @alloc_Pt(i64 %5)
  store ptr %6, ptr %3, align 8
  %7 = load ptr, ptr %m1, align 8
  %8 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %7, i32 0, i32 1
  %9 = load i32, ptr %cap2, align 4
  %10 = sext i32 %9 to i64
  %11 = call ptr @alloc_int(i64 %10)
  store ptr %11, ptr %8, align 8
  %12 = load ptr, ptr %m1, align 8
  %13 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %12, i32 0, i32 2
  %14 = load i32, ptr %cap2, align 4
  %15 = sext i32 %14 to i64
  %16 = call ptr @alloc_uint8(i64 %15)
  store ptr %16, ptr %13, align 8
  %17 = load ptr, ptr %m1, align 8
  %18 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %17, i32 0, i32 3
  %19 = load i32, ptr %cap2, align 4
  store i32 %19, ptr %18, align 4
  %20 = load ptr, ptr %m1, align 8
  %21 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %20, i32 0, i32 4
  store i32 0, ptr %21, align 4
  %22 = load ptr, ptr %m1, align 8
  %23 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %22, i32 0, i32 5
  %24 = load { ptr, ptr }, ptr %hash3, align 8
  store { ptr, ptr } %24, ptr %23, align 8
  %25 = load ptr, ptr %m1, align 8
  %26 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %25, i32 0, i32 6
  %27 = load { ptr, ptr }, ptr %eq4, align 8
  store { ptr, ptr } %27, ptr %26, align 8
  ret void
}

define noundef ptr @alloc_Pt(i64 noundef %n) {
entry:
  %n1 = alloca i64, align 8
  store i64 %n, ptr %n1, align 8
  %0 = load i64, ptr %n1, align 8
  %1 = call ptr @calloc(i64 %0, i64 8)
  ret ptr %1
}

; Function Attrs: nounwind
define internal i64 @__fnptr_pt_hash(ptr %0, ptr %1) #0 {
entry:
  %2 = call i64 @pt_hash(ptr %1)
  ret i64 %2
}

; Function Attrs: nounwind
define internal i32 @__fnptr_pt_eq(ptr %0, ptr %1, ptr %2) #0 {
entry:
  %3 = call i32 @pt_eq(ptr %1, ptr %2)
  ret i32 %3
}

define noundef ptr @HashMap_at_Pt_int(ptr noundef %m, %Pt %key, ptr noundef %created) {
entry:
  %i = alloca i32, align 4
  %h = alloca i64, align 8
  %eqfn = alloca { ptr, ptr }, align 8
  %hashfn = alloca { ptr, ptr }, align 8
  %created3 = alloca ptr, align 8
  %key2 = alloca %Pt, align 8
  %m1 = alloca ptr, align 8
  store ptr %m, ptr %m1, align 8
  store %Pt %key, ptr %key2, align 4
  store ptr %created, ptr %created3, align 8
  %0 = load ptr, ptr %m1, align 8
  %count = getelementptr inbounds nuw %HashMap_Pt_int, ptr %0, i32 0, i32 4
  %count4 = load i32, ptr %count, align 4
  %1 = add i32 %count4, 1
  %2 = mul i32 %1, 4
  %3 = load ptr, ptr %m1, align 8
  %cap = getelementptr inbounds nuw %HashMap_Pt_int, ptr %3, i32 0, i32 3
  %cap5 = load i32, ptr %cap, align 4
  %4 = mul i32 %cap5, 3
  %5 = icmp sge i32 %2, %4
  br i1 %5, label %then, label %merge

then:                                             ; preds = %entry
  %6 = load ptr, ptr %m1, align 8
  call void @_HashMap_grow_Pt_int(ptr %6)
  br label %merge

merge:                                            ; preds = %then, %entry
  %7 = load ptr, ptr %m1, align 8
  %hash = getelementptr inbounds nuw %HashMap_Pt_int, ptr %7, i32 0, i32 5
  %hash6 = load { ptr, ptr }, ptr %hash, align 8
  store { ptr, ptr } %hash6, ptr %hashfn, align 8
  %8 = load ptr, ptr %m1, align 8
  %eq = getelementptr inbounds nuw %HashMap_Pt_int, ptr %8, i32 0, i32 6
  %eq7 = load { ptr, ptr }, ptr %eq, align 8
  store { ptr, ptr } %eq7, ptr %eqfn, align 8
  %9 = load { ptr, ptr }, ptr %hashfn, align 8
  %fn.ptr = extractvalue { ptr, ptr } %9, 0
  %env.ptr = extractvalue { ptr, ptr } %9, 1
  %fn.call = call i64 %fn.ptr(ptr %env.ptr, ptr %key2)
  store i64 %fn.call, ptr %h, align 8
  %10 = load i64, ptr %h, align 8
  %11 = load ptr, ptr %m1, align 8
  %cap8 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %11, i32 0, i32 3
  %cap9 = load i32, ptr %cap8, align 4
  %12 = sext i32 %cap9 to i64
  %13 = icmp eq i64 %12, 0
  br i1 %13, label %div.zero, label %div.ok

div.zero:                                         ; preds = %merge
  call void @llvm.trap()
  unreachable

div.ok:                                           ; preds = %merge
  %14 = urem i64 %10, %12
  %15 = trunc i64 %14 to i32
  store i32 %15, ptr %i, align 4
  br label %while

while:                                            ; preds = %div.ok21, %div.ok
  %16 = load i32, ptr %i, align 4
  %17 = sext i32 %16 to i64
  %18 = load ptr, ptr %m1, align 8
  %used = getelementptr inbounds nuw %HashMap_Pt_int, ptr %18, i32 0, i32 2
  %used10 = load ptr, ptr %used, align 8
  %19 = getelementptr i8, ptr %used10, i64 %17
  %20 = load i8, ptr %19, align 1
  %21 = zext i8 %20 to i32
  %22 = icmp eq i32 %21, 1
  br i1 %22, label %while_body, label %while_exit

while_body:                                       ; preds = %while
  %23 = load { ptr, ptr }, ptr %eqfn, align 8
  %fn.ptr11 = extractvalue { ptr, ptr } %23, 0
  %env.ptr12 = extractvalue { ptr, ptr } %23, 1
  %24 = load i32, ptr %i, align 4
  %25 = sext i32 %24 to i64
  %26 = load ptr, ptr %m1, align 8
  %keys = getelementptr inbounds nuw %HashMap_Pt_int, ptr %26, i32 0, i32 0
  %keys13 = load ptr, ptr %keys, align 8
  %27 = getelementptr %Pt, ptr %keys13, i64 %25
  %fn.call14 = call i32 %fn.ptr11(ptr %env.ptr12, ptr %key2, ptr %27)
  %28 = icmp eq i32 %fn.call14, 1
  br i1 %28, label %then15, label %merge16

while_exit:                                       ; preds = %while
  %29 = load i32, ptr %i, align 4
  %30 = sext i32 %29 to i64
  %31 = load ptr, ptr %m1, align 8
  %used22 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %31, i32 0, i32 2
  %used23 = load ptr, ptr %used22, align 8
  %32 = getelementptr i8, ptr %used23, i64 %30
  store i8 1, ptr %32, align 1
  %33 = load i32, ptr %i, align 4
  %34 = sext i32 %33 to i64
  %35 = load ptr, ptr %m1, align 8
  %keys24 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %35, i32 0, i32 0
  %keys25 = load ptr, ptr %keys24, align 8
  %36 = getelementptr %Pt, ptr %keys25, i64 %34
  %37 = load %Pt, ptr %key2, align 4
  store %Pt %37, ptr %36, align 4
  %38 = load ptr, ptr %m1, align 8
  %39 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %38, i32 0, i32 4
  %40 = load i32, ptr %39, align 4
  %41 = add i32 %40, 1
  store i32 %41, ptr %39, align 4
  %42 = load ptr, ptr %created3, align 8
  %43 = getelementptr i32, ptr %42, i64 0
  store i32 1, ptr %43, align 4
  %44 = load i32, ptr %i, align 4
  %45 = sext i32 %44 to i64
  %46 = load ptr, ptr %m1, align 8
  %vals26 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %46, i32 0, i32 1
  %vals27 = load ptr, ptr %vals26, align 8
  %47 = getelementptr i32, ptr %vals27, i64 %45
  ret ptr %47

then15:                                           ; preds = %while_body
  %48 = load ptr, ptr %created3, align 8
  %49 = getelementptr i32, ptr %48, i64 0
  store i32 0, ptr %49, align 4
  %50 = load i32, ptr %i, align 4
  %51 = sext i32 %50 to i64
  %52 = load ptr, ptr %m1, align 8
  %vals = getelementptr inbounds nuw %HashMap_Pt_int, ptr %52, i32 0, i32 1
  %vals17 = load ptr, ptr %vals, align 8
  %53 = getelementptr i32, ptr %vals17, i64 %51
  ret ptr %53

merge16:                                          ; preds = %while_body
  %54 = load i32, ptr %i, align 4
  %55 = add i32 %54, 1
  %56 = load ptr, ptr %m1, align 8
  %cap18 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %56, i32 0, i32 3
  %cap19 = load i32, ptr %cap18, align 4
  %57 = icmp eq i32 %cap19, 0
  br i1 %57, label %div.zero20, label %div.ok21

div.zero20:                                       ; preds = %merge16
  call void @llvm.trap()
  unreachable

div.ok21:                                         ; preds = %merge16
  %58 = icmp eq i32 %cap19, -1
  %59 = select i1 %58, i32 1, i32 %cap19
  %60 = srem i32 %55, %59
  store i32 %60, ptr %i, align 4
  br label %while
}

define void @_HashMap_grow_Pt_int(ptr noundef %m) {
entry:
  %j = alloca i32, align 4
  %h = alloca i64, align 8
  %__end_i = alloca i32, align 4
  %i = alloca i32, align 4
  %hashfn = alloca { ptr, ptr }, align 8
  %nu = alloca ptr, align 8
  %nv = alloca ptr, align 8
  %nk = alloca ptr, align 8
  %newcap = alloca i32, align 4
  %ou = alloca ptr, align 8
  %ov = alloca ptr, align 8
  %ok = alloca ptr, align 8
  %oldcap = alloca i32, align 4
  %m1 = alloca ptr, align 8
  store ptr %m, ptr %m1, align 8
  %0 = load ptr, ptr %m1, align 8
  %cap = getelementptr inbounds nuw %HashMap_Pt_int, ptr %0, i32 0, i32 3
  %cap2 = load i32, ptr %cap, align 4
  store i32 %cap2, ptr %oldcap, align 4
  %1 = load ptr, ptr %m1, align 8
  %keys = getelementptr inbounds nuw %HashMap_Pt_int, ptr %1, i32 0, i32 0
  %keys3 = load ptr, ptr %keys, align 8
  store ptr %keys3, ptr %ok, align 8
  %2 = load ptr, ptr %m1, align 8
  %vals = getelementptr inbounds nuw %HashMap_Pt_int, ptr %2, i32 0, i32 1
  %vals4 = load ptr, ptr %vals, align 8
  store ptr %vals4, ptr %ov, align 8
  %3 = load ptr, ptr %m1, align 8
  %used = getelementptr inbounds nuw %HashMap_Pt_int, ptr %3, i32 0, i32 2
  %used5 = load ptr, ptr %used, align 8
  store ptr %used5, ptr %ou, align 8
  %4 = load i32, ptr %oldcap, align 4
  %5 = icmp slt i32 %4, 4
  br i1 %5, label %tern.then, label %tern.else

tern.then:                                        ; preds = %entry
  br label %tern.cont

tern.else:                                        ; preds = %entry
  %6 = load i32, ptr %oldcap, align 4
  %7 = mul i32 %6, 2
  br label %tern.cont

tern.cont:                                        ; preds = %tern.else, %tern.then
  %8 = phi i32 [ 8, %tern.then ], [ %7, %tern.else ]
  store i32 %8, ptr %newcap, align 4
  %9 = load i32, ptr %newcap, align 4
  %10 = sext i32 %9 to i64
  %11 = call ptr @alloc_Pt(i64 %10)
  store ptr %11, ptr %nk, align 8
  %12 = load i32, ptr %newcap, align 4
  %13 = sext i32 %12 to i64
  %14 = call ptr @alloc_int(i64 %13)
  store ptr %14, ptr %nv, align 8
  %15 = load i32, ptr %newcap, align 4
  %16 = sext i32 %15 to i64
  %17 = call ptr @alloc_uint8(i64 %16)
  store ptr %17, ptr %nu, align 8
  %18 = load ptr, ptr %m1, align 8
  %hash = getelementptr inbounds nuw %HashMap_Pt_int, ptr %18, i32 0, i32 5
  %hash6 = load { ptr, ptr }, ptr %hash, align 8
  store { ptr, ptr } %hash6, ptr %hashfn, align 8
  store i32 0, ptr %i, align 4
  %19 = load i32, ptr %oldcap, align 4
  store i32 %19, ptr %__end_i, align 4
  br label %for

for:                                              ; preds = %for_step, %tern.cont
  %20 = load i32, ptr %i, align 4
  %21 = load i32, ptr %__end_i, align 4
  %22 = icmp slt i32 %20, %21
  br i1 %22, label %for_body, label %for_exit

for_body:                                         ; preds = %for
  %23 = load i32, ptr %i, align 4
  %24 = sext i32 %23 to i64
  %25 = load ptr, ptr %ou, align 8
  %26 = getelementptr i8, ptr %25, i64 %24
  %27 = load i8, ptr %26, align 1
  %28 = zext i8 %27 to i32
  %29 = icmp eq i32 %28, 1
  br i1 %29, label %then, label %merge

for_step:                                         ; preds = %merge
  %30 = load i32, ptr %i, align 4
  %31 = add i32 %30, 1
  store i32 %31, ptr %i, align 4
  br label %for

for_exit:                                         ; preds = %for
  %32 = load ptr, ptr %ok, align 8
  call void @free(ptr %32)
  %33 = load ptr, ptr %ov, align 8
  call void @free(ptr %33)
  %34 = load ptr, ptr %ou, align 8
  call void @free(ptr %34)
  %35 = load ptr, ptr %m1, align 8
  %36 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %35, i32 0, i32 0
  %37 = load ptr, ptr %nk, align 8
  store ptr %37, ptr %36, align 8
  %38 = load ptr, ptr %m1, align 8
  %39 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %38, i32 0, i32 1
  %40 = load ptr, ptr %nv, align 8
  store ptr %40, ptr %39, align 8
  %41 = load ptr, ptr %m1, align 8
  %42 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %41, i32 0, i32 2
  %43 = load ptr, ptr %nu, align 8
  store ptr %43, ptr %42, align 8
  %44 = load ptr, ptr %m1, align 8
  %45 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %44, i32 0, i32 3
  %46 = load i32, ptr %newcap, align 4
  store i32 %46, ptr %45, align 4
  ret void

then:                                             ; preds = %for_body
  %47 = load { ptr, ptr }, ptr %hashfn, align 8
  %fn.ptr = extractvalue { ptr, ptr } %47, 0
  %env.ptr = extractvalue { ptr, ptr } %47, 1
  %48 = load i32, ptr %i, align 4
  %49 = sext i32 %48 to i64
  %50 = load ptr, ptr %ok, align 8
  %51 = getelementptr %Pt, ptr %50, i64 %49
  %fn.call = call i64 %fn.ptr(ptr %env.ptr, ptr %51)
  store i64 %fn.call, ptr %h, align 8
  %52 = load i64, ptr %h, align 8
  %53 = load i32, ptr %newcap, align 4
  %54 = sext i32 %53 to i64
  %55 = icmp eq i64 %54, 0
  br i1 %55, label %div.zero, label %div.ok

merge:                                            ; preds = %while_exit, %for_body
  br label %for_step

div.zero:                                         ; preds = %then
  call void @llvm.trap()
  unreachable

div.ok:                                           ; preds = %then
  %56 = urem i64 %52, %54
  %57 = trunc i64 %56 to i32
  store i32 %57, ptr %j, align 4
  br label %while

while:                                            ; preds = %div.ok8, %div.ok
  %58 = load i32, ptr %j, align 4
  %59 = sext i32 %58 to i64
  %60 = load ptr, ptr %nu, align 8
  %61 = getelementptr i8, ptr %60, i64 %59
  %62 = load i8, ptr %61, align 1
  %63 = zext i8 %62 to i32
  %64 = icmp eq i32 %63, 1
  br i1 %64, label %while_body, label %while_exit

while_body:                                       ; preds = %while
  %65 = load i32, ptr %j, align 4
  %66 = add i32 %65, 1
  %67 = load i32, ptr %newcap, align 4
  %68 = icmp eq i32 %67, 0
  br i1 %68, label %div.zero7, label %div.ok8

while_exit:                                       ; preds = %while
  %69 = load i32, ptr %j, align 4
  %70 = sext i32 %69 to i64
  %71 = load ptr, ptr %nu, align 8
  %72 = getelementptr i8, ptr %71, i64 %70
  store i8 1, ptr %72, align 1
  %73 = load i32, ptr %j, align 4
  %74 = sext i32 %73 to i64
  %75 = load ptr, ptr %nk, align 8
  %76 = getelementptr %Pt, ptr %75, i64 %74
  %77 = load i32, ptr %i, align 4
  %78 = sext i32 %77 to i64
  %79 = load ptr, ptr %ok, align 8
  %80 = getelementptr %Pt, ptr %79, i64 %78
  %81 = load %Pt, ptr %80, align 4
  store %Pt %81, ptr %76, align 4
  %82 = load i32, ptr %j, align 4
  %83 = sext i32 %82 to i64
  %84 = load ptr, ptr %nv, align 8
  %85 = getelementptr i32, ptr %84, i64 %83
  %86 = load i32, ptr %i, align 4
  %87 = sext i32 %86 to i64
  %88 = load ptr, ptr %ov, align 8
  %89 = getelementptr i32, ptr %88, i64 %87
  %90 = load i32, ptr %89, align 4
  store i32 %90, ptr %85, align 4
  br label %merge

div.zero7:                                        ; preds = %while_body
  call void @llvm.trap()
  unreachable

div.ok8:                                          ; preds = %while_body
  %91 = icmp eq i32 %67, -1
  %92 = select i1 %91, i32 1, i32 %67
  %93 = srem i32 %66, %92
  store i32 %93, ptr %j, align 4
  br label %while
}

define noundef i32 @HashMap_get_Pt_int(ptr noundef %m, %Pt %key, ptr noundef %out) {
entry:
  %n = alloca i32, align 4
  %i = alloca i32, align 4
  %h = alloca i64, align 8
  %eqfn = alloca { ptr, ptr }, align 8
  %hashfn = alloca { ptr, ptr }, align 8
  %out3 = alloca ptr, align 8
  %key2 = alloca %Pt, align 8
  %m1 = alloca ptr, align 8
  store ptr %m, ptr %m1, align 8
  store %Pt %key, ptr %key2, align 4
  store ptr %out, ptr %out3, align 8
  %0 = load ptr, ptr %m1, align 8
  %cap = getelementptr inbounds nuw %HashMap_Pt_int, ptr %0, i32 0, i32 3
  %cap4 = load i32, ptr %cap, align 4
  %1 = icmp eq i32 %cap4, 0
  br i1 %1, label %then, label %merge

then:                                             ; preds = %entry
  ret i32 0

merge:                                            ; preds = %entry
  %2 = load ptr, ptr %m1, align 8
  %hash = getelementptr inbounds nuw %HashMap_Pt_int, ptr %2, i32 0, i32 5
  %hash5 = load { ptr, ptr }, ptr %hash, align 8
  store { ptr, ptr } %hash5, ptr %hashfn, align 8
  %3 = load ptr, ptr %m1, align 8
  %eq = getelementptr inbounds nuw %HashMap_Pt_int, ptr %3, i32 0, i32 6
  %eq6 = load { ptr, ptr }, ptr %eq, align 8
  store { ptr, ptr } %eq6, ptr %eqfn, align 8
  %4 = load { ptr, ptr }, ptr %hashfn, align 8
  %fn.ptr = extractvalue { ptr, ptr } %4, 0
  %env.ptr = extractvalue { ptr, ptr } %4, 1
  %fn.call = call i64 %fn.ptr(ptr %env.ptr, ptr %key2)
  store i64 %fn.call, ptr %h, align 8
  %5 = load i64, ptr %h, align 8
  %6 = load ptr, ptr %m1, align 8
  %cap7 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %6, i32 0, i32 3
  %cap8 = load i32, ptr %cap7, align 4
  %7 = sext i32 %cap8 to i64
  %8 = icmp eq i64 %7, 0
  br i1 %8, label %div.zero, label %div.ok

div.zero:                                         ; preds = %merge
  call void @llvm.trap()
  unreachable

div.ok:                                           ; preds = %merge
  %9 = urem i64 %5, %7
  %10 = trunc i64 %9 to i32
  store i32 %10, ptr %i, align 4
  store i32 0, ptr %n, align 4
  br label %for

for:                                              ; preds = %for_step, %div.ok
  %11 = load i32, ptr %i, align 4
  %12 = sext i32 %11 to i64
  %13 = load ptr, ptr %m1, align 8
  %used = getelementptr inbounds nuw %HashMap_Pt_int, ptr %13, i32 0, i32 2
  %used9 = load ptr, ptr %used, align 8
  %14 = getelementptr i8, ptr %used9, i64 %12
  %15 = load i8, ptr %14, align 1
  %16 = zext i8 %15 to i32
  %17 = icmp eq i32 %16, 1
  br i1 %17, label %sc.rhs, label %sc.cont

for_body:                                         ; preds = %sc.cont
  %18 = load { ptr, ptr }, ptr %eqfn, align 8
  %fn.ptr12 = extractvalue { ptr, ptr } %18, 0
  %env.ptr13 = extractvalue { ptr, ptr } %18, 1
  %19 = load i32, ptr %i, align 4
  %20 = sext i32 %19 to i64
  %21 = load ptr, ptr %m1, align 8
  %keys = getelementptr inbounds nuw %HashMap_Pt_int, ptr %21, i32 0, i32 0
  %keys14 = load ptr, ptr %keys, align 8
  %22 = getelementptr %Pt, ptr %keys14, i64 %20
  %fn.call15 = call i32 %fn.ptr12(ptr %env.ptr13, ptr %key2, ptr %22)
  %23 = icmp eq i32 %fn.call15, 1
  br i1 %23, label %then16, label %merge17

for_step:                                         ; preds = %div.ok22
  %24 = load i32, ptr %n, align 4
  %25 = add i32 %24, 1
  store i32 %25, ptr %n, align 4
  br label %for

for_exit:                                         ; preds = %sc.cont
  ret i32 0

sc.rhs:                                           ; preds = %for
  %26 = load i32, ptr %n, align 4
  %27 = load ptr, ptr %m1, align 8
  %cap10 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %27, i32 0, i32 3
  %cap11 = load i32, ptr %cap10, align 4
  %28 = icmp slt i32 %26, %cap11
  br label %sc.cont

sc.cont:                                          ; preds = %sc.rhs, %for
  %29 = phi i1 [ false, %for ], [ %28, %sc.rhs ]
  br i1 %29, label %for_body, label %for_exit

then16:                                           ; preds = %for_body
  %30 = load ptr, ptr %out3, align 8
  %31 = getelementptr i32, ptr %30, i64 0
  %32 = load i32, ptr %i, align 4
  %33 = sext i32 %32 to i64
  %34 = load ptr, ptr %m1, align 8
  %vals = getelementptr inbounds nuw %HashMap_Pt_int, ptr %34, i32 0, i32 1
  %vals18 = load ptr, ptr %vals, align 8
  %35 = getelementptr i32, ptr %vals18, i64 %33
  %36 = load i32, ptr %35, align 4
  store i32 %36, ptr %31, align 4
  ret i32 1

merge17:                                          ; preds = %for_body
  %37 = load i32, ptr %i, align 4
  %38 = add i32 %37, 1
  %39 = load ptr, ptr %m1, align 8
  %cap19 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %39, i32 0, i32 3
  %cap20 = load i32, ptr %cap19, align 4
  %40 = icmp eq i32 %cap20, 0
  br i1 %40, label %div.zero21, label %div.ok22

div.zero21:                                       ; preds = %merge17
  call void @llvm.trap()
  unreachable

div.ok22:                                         ; preds = %merge17
  %41 = icmp eq i32 %cap20, -1
  %42 = select i1 %41, i32 1, i32 %cap20
  %43 = srem i32 %38, %42
  store i32 %43, ptr %i, align 4
  br label %for_step
}

define void @HashMap_free_int_int(ptr noundef %m) {
entry:
  %m1 = alloca ptr, align 8
  store ptr %m, ptr %m1, align 8
  %0 = load ptr, ptr %m1, align 8
  %keys = getelementptr inbounds nuw %HashMap_int_int, ptr %0, i32 0, i32 0
  %keys2 = load ptr, ptr %keys, align 8
  call void @free(ptr %keys2)
  %1 = load ptr, ptr %m1, align 8
  %vals = getelementptr inbounds nuw %HashMap_int_int, ptr %1, i32 0, i32 1
  %vals3 = load ptr, ptr %vals, align 8
  call void @free(ptr %vals3)
  %2 = load ptr, ptr %m1, align 8
  %used = getelementptr inbounds nuw %HashMap_int_int, ptr %2, i32 0, i32 2
  %used4 = load ptr, ptr %used, align 8
  call void @free(ptr %used4)
  %3 = load ptr, ptr %m1, align 8
  %4 = getelementptr inbounds nuw %HashMap_int_int, ptr %3, i32 0, i32 0
  store ptr null, ptr %4, align 8
  %5 = load ptr, ptr %m1, align 8
  %6 = getelementptr inbounds nuw %HashMap_int_int, ptr %5, i32 0, i32 1
  store ptr null, ptr %6, align 8
  %7 = load ptr, ptr %m1, align 8
  %8 = getelementptr inbounds nuw %HashMap_int_int, ptr %7, i32 0, i32 2
  store ptr null, ptr %8, align 8
  %9 = load ptr, ptr %m1, align 8
  %10 = getelementptr inbounds nuw %HashMap_int_int, ptr %9, i32 0, i32 3
  store i32 0, ptr %10, align 4
  %11 = load ptr, ptr %m1, align 8
  %12 = getelementptr inbounds nuw %HashMap_int_int, ptr %11, i32 0, i32 4
  store i32 0, ptr %12, align 4
  ret void
}

define void @HashMap_free_Pt_int(ptr noundef %m) {
entry:
  %m1 = alloca ptr, align 8
  store ptr %m, ptr %m1, align 8
  %0 = load ptr, ptr %m1, align 8
  %keys = getelementptr inbounds nuw %HashMap_Pt_int, ptr %0, i32 0, i32 0
  %keys2 = load ptr, ptr %keys, align 8
  call void @free(ptr %keys2)
  %1 = load ptr, ptr %m1, align 8
  %vals = getelementptr inbounds nuw %HashMap_Pt_int, ptr %1, i32 0, i32 1
  %vals3 = load ptr, ptr %vals, align 8
  call void @free(ptr %vals3)
  %2 = load ptr, ptr %m1, align 8
  %used = getelementptr inbounds nuw %HashMap_Pt_int, ptr %2, i32 0, i32 2
  %used4 = load ptr, ptr %used, align 8
  call void @free(ptr %used4)
  %3 = load ptr, ptr %m1, align 8
  %4 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %3, i32 0, i32 0
  store ptr null, ptr %4, align 8
  %5 = load ptr, ptr %m1, align 8
  %6 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %5, i32 0, i32 1
  store ptr null, ptr %6, align 8
  %7 = load ptr, ptr %m1, align 8
  %8 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %7, i32 0, i32 2
  store ptr null, ptr %8, align 8
  %9 = load ptr, ptr %m1, align 8
  %10 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %9, i32 0, i32 3
  store i32 0, ptr %10, align 4
  %11 = load ptr, ptr %m1, align 8
  %12 = getelementptr inbounds nuw %HashMap_Pt_int, ptr %11, i32 0, i32 4
  store i32 0, ptr %12, align 4
  ret void
}

attributes #0 = { nounwind }
attributes #1 = { cold noreturn nounwind memory(inaccessiblemem: write) }
