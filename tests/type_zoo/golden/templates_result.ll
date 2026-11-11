
%Result_int_string = type { i32, i32, ptr }

@0 = private unnamed_addr constant [17 x i8] c"division by zero\00", align 1
@1 = private unnamed_addr constant [7 x i8] c"ok=%d\0A\00", align 1
@2 = private unnamed_addr constant [8 x i8] c"err=%s\0A\00", align 1

declare i32 @printf(ptr, ...)

; Function Attrs: nounwind
define %Result_int_string @divide(i32 noundef %a, i32 noundef %b) #0 {
entry:
  %b2 = alloca i32, align 4
  %a1 = alloca i32, align 4
  store i32 %a, ptr %a1, align 4
  store i32 %b, ptr %b2, align 4
  %0 = load i32, ptr %b2, align 4
  %1 = icmp eq i32 %0, 0
  br i1 %1, label %then, label %merge

then:                                             ; preds = %entry
  %2 = call %Result_int_string @Err_int_string(ptr @0)
  ret %Result_int_string %2

merge:                                            ; preds = %entry
  %3 = load i32, ptr %a1, align 4
  %4 = load i32, ptr %b2, align 4
  %5 = icmp eq i32 %4, 0
  br i1 %5, label %div.zero, label %div.ok

div.zero:                                         ; preds = %merge
  call void @llvm.trap()
  unreachable

div.ok:                                           ; preds = %merge
  %6 = icmp eq i32 %4, -1
  %7 = select i1 %6, i32 1, i32 %4
  %8 = sub i32 0, %3
  %9 = sdiv i32 %3, %7
  %10 = select i1 %6, i32 %8, i32 %9
  %11 = call %Result_int_string @Ok_int_string(i32 %10)
  ret %Result_int_string %11
}

define noundef i32 @main() {
entry:
  %bad = alloca %Result_int_string, align 8
  %ok = alloca %Result_int_string, align 8
  %0 = call %Result_int_string @divide(i32 20, i32 4)
  store %Result_int_string %0, ptr %ok, align 8
  %ok1 = getelementptr inbounds nuw %Result_int_string, ptr %ok, i32 0, i32 0
  %ok2 = load i32, ptr %ok1, align 4
  %1 = icmp eq i32 %ok2, 1
  br i1 %1, label %then, label %merge

then:                                             ; preds = %entry
  %value = getelementptr inbounds nuw %Result_int_string, ptr %ok, i32 0, i32 1
  %value3 = load i32, ptr %value, align 4
  %2 = call i32 (ptr, ...) @printf(ptr @1, i32 %value3)
  br label %merge

merge:                                            ; preds = %then, %entry
  %3 = call %Result_int_string @divide(i32 1, i32 0)
  store %Result_int_string %3, ptr %bad, align 8
  %ok4 = getelementptr inbounds nuw %Result_int_string, ptr %bad, i32 0, i32 0
  %ok5 = load i32, ptr %ok4, align 4
  %4 = icmp eq i32 %ok5, 0
  br i1 %4, label %then6, label %merge7

then6:                                            ; preds = %merge
  %error = getelementptr inbounds nuw %Result_int_string, ptr %bad, i32 0, i32 2
  %error8 = load ptr, ptr %error, align 8
  %5 = call i32 (ptr, ...) @printf(ptr @2, ptr %error8)
  br label %merge7

merge7:                                           ; preds = %then6, %merge
  ret i32 0
}

; Function Attrs: nounwind
define %Result_int_string @Err_int_string(ptr noundef %msg) #0 {
entry:
  %r = alloca %Result_int_string, align 8
  %msg1 = alloca ptr, align 8
  store ptr %msg, ptr %msg1, align 8
  store %Result_int_string zeroinitializer, ptr %r, align 8
  %0 = getelementptr inbounds nuw %Result_int_string, ptr %r, i32 0, i32 0
  store i32 0, ptr %0, align 4
  %1 = getelementptr inbounds nuw %Result_int_string, ptr %r, i32 0, i32 2
  %2 = load ptr, ptr %msg1, align 8
  store ptr %2, ptr %1, align 8
  %3 = load %Result_int_string, ptr %r, align 8
  ret %Result_int_string %3
}

; Function Attrs: nounwind
define %Result_int_string @Ok_int_string(i32 noundef %value) #0 {
entry:
  %r = alloca %Result_int_string, align 8
  %value1 = alloca i32, align 4
  store i32 %value, ptr %value1, align 4
  store %Result_int_string zeroinitializer, ptr %r, align 8
  %0 = getelementptr inbounds nuw %Result_int_string, ptr %r, i32 0, i32 0
  store i32 1, ptr %0, align 4
  %1 = getelementptr inbounds nuw %Result_int_string, ptr %r, i32 0, i32 1
  %2 = load i32, ptr %value1, align 4
  store i32 %2, ptr %1, align 4
  %3 = load %Result_int_string, ptr %r, align 8
  ret %Result_int_string %3
}

; Function Attrs: cold noreturn nounwind memory(inaccessiblemem: write)
declare void @llvm.trap() #1

attributes #0 = { nounwind }
attributes #1 = { cold noreturn nounwind memory(inaccessiblemem: write) }
