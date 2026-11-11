
%Box_int = type { i32 }
%Pair_int_int = type { i32, i32 }
%Pair_int_float = type { i32, float }

@0 = private unnamed_addr constant [22 x i8] c"first=%d second=%.2f\0A\00", align 1
@1 = private unnamed_addr constant [9 x i8] c"q=%d,%d\0A\00", align 1
@2 = private unnamed_addr constant [8 x i8] c"box=%d\0A\00", align 1

declare i32 @printf(ptr, ...)

define noundef i32 @main() {
entry:
  %b = alloca %Box_int, align 8
  %q = alloca %Pair_int_int, align 8
  %p = alloca %Pair_int_float, align 8
  store %Pair_int_float zeroinitializer, ptr %p, align 4
  %0 = getelementptr inbounds nuw %Pair_int_float, ptr %p, i32 0, i32 0
  store i32 7, ptr %0, align 4
  %1 = getelementptr inbounds nuw %Pair_int_float, ptr %p, i32 0, i32 1
  store float 0x40091EB860000000, ptr %1, align 4
  %first = getelementptr inbounds nuw %Pair_int_float, ptr %p, i32 0, i32 0
  %first1 = load i32, ptr %first, align 4
  %second = getelementptr inbounds nuw %Pair_int_float, ptr %p, i32 0, i32 1
  %second2 = load float, ptr %second, align 4
  %2 = fpext float %second2 to double
  %3 = call i32 (ptr, ...) @printf(ptr @0, i32 %first1, double %2)
  store %Pair_int_int zeroinitializer, ptr %q, align 4
  %4 = getelementptr inbounds nuw %Pair_int_int, ptr %q, i32 0, i32 0
  store i32 10, ptr %4, align 4
  %5 = getelementptr inbounds nuw %Pair_int_int, ptr %q, i32 0, i32 1
  store i32 20, ptr %5, align 4
  %first3 = getelementptr inbounds nuw %Pair_int_int, ptr %q, i32 0, i32 0
  %first4 = load i32, ptr %first3, align 4
  %second5 = getelementptr inbounds nuw %Pair_int_int, ptr %q, i32 0, i32 1
  %second6 = load i32, ptr %second5, align 4
  %6 = call i32 (ptr, ...) @printf(ptr @1, i32 %first4, i32 %second6)
  store %Box_int zeroinitializer, ptr %b, align 4
  %7 = getelementptr inbounds nuw %Box_int, ptr %b, i32 0, i32 0
  store i32 99, ptr %7, align 4
  %value = getelementptr inbounds nuw %Box_int, ptr %b, i32 0, i32 0
  %value7 = load i32, ptr %value, align 4
  %8 = call i32 (ptr, ...) @printf(ptr @2, i32 %value7)
  ret i32 0
}
