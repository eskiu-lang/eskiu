
%__lambda0.env = type { { ptr, ptr }, ptr }

@0 = private unnamed_addr constant [11 x i8] c"global %d\0A\00", align 1
@1 = private unnamed_addr constant [15 x i8] c"dispatched %d\0A\00", align 1
@2 = private unnamed_addr constant [8 x i8] c"sum=%d\0A\00", align 1

declare i32 @printf(ptr, ...)

define noundef i32 @apply2({ ptr, ptr } %op, i32 noundef %a, i32 noundef %b) {
entry:
  %b3 = alloca i32, align 4
  %a2 = alloca i32, align 4
  %op1 = alloca { ptr, ptr }, align 8
  store { ptr, ptr } %op, ptr %op1, align 8
  store i32 %a, ptr %a2, align 4
  store i32 %b, ptr %b3, align 4
  %0 = load { ptr, ptr }, ptr %op1, align 8
  %fn.ptr = extractvalue { ptr, ptr } %0, 0
  %env.ptr = extractvalue { ptr, ptr } %0, 1
  %1 = load i32, ptr %a2, align 4
  %2 = load i32, ptr %b3, align 4
  %fn.call = call i32 %fn.ptr(ptr %env.ptr, i32 %1, i32 %2)
  ret i32 %fn.call
}

; Function Attrs: nounwind
define noundef i32 @add(i32 noundef %a, i32 noundef %b) #0 {
entry:
  %b2 = alloca i32, align 4
  %a1 = alloca i32, align 4
  store i32 %a, ptr %a1, align 4
  store i32 %b, ptr %b2, align 4
  %0 = load i32, ptr %a1, align 4
  %1 = load i32, ptr %b2, align 4
  %2 = add i32 %0, %1
  ret i32 %2
}

define void @handler(i32 noundef %x) {
entry:
  %x1 = alloca i32, align 4
  store i32 %x, ptr %x1, align 4
  %0 = load i32, ptr %x1, align 4
  %1 = call i32 (ptr, ...) @printf(ptr @0, i32 %0)
  ret void
}

define void @dispatch(i32 noundef %v, { ptr, ptr } %handler) {
entry:
  %__lambda0.fat = alloca { ptr, ptr }, align 8
  %__lambda0.env = alloca %__lambda0.env, align 8
  %w = alloca { ptr, ptr }, align 8
  %pv = alloca ptr, align 8
  %vv = alloca i32, align 4
  %handler2 = alloca { ptr, ptr }, align 8
  %v1 = alloca i32, align 4
  store i32 %v, ptr %v1, align 4
  store { ptr, ptr } %handler, ptr %handler2, align 8
  %0 = load i32, ptr %v1, align 4
  store i32 %0, ptr %vv, align 4
  store ptr %vv, ptr %pv, align 8
  %handler3 = load { ptr, ptr }, ptr %handler2, align 8
  %1 = getelementptr inbounds nuw %__lambda0.env, ptr %__lambda0.env, i32 0, i32 0
  store { ptr, ptr } %handler3, ptr %1, align 8
  %pv4 = load ptr, ptr %pv, align 8
  %2 = getelementptr inbounds nuw %__lambda0.env, ptr %__lambda0.env, i32 0, i32 1
  store ptr %pv4, ptr %2, align 8
  %3 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda0.fat, i32 0, i32 0
  %4 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda0.fat, i32 0, i32 1
  store ptr @__lambda0, ptr %3, align 8
  store ptr %__lambda0.env, ptr %4, align 8
  %__lambda0.fat.val = load { ptr, ptr }, ptr %__lambda0.fat, align 8
  store { ptr, ptr } %__lambda0.fat.val, ptr %w, align 8
  %5 = load { ptr, ptr }, ptr %w, align 8
  %fn.ptr = extractvalue { ptr, ptr } %5, 0
  %env.ptr = extractvalue { ptr, ptr } %5, 1
  call void %fn.ptr(ptr %env.ptr)
  ret void
}

define void @say(i32 noundef %x) {
entry:
  %x1 = alloca i32, align 4
  store i32 %x, ptr %x1, align 4
  %0 = load i32, ptr %x1, align 4
  %1 = call i32 (ptr, ...) @printf(ptr @1, i32 %0)
  ret void
}

define noundef i32 @main() {
entry:
  %0 = call i32 @apply2({ ptr, ptr } { ptr @__fnptr_add, ptr null }, i32 20, i32 22)
  %1 = call i32 (ptr, ...) @printf(ptr @2, i32 %0)
  call void @dispatch(i32 7, { ptr, ptr } { ptr @__fnptr_say, ptr null })
  ret i32 0
}

define internal void @__lambda0(ptr %env) {
entry:
  %pv = alloca ptr, align 8
  %handler = alloca { ptr, ptr }, align 8
  %handler.gep = getelementptr inbounds nuw %__lambda0.env, ptr %env, i32 0, i32 0
  %handler.val = load { ptr, ptr }, ptr %handler.gep, align 8
  store { ptr, ptr } %handler.val, ptr %handler, align 8
  %pv.gep = getelementptr inbounds nuw %__lambda0.env, ptr %env, i32 0, i32 1
  %pv.val = load ptr, ptr %pv.gep, align 8
  store ptr %pv.val, ptr %pv, align 8
  %0 = load { ptr, ptr }, ptr %handler, align 8
  %fn.ptr = extractvalue { ptr, ptr } %0, 0
  %env.ptr = extractvalue { ptr, ptr } %0, 1
  %1 = load ptr, ptr %pv, align 8
  %2 = load i32, ptr %1, align 4
  call void %fn.ptr(ptr %env.ptr, i32 %2)
  ret void
}

; Function Attrs: nounwind
define internal i32 @__fnptr_add(ptr %0, i32 %1, i32 %2) #0 {
entry:
  %3 = call i32 @add(i32 %1, i32 %2)
  ret i32 %3
}

define internal void @__fnptr_say(ptr %0, i32 %1) {
entry:
  call void @say(i32 %1)
  ret void
}

attributes #0 = { nounwind }
