
%Speaker_vtable = type { ptr }
%Dog = type { i32 }
%Cat = type { i32 }
%Speaker_fat = type { ptr, ptr }

@0 = private unnamed_addr constant [8 x i8] c"dog=%d\0A\00", align 1
@Speaker_vtable_Dog = private constant %Speaker_vtable { ptr @Dog_sound }
@1 = private unnamed_addr constant [8 x i8] c"cat=%d\0A\00", align 1
@Speaker_vtable_Cat = private constant %Speaker_vtable { ptr @Cat_sound }
@2 = private unnamed_addr constant [8 x i8] c"sum=%d\0A\00", align 1
@Speaker_vtable_Dog.1 = private constant %Speaker_vtable { ptr @Dog_sound }
@Speaker_vtable_Cat.2 = private constant %Speaker_vtable { ptr @Cat_sound }

declare i32 @printf(ptr, ...)

; Function Attrs: nounwind
define noundef i32 @Dog_sound(ptr noundef %self) #0 {
entry:
  %self1 = alloca ptr, align 8
  store ptr %self, ptr %self1, align 8
  %0 = load ptr, ptr %self1, align 8
  %volume = getelementptr inbounds nuw %Dog, ptr %0, i32 0, i32 0
  %volume2 = load i32, ptr %volume, align 4
  %1 = mul i32 %volume2, 2
  ret i32 %1
}

; Function Attrs: nounwind
define noundef i32 @Cat_sound(ptr noundef %self) #0 {
entry:
  %self1 = alloca ptr, align 8
  store ptr %self, ptr %self1, align 8
  %0 = load ptr, ptr %self1, align 8
  %volume = getelementptr inbounds nuw %Cat, ptr %0, i32 0, i32 0
  %volume2 = load i32, ptr %volume, align 4
  %1 = add i32 %volume2, 1
  ret i32 %1
}

define noundef i32 @speak(%Speaker_fat %s) {
entry:
  %s1 = alloca %Speaker_fat, align 8
  store %Speaker_fat %s, ptr %s1, align 8
  %0 = load %Speaker_fat, ptr %s1, align 8
  %iface.data = extractvalue %Speaker_fat %0, 0
  %iface.vt = extractvalue %Speaker_fat %0, 1
  %1 = getelementptr inbounds nuw %Speaker_vtable, ptr %iface.vt, i32 0, i32 0
  %2 = load ptr, ptr %1, align 8
  %3 = call i32 %2(ptr %iface.data)
  ret i32 %3
}

define noundef i32 @main() {
entry:
  %c = alloca %Cat, align 8
  %d = alloca %Dog, align 8
  store %Dog zeroinitializer, ptr %d, align 4
  %0 = getelementptr inbounds nuw %Dog, ptr %d, i32 0, i32 0
  store i32 10, ptr %0, align 4
  store %Cat zeroinitializer, ptr %c, align 4
  %1 = getelementptr inbounds nuw %Cat, ptr %c, i32 0, i32 0
  store i32 10, ptr %1, align 4
  %2 = insertvalue %Speaker_fat undef, ptr %d, 0
  %3 = insertvalue %Speaker_fat %2, ptr @Speaker_vtable_Dog, 1
  %4 = call i32 @speak(%Speaker_fat %3)
  %5 = call i32 (ptr, ...) @printf(ptr @0, i32 %4)
  %6 = insertvalue %Speaker_fat undef, ptr %c, 0
  %7 = insertvalue %Speaker_fat %6, ptr @Speaker_vtable_Cat, 1
  %8 = call i32 @speak(%Speaker_fat %7)
  %9 = call i32 (ptr, ...) @printf(ptr @1, i32 %8)
  %10 = insertvalue %Speaker_fat undef, ptr %d, 0
  %11 = insertvalue %Speaker_fat %10, ptr @Speaker_vtable_Dog.1, 1
  %12 = call i32 @speak(%Speaker_fat %11)
  %13 = insertvalue %Speaker_fat undef, ptr %c, 0
  %14 = insertvalue %Speaker_fat %13, ptr @Speaker_vtable_Cat.2, 1
  %15 = call i32 @speak(%Speaker_fat %14)
  %16 = add i32 %12, %15
  %17 = call i32 (ptr, ...) @printf(ptr @2, i32 %16)
  ret i32 0
}

attributes #0 = { nounwind }
