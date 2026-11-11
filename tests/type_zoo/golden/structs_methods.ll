
%Counter = type { i32 }
%Point = type { i32, i32 }

@0 = private unnamed_addr constant [10 x i8] c"count=%d\0A\00", align 1
@1 = private unnamed_addr constant [11 x i8] c"x=%d y=%d\0A\00", align 1
@2 = private unnamed_addr constant [8 x i8] c"sum=%d\0A\00", align 1

declare i32 @printf(ptr, ...)

; Function Attrs: nounwind
define void @Counter_inc(ptr noundef %self) #0 {
entry:
  %self1 = alloca ptr, align 8
  store ptr %self, ptr %self1, align 8
  %0 = load ptr, ptr %self1, align 8
  %1 = getelementptr inbounds nuw %Counter, ptr %0, i32 0, i32 0
  %2 = load ptr, ptr %self1, align 8
  %count = getelementptr inbounds nuw %Counter, ptr %2, i32 0, i32 0
  %count2 = load i32, ptr %count, align 4
  %3 = add i32 %count2, 1
  store i32 %3, ptr %1, align 4
  ret void
}

; Function Attrs: nounwind
define noundef i32 @Counter_get(ptr noundef %self) #0 {
entry:
  %self1 = alloca ptr, align 8
  store ptr %self, ptr %self1, align 8
  %0 = load ptr, ptr %self1, align 8
  %count = getelementptr inbounds nuw %Counter, ptr %0, i32 0, i32 0
  %count2 = load i32, ptr %count, align 4
  ret i32 %count2
}

; Function Attrs: nounwind
define noundef i32 @Point_sumCoords(ptr noundef %self) #0 {
entry:
  %self1 = alloca ptr, align 8
  store ptr %self, ptr %self1, align 8
  %0 = load ptr, ptr %self1, align 8
  %x = getelementptr inbounds nuw %Point, ptr %0, i32 0, i32 0
  %x2 = load i32, ptr %x, align 4
  %1 = load ptr, ptr %self1, align 8
  %y = getelementptr inbounds nuw %Point, ptr %1, i32 0, i32 1
  %y3 = load i32, ptr %y, align 4
  %2 = add i32 %x2, %y3
  ret i32 %2
}

define noundef i32 @main() {
entry:
  %p = alloca %Point, align 8
  %c = alloca %Counter, align 8
  store %Counter zeroinitializer, ptr %c, align 4
  %0 = getelementptr inbounds nuw %Counter, ptr %c, i32 0, i32 0
  store i32 0, ptr %0, align 4
  call void @Counter_inc(ptr %c)
  call void @Counter_inc(ptr %c)
  call void @Counter_inc(ptr %c)
  %1 = call i32 @Counter_get(ptr %c)
  %2 = call i32 (ptr, ...) @printf(ptr @0, i32 %1)
  store %Point zeroinitializer, ptr %p, align 4
  %3 = getelementptr inbounds nuw %Point, ptr %p, i32 0, i32 0
  store i32 3, ptr %3, align 4
  %4 = getelementptr inbounds nuw %Point, ptr %p, i32 0, i32 1
  store i32 4, ptr %4, align 4
  %x = getelementptr inbounds nuw %Point, ptr %p, i32 0, i32 0
  %x1 = load i32, ptr %x, align 4
  %y = getelementptr inbounds nuw %Point, ptr %p, i32 0, i32 1
  %y2 = load i32, ptr %y, align 4
  %5 = call i32 (ptr, ...) @printf(ptr @1, i32 %x1, i32 %y2)
  %6 = call i32 @Point_sumCoords(ptr %p)
  %7 = call i32 (ptr, ...) @printf(ptr @2, i32 %6)
  ret i32 0
}

attributes #0 = { nounwind }
