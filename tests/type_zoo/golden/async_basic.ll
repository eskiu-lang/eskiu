
%__lambda0.env = type { ptr }
%FutureHdr = type { i32, { ptr, ptr }, { ptr, ptr } }
%__lambda3.env = type { ptr, ptr, ptr }
%__lambda4.env = type { ptr, ptr, ptr }
%Future_int = type { i32, { ptr, ptr }, { ptr, ptr }, i32 }
%__lambda5.env = type { ptr, ptr }
%__lambda6.env = type { ptr, ptr, ptr }
%__lambda8.env = type { ptr, ptr, ptr }
%__lambda12.env = type { ptr, ptr, ptr }
%__one_frame = type { %Future_int, i32, ptr, i32, ptr }
%__lambda13.env = type { ptr }
%__lambda15.env = type { ptr }

@PENDING = global i32 0
@WAITING = global i32 1
@READY = global i32 2
@CANCELLED = global i32 3
@DETACHED = global i32 4
@ORPHANED = global i32 5
@CLEANED = global i32 6
@0 = private unnamed_addr constant [7 x i8] c"%d %d\0A\00", align 1

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

declare void @__cxa_free_exception(ptr)

declare i32 @printf(ptr, ...)

define void @_spawn_hdr(ptr noundef %f) {
entry:
  %__lambda0.fat = alloca { ptr, ptr }, align 8
  %reap = alloca { ptr, ptr }, align 8
  %f1 = alloca ptr, align 8
  store ptr %f, ptr %f1, align 8
  %__lambda0.env.heap = call ptr @malloc(i64 8)
  %f2 = load ptr, ptr %f1, align 8
  %0 = getelementptr inbounds nuw %__lambda0.env, ptr %__lambda0.env.heap, i32 0, i32 0
  store ptr %f2, ptr %0, align 8
  %1 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda0.fat, i32 0, i32 0
  %2 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda0.fat, i32 0, i32 1
  store ptr @__lambda0, ptr %1, align 8
  store ptr %__lambda0.env.heap, ptr %2, align 8
  %__lambda0.fat.val = load { ptr, ptr }, ptr %__lambda0.fat, align 8
  store { ptr, ptr } %__lambda0.fat.val, ptr %reap, align 8
  %3 = load ptr, ptr %f1, align 8
  %4 = getelementptr inbounds nuw %FutureHdr, ptr %3, i32 0, i32 1
  %5 = load { ptr, ptr }, ptr %reap, align 8
  store { ptr, ptr } %5, ptr %4, align 8
  %6 = load ptr, ptr %f1, align 8
  %7 = getelementptr inbounds nuw %FutureHdr, ptr %6, i32 0, i32 0
  %8 = cmpxchg ptr %7, i32 0, i32 1 acq_rel acquire, align 4
  %atm.cas.ok = extractvalue { i32, i1 } %8, 1
  br i1 %atm.cas.ok, label %then, label %merge

then:                                             ; preds = %entry
  ret void

merge:                                            ; preds = %entry
  %9 = load { ptr, ptr }, ptr %reap, align 8
  %clos.env = extractvalue { ptr, ptr } %9, 1
  call void @free(ptr %clos.env)
  %10 = load ptr, ptr %f1, align 8
  %on_drop = getelementptr inbounds nuw %FutureHdr, ptr %10, i32 0, i32 2
  %on_drop3 = load { ptr, ptr }, ptr %on_drop, align 8
  %clos.env4 = extractvalue { ptr, ptr } %on_drop3, 1
  call void @free(ptr %clos.env4)
  %11 = load ptr, ptr %f1, align 8
  call void @free(ptr %11)
  ret void
}

define noundef ptr @_select2_hdr(ptr noundef %a, ptr noundef %b) {
entry:
  %__lambda5.fat = alloca { ptr, ptr }, align 8
  %__lambda4.fat = alloca { ptr, ptr }, align 8
  %onB = alloca { ptr, ptr }, align 8
  %__lambda3.fat = alloca { ptr, ptr }, align 8
  %onA = alloca { ptr, ptr }, align 8
  %sel = alloca ptr, align 8
  %b2 = alloca ptr, align 8
  %a1 = alloca ptr, align 8
  store ptr %a, ptr %a1, align 8
  store ptr %b, ptr %b2, align 8
  %0 = call ptr @future_new_int()
  store ptr %0, ptr %sel, align 8
  %__lambda3.env.heap = call ptr @malloc(i64 24)
  %a3 = load ptr, ptr %a1, align 8
  %1 = getelementptr inbounds nuw %__lambda3.env, ptr %__lambda3.env.heap, i32 0, i32 0
  store ptr %a3, ptr %1, align 8
  %b4 = load ptr, ptr %b2, align 8
  %2 = getelementptr inbounds nuw %__lambda3.env, ptr %__lambda3.env.heap, i32 0, i32 1
  store ptr %b4, ptr %2, align 8
  %sel5 = load ptr, ptr %sel, align 8
  %3 = getelementptr inbounds nuw %__lambda3.env, ptr %__lambda3.env.heap, i32 0, i32 2
  store ptr %sel5, ptr %3, align 8
  %4 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda3.fat, i32 0, i32 0
  %5 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda3.fat, i32 0, i32 1
  store ptr @__lambda3, ptr %4, align 8
  store ptr %__lambda3.env.heap, ptr %5, align 8
  %__lambda3.fat.val = load { ptr, ptr }, ptr %__lambda3.fat, align 8
  store { ptr, ptr } %__lambda3.fat.val, ptr %onA, align 8
  %__lambda4.env.heap = call ptr @malloc(i64 24)
  %a6 = load ptr, ptr %a1, align 8
  %6 = getelementptr inbounds nuw %__lambda4.env, ptr %__lambda4.env.heap, i32 0, i32 0
  store ptr %a6, ptr %6, align 8
  %b7 = load ptr, ptr %b2, align 8
  %7 = getelementptr inbounds nuw %__lambda4.env, ptr %__lambda4.env.heap, i32 0, i32 1
  store ptr %b7, ptr %7, align 8
  %sel8 = load ptr, ptr %sel, align 8
  %8 = getelementptr inbounds nuw %__lambda4.env, ptr %__lambda4.env.heap, i32 0, i32 2
  store ptr %sel8, ptr %8, align 8
  %9 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda4.fat, i32 0, i32 0
  %10 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda4.fat, i32 0, i32 1
  store ptr @__lambda4, ptr %9, align 8
  store ptr %__lambda4.env.heap, ptr %10, align 8
  %__lambda4.fat.val = load { ptr, ptr }, ptr %__lambda4.fat, align 8
  store { ptr, ptr } %__lambda4.fat.val, ptr %onB, align 8
  %11 = load ptr, ptr %a1, align 8
  %12 = getelementptr inbounds nuw %FutureHdr, ptr %11, i32 0, i32 1
  %13 = load { ptr, ptr }, ptr %onA, align 8
  store { ptr, ptr } %13, ptr %12, align 8
  %14 = load ptr, ptr %a1, align 8
  %15 = getelementptr inbounds nuw %FutureHdr, ptr %14, i32 0, i32 0
  %16 = cmpxchg ptr %15, i32 0, i32 1 acq_rel acquire, align 4
  %atm.cas.ok = extractvalue { i32, i1 } %16, 1
  %17 = xor i1 %atm.cas.ok, true
  br i1 %17, label %then, label %merge

then:                                             ; preds = %entry
  %18 = load { ptr, ptr }, ptr %onA, align 8
  %clos.env = extractvalue { ptr, ptr } %18, 1
  call void @free(ptr %clos.env)
  %19 = load { ptr, ptr }, ptr %onB, align 8
  %clos.env9 = extractvalue { ptr, ptr } %19, 1
  call void @free(ptr %clos.env9)
  %20 = load ptr, ptr %b2, align 8
  call void @Future_drop(ptr %20)
  %21 = load ptr, ptr %sel, align 8
  call void @Future_complete_int(ptr %21, i32 0)
  %22 = load ptr, ptr %sel, align 8
  ret ptr %22

merge:                                            ; preds = %entry
  %23 = load ptr, ptr %b2, align 8
  %24 = getelementptr inbounds nuw %FutureHdr, ptr %23, i32 0, i32 1
  %25 = load { ptr, ptr }, ptr %onB, align 8
  store { ptr, ptr } %25, ptr %24, align 8
  %26 = load ptr, ptr %b2, align 8
  %27 = getelementptr inbounds nuw %FutureHdr, ptr %26, i32 0, i32 0
  %28 = cmpxchg ptr %27, i32 0, i32 1 acq_rel acquire, align 4
  %atm.cas.ok10 = extractvalue { i32, i1 } %28, 1
  %29 = xor i1 %atm.cas.ok10, true
  br i1 %29, label %then11, label %merge12

then11:                                           ; preds = %merge
  %30 = load { ptr, ptr }, ptr %onB, align 8
  %clos.env13 = extractvalue { ptr, ptr } %30, 1
  call void @free(ptr %clos.env13)
  %31 = load ptr, ptr %a1, align 8
  call void @Future_drop(ptr %31)
  %32 = load ptr, ptr %sel, align 8
  call void @Future_complete_int(ptr %32, i32 1)
  %33 = load ptr, ptr %sel, align 8
  ret ptr %33

merge12:                                          ; preds = %merge
  %34 = load ptr, ptr %sel, align 8
  %on_drop = getelementptr inbounds nuw %Future_int, ptr %34, i32 0, i32 2
  %on_drop14 = load { ptr, ptr }, ptr %on_drop, align 8
  %clos.env15 = extractvalue { ptr, ptr } %on_drop14, 1
  call void @free(ptr %clos.env15)
  %35 = load ptr, ptr %sel, align 8
  %36 = getelementptr inbounds nuw %Future_int, ptr %35, i32 0, i32 2
  %__lambda5.env.heap = call ptr @malloc(i64 16)
  %a16 = load ptr, ptr %a1, align 8
  %37 = getelementptr inbounds nuw %__lambda5.env, ptr %__lambda5.env.heap, i32 0, i32 0
  store ptr %a16, ptr %37, align 8
  %b17 = load ptr, ptr %b2, align 8
  %38 = getelementptr inbounds nuw %__lambda5.env, ptr %__lambda5.env.heap, i32 0, i32 1
  store ptr %b17, ptr %38, align 8
  %39 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda5.fat, i32 0, i32 0
  %40 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda5.fat, i32 0, i32 1
  store ptr @__lambda5, ptr %39, align 8
  store ptr %__lambda5.env.heap, ptr %40, align 8
  %__lambda5.fat.val = load { ptr, ptr }, ptr %__lambda5.fat, align 8
  store { ptr, ptr } %__lambda5.fat.val, ptr %36, align 8
  %41 = load ptr, ptr %sel, align 8
  ret ptr %41
}

define noundef ptr @_join2_hdr(ptr noundef %a, ptr noundef %b) {
entry:
  %__lambda12.fat = alloca { ptr, ptr }, align 8
  %__lambda11.fat = alloca { ptr, ptr }, align 8
  %__lambda10.fat = alloca { ptr, ptr }, align 8
  %__lambda8.fat = alloca { ptr, ptr }, align 8
  %onB = alloca { ptr, ptr }, align 8
  %__lambda6.fat = alloca { ptr, ptr }, align 8
  %onA = alloca { ptr, ptr }, align 8
  %cnt = alloca ptr, align 8
  %j = alloca ptr, align 8
  %b2 = alloca ptr, align 8
  %a1 = alloca ptr, align 8
  store ptr %a, ptr %a1, align 8
  store ptr %b, ptr %b2, align 8
  %0 = call ptr @future_new_int()
  store ptr %0, ptr %j, align 8
  %1 = call ptr @alloc_int(i64 1)
  store ptr %1, ptr %cnt, align 8
  %__lambda6.env.heap = call ptr @malloc(i64 24)
  %a3 = load ptr, ptr %a1, align 8
  %2 = getelementptr inbounds nuw %__lambda6.env, ptr %__lambda6.env.heap, i32 0, i32 0
  store ptr %a3, ptr %2, align 8
  %cnt4 = load ptr, ptr %cnt, align 8
  %3 = getelementptr inbounds nuw %__lambda6.env, ptr %__lambda6.env.heap, i32 0, i32 1
  store ptr %cnt4, ptr %3, align 8
  %j5 = load ptr, ptr %j, align 8
  %4 = getelementptr inbounds nuw %__lambda6.env, ptr %__lambda6.env.heap, i32 0, i32 2
  store ptr %j5, ptr %4, align 8
  %5 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda6.fat, i32 0, i32 0
  %6 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda6.fat, i32 0, i32 1
  store ptr @__lambda6, ptr %5, align 8
  store ptr %__lambda6.env.heap, ptr %6, align 8
  %__lambda6.fat.val = load { ptr, ptr }, ptr %__lambda6.fat, align 8
  store { ptr, ptr } %__lambda6.fat.val, ptr %onA, align 8
  %__lambda8.env.heap = call ptr @malloc(i64 24)
  %b6 = load ptr, ptr %b2, align 8
  %7 = getelementptr inbounds nuw %__lambda8.env, ptr %__lambda8.env.heap, i32 0, i32 0
  store ptr %b6, ptr %7, align 8
  %cnt7 = load ptr, ptr %cnt, align 8
  %8 = getelementptr inbounds nuw %__lambda8.env, ptr %__lambda8.env.heap, i32 0, i32 1
  store ptr %cnt7, ptr %8, align 8
  %j8 = load ptr, ptr %j, align 8
  %9 = getelementptr inbounds nuw %__lambda8.env, ptr %__lambda8.env.heap, i32 0, i32 2
  store ptr %j8, ptr %9, align 8
  %10 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda8.fat, i32 0, i32 0
  %11 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda8.fat, i32 0, i32 1
  store ptr @__lambda8, ptr %10, align 8
  store ptr %__lambda8.env.heap, ptr %11, align 8
  %__lambda8.fat.val = load { ptr, ptr }, ptr %__lambda8.fat, align 8
  store { ptr, ptr } %__lambda8.fat.val, ptr %onB, align 8
  %12 = load ptr, ptr %a1, align 8
  %13 = getelementptr inbounds nuw %FutureHdr, ptr %12, i32 0, i32 1
  %14 = load { ptr, ptr }, ptr %onA, align 8
  store { ptr, ptr } %14, ptr %13, align 8
  %15 = load ptr, ptr %a1, align 8
  %16 = getelementptr inbounds nuw %FutureHdr, ptr %15, i32 0, i32 0
  %17 = cmpxchg ptr %16, i32 0, i32 1 acq_rel acquire, align 4
  %atm.cas.ok = extractvalue { i32, i1 } %17, 1
  %18 = xor i1 %atm.cas.ok, true
  br i1 %18, label %then, label %merge

then:                                             ; preds = %entry
  %19 = load ptr, ptr %cnt, align 8
  %20 = load i32, ptr %19, align 4
  %21 = add i32 %20, 1
  store i32 %21, ptr %19, align 4
  %22 = load { ptr, ptr }, ptr %onA, align 8
  %clos.env = extractvalue { ptr, ptr } %22, 1
  call void @free(ptr %clos.env)
  %23 = load ptr, ptr %a1, align 8
  %24 = getelementptr inbounds nuw %FutureHdr, ptr %23, i32 0, i32 1
  %25 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda10.fat, i32 0, i32 0
  %26 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda10.fat, i32 0, i32 1
  store ptr @__lambda10, ptr %25, align 8
  store ptr null, ptr %26, align 8
  %__lambda10.fat.val = load { ptr, ptr }, ptr %__lambda10.fat, align 8
  store { ptr, ptr } %__lambda10.fat.val, ptr %24, align 8
  br label %merge

merge:                                            ; preds = %then, %entry
  %27 = load ptr, ptr %b2, align 8
  %28 = getelementptr inbounds nuw %FutureHdr, ptr %27, i32 0, i32 1
  %29 = load { ptr, ptr }, ptr %onB, align 8
  store { ptr, ptr } %29, ptr %28, align 8
  %30 = load ptr, ptr %b2, align 8
  %31 = getelementptr inbounds nuw %FutureHdr, ptr %30, i32 0, i32 0
  %32 = cmpxchg ptr %31, i32 0, i32 1 acq_rel acquire, align 4
  %atm.cas.ok9 = extractvalue { i32, i1 } %32, 1
  %33 = xor i1 %atm.cas.ok9, true
  br i1 %33, label %then10, label %merge11

then10:                                           ; preds = %merge
  %34 = load ptr, ptr %cnt, align 8
  %35 = load i32, ptr %34, align 4
  %36 = add i32 %35, 1
  store i32 %36, ptr %34, align 4
  %37 = load { ptr, ptr }, ptr %onB, align 8
  %clos.env12 = extractvalue { ptr, ptr } %37, 1
  call void @free(ptr %clos.env12)
  %38 = load ptr, ptr %b2, align 8
  %39 = getelementptr inbounds nuw %FutureHdr, ptr %38, i32 0, i32 1
  %40 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda11.fat, i32 0, i32 0
  %41 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda11.fat, i32 0, i32 1
  store ptr @__lambda11, ptr %40, align 8
  store ptr null, ptr %41, align 8
  %__lambda11.fat.val = load { ptr, ptr }, ptr %__lambda11.fat, align 8
  store { ptr, ptr } %__lambda11.fat.val, ptr %39, align 8
  br label %merge11

merge11:                                          ; preds = %then10, %merge
  %42 = load ptr, ptr %cnt, align 8
  %43 = load i32, ptr %42, align 4
  %44 = icmp eq i32 %43, 2
  br i1 %44, label %then13, label %merge14

then13:                                           ; preds = %merge11
  %45 = load ptr, ptr %cnt, align 8
  call void @free(ptr %45)
  %46 = load ptr, ptr %j, align 8
  call void @Future_complete_int(ptr %46, i32 2)
  %47 = load ptr, ptr %j, align 8
  ret ptr %47

merge14:                                          ; preds = %merge11
  %48 = load ptr, ptr %j, align 8
  %on_drop = getelementptr inbounds nuw %Future_int, ptr %48, i32 0, i32 2
  %on_drop15 = load { ptr, ptr }, ptr %on_drop, align 8
  %clos.env16 = extractvalue { ptr, ptr } %on_drop15, 1
  call void @free(ptr %clos.env16)
  %49 = load ptr, ptr %j, align 8
  %50 = getelementptr inbounds nuw %Future_int, ptr %49, i32 0, i32 2
  %__lambda12.env.heap = call ptr @malloc(i64 24)
  %a17 = load ptr, ptr %a1, align 8
  %51 = getelementptr inbounds nuw %__lambda12.env, ptr %__lambda12.env.heap, i32 0, i32 0
  store ptr %a17, ptr %51, align 8
  %b18 = load ptr, ptr %b2, align 8
  %52 = getelementptr inbounds nuw %__lambda12.env, ptr %__lambda12.env.heap, i32 0, i32 1
  store ptr %b18, ptr %52, align 8
  %cnt19 = load ptr, ptr %cnt, align 8
  %53 = getelementptr inbounds nuw %__lambda12.env, ptr %__lambda12.env.heap, i32 0, i32 2
  store ptr %cnt19, ptr %53, align 8
  %54 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda12.fat, i32 0, i32 0
  %55 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda12.fat, i32 0, i32 1
  store ptr @__lambda12, ptr %54, align 8
  store ptr %__lambda12.env.heap, ptr %55, align 8
  %__lambda12.fat.val = load { ptr, ptr }, ptr %__lambda12.fat, align 8
  store { ptr, ptr } %__lambda12.fat.val, ptr %50, align 8
  %56 = load ptr, ptr %j, align 8
  ret ptr %56
}

define void @Future_drop(ptr noundef %f) {
entry:
  %old = alloca i32, align 4
  %f1 = alloca ptr, align 8
  store ptr %f, ptr %f1, align 8
  %0 = load ptr, ptr %f1, align 8
  %1 = getelementptr inbounds nuw %FutureHdr, ptr %0, i32 0, i32 0
  %atm.load = load atomic i32, ptr %1 acquire, align 4
  %2 = icmp sge i32 %atm.load, 4
  br i1 %2, label %then, label %merge

then:                                             ; preds = %entry
  ret void

merge:                                            ; preds = %entry
  %3 = load ptr, ptr %f1, align 8
  %4 = getelementptr inbounds nuw %FutureHdr, ptr %3, i32 0, i32 0
  %5 = atomicrmw xchg ptr %4, i32 3 acq_rel, align 4
  store i32 %5, ptr %old, align 4
  %6 = load i32, ptr %old, align 4
  %7 = icmp ne i32 %6, 2
  br i1 %7, label %then2, label %merge3

then2:                                            ; preds = %merge
  %8 = load ptr, ptr %f1, align 8
  %on_drop = getelementptr inbounds nuw %FutureHdr, ptr %8, i32 0, i32 2
  %on_drop4 = load { ptr, ptr }, ptr %on_drop, align 8
  %fn.ptr = extractvalue { ptr, ptr } %on_drop4, 0
  %env.ptr = extractvalue { ptr, ptr } %on_drop4, 1
  call void %fn.ptr(ptr %env.ptr)
  br label %merge3

merge3:                                           ; preds = %then2, %merge
  %9 = load ptr, ptr %f1, align 8
  %waker = getelementptr inbounds nuw %FutureHdr, ptr %9, i32 0, i32 1
  %waker5 = load { ptr, ptr }, ptr %waker, align 8
  %clos.env = extractvalue { ptr, ptr } %waker5, 1
  call void @free(ptr %clos.env)
  %10 = load ptr, ptr %f1, align 8
  %on_drop6 = getelementptr inbounds nuw %FutureHdr, ptr %10, i32 0, i32 2
  %on_drop7 = load { ptr, ptr }, ptr %on_drop6, align 8
  %clos.env8 = extractvalue { ptr, ptr } %on_drop7, 1
  call void @free(ptr %clos.env8)
  %11 = load ptr, ptr %f1, align 8
  %12 = getelementptr inbounds nuw %FutureHdr, ptr %11, i32 0, i32 0
  %13 = cmpxchg ptr %12, i32 4, i32 5 acq_rel acquire, align 4
  %atm.cas.ok = extractvalue { i32, i1 } %13, 1
  br i1 %atm.cas.ok, label %then9, label %merge10

then9:                                            ; preds = %merge3
  ret void

merge10:                                          ; preds = %merge3
  %14 = load ptr, ptr %f1, align 8
  call void @free(ptr %14)
  ret void
}

define noundef ptr @produce() {
entry:
  %f = alloca ptr, align 8
  %0 = call ptr @future_new_int()
  store ptr %0, ptr %f, align 8
  %1 = load ptr, ptr %f, align 8
  call void @Future_complete_int(ptr %1, i32 41)
  %2 = load ptr, ptr %f, align 8
  ret ptr %2
}

define void @__one_resume(ptr noundef %__fr) {
entry:
  %__lambda13.fat = alloca { ptr, ptr }, align 8
  %__fr1 = alloca ptr, align 8
  store ptr %__fr, ptr %__fr1, align 8
  br label %while

while:                                            ; preds = %merge, %entry
  br i1 true, label %while_body, label %while_exit

while_body:                                       ; preds = %while
  %0 = load ptr, ptr %__fr1, align 8
  %st = getelementptr inbounds nuw %__one_frame, ptr %0, i32 0, i32 1
  %st2 = load i32, ptr %st, align 4
  %1 = icmp eq i32 %st2, 0
  br i1 %1, label %then, label %else

while_exit:                                       ; preds = %while
  ret void

then:                                             ; preds = %while_body
  %2 = load ptr, ptr %__fr1, align 8
  %3 = getelementptr inbounds nuw %__one_frame, ptr %2, i32 0, i32 4
  %4 = call ptr @produce()
  store ptr %4, ptr %3, align 8
  %5 = load ptr, ptr %__fr1, align 8
  %__aw0 = getelementptr inbounds nuw %__one_frame, ptr %5, i32 0, i32 4
  %__aw03 = load ptr, ptr %__aw0, align 8
  %__lambda13.env.heap = call ptr @malloc(i64 8)
  %__fr4 = load ptr, ptr %__fr1, align 8
  %6 = getelementptr inbounds nuw %__lambda13.env, ptr %__lambda13.env.heap, i32 0, i32 0
  store ptr %__fr4, ptr %6, align 8
  %7 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda13.fat, i32 0, i32 0
  %8 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda13.fat, i32 0, i32 1
  store ptr @__lambda13, ptr %7, align 8
  store ptr %__lambda13.env.heap, ptr %8, align 8
  %__lambda13.fat.val = load { ptr, ptr }, ptr %__lambda13.fat, align 8
  %9 = call i32 @Future_poll_int(ptr %__aw03, { ptr, ptr } %__lambda13.fat.val)
  %10 = icmp eq i32 %9, 0
  br i1 %10, label %then5, label %merge6

merge:                                            ; preds = %merge12, %merge6
  br label %while

else:                                             ; preds = %while_body
  %11 = load ptr, ptr %__fr1, align 8
  %st9 = getelementptr inbounds nuw %__one_frame, ptr %11, i32 0, i32 1
  %st10 = load i32, ptr %st9, align 4
  %12 = icmp eq i32 %st10, 1
  br i1 %12, label %then11, label %else13

then5:                                            ; preds = %then
  %13 = load ptr, ptr %__fr1, align 8
  %14 = getelementptr inbounds nuw %__one_frame, ptr %13, i32 0, i32 2
  %15 = load ptr, ptr %__fr1, align 8
  %__aw07 = getelementptr inbounds nuw %__one_frame, ptr %15, i32 0, i32 4
  %__aw08 = load ptr, ptr %__aw07, align 8
  store ptr %__aw08, ptr %14, align 8
  ret void

merge6:                                           ; preds = %then
  %16 = load ptr, ptr %__fr1, align 8
  %17 = getelementptr inbounds nuw %__one_frame, ptr %16, i32 0, i32 1
  store i32 1, ptr %17, align 4
  br label %merge

then11:                                           ; preds = %else
  %18 = load ptr, ptr %__fr1, align 8
  %19 = getelementptr inbounds nuw %__one_frame, ptr %18, i32 0, i32 2
  store ptr null, ptr %19, align 8
  %20 = load ptr, ptr %__fr1, align 8
  %21 = getelementptr inbounds nuw %__one_frame, ptr %20, i32 0, i32 3
  %22 = load ptr, ptr %__fr1, align 8
  %__aw014 = getelementptr inbounds nuw %__one_frame, ptr %22, i32 0, i32 4
  %__aw015 = load ptr, ptr %__aw014, align 8
  %value = getelementptr inbounds nuw %Future_int, ptr %__aw015, i32 0, i32 3
  %value16 = load i32, ptr %value, align 4
  store i32 %value16, ptr %21, align 4
  %23 = load ptr, ptr %__fr1, align 8
  %__aw017 = getelementptr inbounds nuw %__one_frame, ptr %23, i32 0, i32 4
  %__aw018 = load ptr, ptr %__aw017, align 8
  %waker = getelementptr inbounds nuw %Future_int, ptr %__aw018, i32 0, i32 1
  %waker19 = load { ptr, ptr }, ptr %waker, align 8
  %clos.env = extractvalue { ptr, ptr } %waker19, 1
  call void @free(ptr %clos.env)
  %24 = load ptr, ptr %__fr1, align 8
  %__aw020 = getelementptr inbounds nuw %__one_frame, ptr %24, i32 0, i32 4
  %__aw021 = load ptr, ptr %__aw020, align 8
  call void @Future_free_int(ptr %__aw021)
  %25 = load ptr, ptr %__fr1, align 8
  %26 = getelementptr inbounds nuw %__one_frame, ptr %25, i32 0, i32 0
  %27 = getelementptr inbounds nuw %Future_int, ptr %26, i32 0, i32 3
  %28 = load ptr, ptr %__fr1, align 8
  %n = getelementptr inbounds nuw %__one_frame, ptr %28, i32 0, i32 3
  %n22 = load i32, ptr %n, align 4
  %29 = add i32 %n22, 1
  store i32 %29, ptr %27, align 4
  %30 = load ptr, ptr %__fr1, align 8
  %31 = getelementptr inbounds nuw %__one_frame, ptr %30, i32 0, i32 0
  %32 = getelementptr inbounds nuw %Future_int, ptr %31, i32 0, i32 0
  %33 = atomicrmw xchg ptr %32, i32 2 acq_rel, align 4
  %34 = icmp eq i32 %33, 1
  br i1 %34, label %then23, label %merge24

merge12:                                          ; No predecessors!
  br label %merge

else13:                                           ; preds = %else
  ret void

then23:                                           ; preds = %then11
  %35 = load ptr, ptr %__fr1, align 8
  %36 = getelementptr inbounds nuw %__one_frame, ptr %35, i32 0, i32 0
  %waker25 = getelementptr inbounds nuw %Future_int, ptr %36, i32 0, i32 1
  %waker26 = load { ptr, ptr }, ptr %waker25, align 8
  %fn.ptr = extractvalue { ptr, ptr } %waker26, 0
  %env.ptr = extractvalue { ptr, ptr } %waker26, 1
  call void %fn.ptr(ptr %env.ptr)
  br label %merge24

merge24:                                          ; preds = %then23, %then11
  ret void
}

define noundef ptr @one() {
entry:
  %__lambda15.fat = alloca { ptr, ptr }, align 8
  %__lambda14.fat = alloca { ptr, ptr }, align 8
  %__fr = alloca ptr, align 8
  %0 = call ptr @alloc___one_frame(i64 1)
  store ptr %0, ptr %__fr, align 8
  %1 = load ptr, ptr %__fr, align 8
  %2 = getelementptr inbounds nuw %__one_frame, ptr %1, i32 0, i32 1
  store i32 0, ptr %2, align 4
  %3 = load ptr, ptr %__fr, align 8
  %4 = getelementptr inbounds nuw %__one_frame, ptr %3, i32 0, i32 2
  store ptr null, ptr %4, align 8
  %5 = load ptr, ptr %__fr, align 8
  %6 = getelementptr inbounds nuw %__one_frame, ptr %5, i32 0, i32 0
  %7 = getelementptr inbounds nuw %Future_int, ptr %6, i32 0, i32 0
  store i32 0, ptr %7, align 4
  %8 = load ptr, ptr %__fr, align 8
  %9 = getelementptr inbounds nuw %__one_frame, ptr %8, i32 0, i32 0
  %10 = getelementptr inbounds nuw %Future_int, ptr %9, i32 0, i32 1
  %11 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda14.fat, i32 0, i32 0
  %12 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda14.fat, i32 0, i32 1
  store ptr @__lambda14, ptr %11, align 8
  store ptr null, ptr %12, align 8
  %__lambda14.fat.val = load { ptr, ptr }, ptr %__lambda14.fat, align 8
  store { ptr, ptr } %__lambda14.fat.val, ptr %10, align 8
  %13 = load ptr, ptr %__fr, align 8
  %14 = getelementptr inbounds nuw %__one_frame, ptr %13, i32 0, i32 0
  %15 = getelementptr inbounds nuw %Future_int, ptr %14, i32 0, i32 2
  %__lambda15.env.heap = call ptr @malloc(i64 8)
  %__fr1 = load ptr, ptr %__fr, align 8
  %16 = getelementptr inbounds nuw %__lambda15.env, ptr %__lambda15.env.heap, i32 0, i32 0
  store ptr %__fr1, ptr %16, align 8
  %17 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda15.fat, i32 0, i32 0
  %18 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda15.fat, i32 0, i32 1
  store ptr @__lambda15, ptr %17, align 8
  store ptr %__lambda15.env.heap, ptr %18, align 8
  %__lambda15.fat.val = load { ptr, ptr }, ptr %__lambda15.fat, align 8
  store { ptr, ptr } %__lambda15.fat.val, ptr %15, align 8
  %19 = load ptr, ptr %__fr, align 8
  call void @__one_resume(ptr %19)
  %20 = load ptr, ptr %__fr, align 8
  %21 = getelementptr inbounds nuw %__one_frame, ptr %20, i32 0, i32 0
  ret ptr %21
}

define noundef i32 @main() {
entry:
  %r = alloca ptr, align 8
  %0 = call ptr @one()
  store ptr %0, ptr %r, align 8
  %1 = load ptr, ptr %r, align 8
  %value = getelementptr inbounds nuw %Future_int, ptr %1, i32 0, i32 3
  %value1 = load i32, ptr %value, align 4
  %2 = load ptr, ptr %r, align 8
  %state = getelementptr inbounds nuw %Future_int, ptr %2, i32 0, i32 0
  %state2 = load i32, ptr %state, align 4
  %3 = call i32 (ptr, ...) @printf(ptr @0, i32 %value1, i32 %state2)
  %4 = load ptr, ptr %r, align 8
  call void @Future_free_int(ptr %4)
  ret i32 0
}

declare ptr @malloc(i64)

define internal void @__lambda0(ptr %env) {
entry:
  %self = alloca { ptr, ptr }, align 8
  %f = alloca ptr, align 8
  %f.gep = getelementptr inbounds nuw %__lambda0.env, ptr %env, i32 0, i32 0
  %f.val = load ptr, ptr %f.gep, align 8
  store ptr %f.val, ptr %f, align 8
  %0 = load ptr, ptr %f, align 8
  %waker = getelementptr inbounds nuw %FutureHdr, ptr %0, i32 0, i32 1
  %waker1 = load { ptr, ptr }, ptr %waker, align 8
  store { ptr, ptr } %waker1, ptr %self, align 8
  %1 = load ptr, ptr %f, align 8
  %on_drop = getelementptr inbounds nuw %FutureHdr, ptr %1, i32 0, i32 2
  %on_drop2 = load { ptr, ptr }, ptr %on_drop, align 8
  %clos.env = extractvalue { ptr, ptr } %on_drop2, 1
  call void @free(ptr %clos.env)
  %2 = load ptr, ptr %f, align 8
  call void @free(ptr %2)
  %3 = load { ptr, ptr }, ptr %self, align 8
  %clos.env3 = extractvalue { ptr, ptr } %3, 1
  call void @free(ptr %clos.env3)
  ret void
}

define noundef ptr @future_new_int() {
entry:
  %__lambda2.fat = alloca { ptr, ptr }, align 8
  %__lambda1.fat = alloca { ptr, ptr }, align 8
  %f = alloca ptr, align 8
  %0 = call ptr @alloc_Future_int(i64 1)
  store ptr %0, ptr %f, align 8
  %1 = load ptr, ptr %f, align 8
  %2 = getelementptr inbounds nuw %Future_int, ptr %1, i32 0, i32 0
  store i32 0, ptr %2, align 4
  %3 = load ptr, ptr %f, align 8
  %4 = getelementptr inbounds nuw %Future_int, ptr %3, i32 0, i32 1
  %5 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda1.fat, i32 0, i32 0
  %6 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda1.fat, i32 0, i32 1
  store ptr @__lambda1, ptr %5, align 8
  store ptr null, ptr %6, align 8
  %__lambda1.fat.val = load { ptr, ptr }, ptr %__lambda1.fat, align 8
  store { ptr, ptr } %__lambda1.fat.val, ptr %4, align 8
  %7 = load ptr, ptr %f, align 8
  %8 = getelementptr inbounds nuw %Future_int, ptr %7, i32 0, i32 2
  %9 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda2.fat, i32 0, i32 0
  %10 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda2.fat, i32 0, i32 1
  store ptr @__lambda2, ptr %9, align 8
  store ptr null, ptr %10, align 8
  %__lambda2.fat.val = load { ptr, ptr }, ptr %__lambda2.fat, align 8
  store { ptr, ptr } %__lambda2.fat.val, ptr %8, align 8
  %11 = load ptr, ptr %f, align 8
  ret ptr %11
}

define noundef ptr @alloc_Future_int(i64 noundef %n) {
entry:
  %n1 = alloca i64, align 8
  store i64 %n, ptr %n1, align 8
  %0 = load i64, ptr %n1, align 8
  %1 = call ptr @calloc(i64 %0, i64 48)
  ret ptr %1
}

; Function Attrs: nounwind
define internal void @__lambda1(ptr %env) #0 {
entry:
  ret void
}

; Function Attrs: nounwind
define internal void @__lambda2(ptr %env) #0 {
entry:
  ret void
}

define internal void @__lambda3(ptr %env) {
entry:
  %self = alloca { ptr, ptr }, align 8
  %sel = alloca ptr, align 8
  %b = alloca ptr, align 8
  %a = alloca ptr, align 8
  %a.gep = getelementptr inbounds nuw %__lambda3.env, ptr %env, i32 0, i32 0
  %a.val = load ptr, ptr %a.gep, align 8
  store ptr %a.val, ptr %a, align 8
  %b.gep = getelementptr inbounds nuw %__lambda3.env, ptr %env, i32 0, i32 1
  %b.val = load ptr, ptr %b.gep, align 8
  store ptr %b.val, ptr %b, align 8
  %sel.gep = getelementptr inbounds nuw %__lambda3.env, ptr %env, i32 0, i32 2
  %sel.val = load ptr, ptr %sel.gep, align 8
  store ptr %sel.val, ptr %sel, align 8
  %0 = load ptr, ptr %a, align 8
  %waker = getelementptr inbounds nuw %FutureHdr, ptr %0, i32 0, i32 1
  %waker1 = load { ptr, ptr }, ptr %waker, align 8
  store { ptr, ptr } %waker1, ptr %self, align 8
  %1 = load ptr, ptr %b, align 8
  call void @Future_drop(ptr %1)
  %2 = load ptr, ptr %sel, align 8
  call void @Future_complete_int(ptr %2, i32 0)
  %3 = load { ptr, ptr }, ptr %self, align 8
  %clos.env = extractvalue { ptr, ptr } %3, 1
  call void @free(ptr %clos.env)
  ret void
}

define void @Future_complete_int(ptr noundef %f, i32 noundef %v) {
entry:
  %v2 = alloca i32, align 4
  %f1 = alloca ptr, align 8
  store ptr %f, ptr %f1, align 8
  store i32 %v, ptr %v2, align 4
  %0 = load ptr, ptr %f1, align 8
  %1 = getelementptr inbounds nuw %Future_int, ptr %0, i32 0, i32 3
  %2 = load i32, ptr %v2, align 4
  store i32 %2, ptr %1, align 4
  %3 = load ptr, ptr %f1, align 8
  %4 = getelementptr inbounds nuw %Future_int, ptr %3, i32 0, i32 0
  %5 = cmpxchg ptr %4, i32 0, i32 2 acq_rel acquire, align 4
  %atm.cas.ok = extractvalue { i32, i1 } %5, 1
  br i1 %atm.cas.ok, label %then, label %merge

then:                                             ; preds = %entry
  ret void

merge:                                            ; preds = %entry
  %6 = load ptr, ptr %f1, align 8
  %7 = getelementptr inbounds nuw %Future_int, ptr %6, i32 0, i32 0
  %8 = cmpxchg ptr %7, i32 1, i32 2 acq_rel acquire, align 4
  %atm.cas.ok3 = extractvalue { i32, i1 } %8, 1
  br i1 %atm.cas.ok3, label %then4, label %merge5

then4:                                            ; preds = %merge
  %9 = load ptr, ptr %f1, align 8
  %waker = getelementptr inbounds nuw %Future_int, ptr %9, i32 0, i32 1
  %waker6 = load { ptr, ptr }, ptr %waker, align 8
  %fn.ptr = extractvalue { ptr, ptr } %waker6, 0
  %env.ptr = extractvalue { ptr, ptr } %waker6, 1
  call void %fn.ptr(ptr %env.ptr)
  br label %merge5

merge5:                                           ; preds = %then4, %merge
  ret void
}

define internal void @__lambda4(ptr %env) {
entry:
  %self = alloca { ptr, ptr }, align 8
  %sel = alloca ptr, align 8
  %b = alloca ptr, align 8
  %a = alloca ptr, align 8
  %a.gep = getelementptr inbounds nuw %__lambda4.env, ptr %env, i32 0, i32 0
  %a.val = load ptr, ptr %a.gep, align 8
  store ptr %a.val, ptr %a, align 8
  %b.gep = getelementptr inbounds nuw %__lambda4.env, ptr %env, i32 0, i32 1
  %b.val = load ptr, ptr %b.gep, align 8
  store ptr %b.val, ptr %b, align 8
  %sel.gep = getelementptr inbounds nuw %__lambda4.env, ptr %env, i32 0, i32 2
  %sel.val = load ptr, ptr %sel.gep, align 8
  store ptr %sel.val, ptr %sel, align 8
  %0 = load ptr, ptr %b, align 8
  %waker = getelementptr inbounds nuw %FutureHdr, ptr %0, i32 0, i32 1
  %waker1 = load { ptr, ptr }, ptr %waker, align 8
  store { ptr, ptr } %waker1, ptr %self, align 8
  %1 = load ptr, ptr %a, align 8
  call void @Future_drop(ptr %1)
  %2 = load ptr, ptr %sel, align 8
  call void @Future_complete_int(ptr %2, i32 1)
  %3 = load { ptr, ptr }, ptr %self, align 8
  %clos.env = extractvalue { ptr, ptr } %3, 1
  call void @free(ptr %clos.env)
  ret void
}

define internal void @__lambda5(ptr %env) {
entry:
  %b = alloca ptr, align 8
  %a = alloca ptr, align 8
  %a.gep = getelementptr inbounds nuw %__lambda5.env, ptr %env, i32 0, i32 0
  %a.val = load ptr, ptr %a.gep, align 8
  store ptr %a.val, ptr %a, align 8
  %b.gep = getelementptr inbounds nuw %__lambda5.env, ptr %env, i32 0, i32 1
  %b.val = load ptr, ptr %b.gep, align 8
  store ptr %b.val, ptr %b, align 8
  %0 = load ptr, ptr %a, align 8
  call void @Future_drop(ptr %0)
  %1 = load ptr, ptr %b, align 8
  call void @Future_drop(ptr %1)
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

define internal void @__lambda6(ptr %env) {
entry:
  %__lambda7.fat = alloca { ptr, ptr }, align 8
  %self = alloca { ptr, ptr }, align 8
  %j = alloca ptr, align 8
  %cnt = alloca ptr, align 8
  %a = alloca ptr, align 8
  %a.gep = getelementptr inbounds nuw %__lambda6.env, ptr %env, i32 0, i32 0
  %a.val = load ptr, ptr %a.gep, align 8
  store ptr %a.val, ptr %a, align 8
  %cnt.gep = getelementptr inbounds nuw %__lambda6.env, ptr %env, i32 0, i32 1
  %cnt.val = load ptr, ptr %cnt.gep, align 8
  store ptr %cnt.val, ptr %cnt, align 8
  %j.gep = getelementptr inbounds nuw %__lambda6.env, ptr %env, i32 0, i32 2
  %j.val = load ptr, ptr %j.gep, align 8
  store ptr %j.val, ptr %j, align 8
  %0 = load ptr, ptr %a, align 8
  %waker = getelementptr inbounds nuw %FutureHdr, ptr %0, i32 0, i32 1
  %waker1 = load { ptr, ptr }, ptr %waker, align 8
  store { ptr, ptr } %waker1, ptr %self, align 8
  %1 = load ptr, ptr %a, align 8
  %2 = getelementptr inbounds nuw %FutureHdr, ptr %1, i32 0, i32 1
  %3 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda7.fat, i32 0, i32 0
  %4 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda7.fat, i32 0, i32 1
  store ptr @__lambda7, ptr %3, align 8
  store ptr null, ptr %4, align 8
  %__lambda7.fat.val = load { ptr, ptr }, ptr %__lambda7.fat, align 8
  store { ptr, ptr } %__lambda7.fat.val, ptr %2, align 8
  %5 = load ptr, ptr %cnt, align 8
  %6 = load i32, ptr %5, align 4
  %7 = add i32 %6, 1
  store i32 %7, ptr %5, align 4
  %8 = load ptr, ptr %cnt, align 8
  %9 = load i32, ptr %8, align 4
  %10 = icmp eq i32 %9, 2
  br i1 %10, label %then, label %merge

then:                                             ; preds = %entry
  %11 = load ptr, ptr %cnt, align 8
  call void @free(ptr %11)
  %12 = load ptr, ptr %j, align 8
  call void @Future_complete_int(ptr %12, i32 2)
  br label %merge

merge:                                            ; preds = %then, %entry
  %13 = load { ptr, ptr }, ptr %self, align 8
  %clos.env = extractvalue { ptr, ptr } %13, 1
  call void @free(ptr %clos.env)
  ret void
}

; Function Attrs: nounwind
define internal void @__lambda7(ptr %env) #0 {
entry:
  ret void
}

define internal void @__lambda8(ptr %env) {
entry:
  %__lambda9.fat = alloca { ptr, ptr }, align 8
  %self = alloca { ptr, ptr }, align 8
  %j = alloca ptr, align 8
  %cnt = alloca ptr, align 8
  %b = alloca ptr, align 8
  %b.gep = getelementptr inbounds nuw %__lambda8.env, ptr %env, i32 0, i32 0
  %b.val = load ptr, ptr %b.gep, align 8
  store ptr %b.val, ptr %b, align 8
  %cnt.gep = getelementptr inbounds nuw %__lambda8.env, ptr %env, i32 0, i32 1
  %cnt.val = load ptr, ptr %cnt.gep, align 8
  store ptr %cnt.val, ptr %cnt, align 8
  %j.gep = getelementptr inbounds nuw %__lambda8.env, ptr %env, i32 0, i32 2
  %j.val = load ptr, ptr %j.gep, align 8
  store ptr %j.val, ptr %j, align 8
  %0 = load ptr, ptr %b, align 8
  %waker = getelementptr inbounds nuw %FutureHdr, ptr %0, i32 0, i32 1
  %waker1 = load { ptr, ptr }, ptr %waker, align 8
  store { ptr, ptr } %waker1, ptr %self, align 8
  %1 = load ptr, ptr %b, align 8
  %2 = getelementptr inbounds nuw %FutureHdr, ptr %1, i32 0, i32 1
  %3 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda9.fat, i32 0, i32 0
  %4 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda9.fat, i32 0, i32 1
  store ptr @__lambda9, ptr %3, align 8
  store ptr null, ptr %4, align 8
  %__lambda9.fat.val = load { ptr, ptr }, ptr %__lambda9.fat, align 8
  store { ptr, ptr } %__lambda9.fat.val, ptr %2, align 8
  %5 = load ptr, ptr %cnt, align 8
  %6 = load i32, ptr %5, align 4
  %7 = add i32 %6, 1
  store i32 %7, ptr %5, align 4
  %8 = load ptr, ptr %cnt, align 8
  %9 = load i32, ptr %8, align 4
  %10 = icmp eq i32 %9, 2
  br i1 %10, label %then, label %merge

then:                                             ; preds = %entry
  %11 = load ptr, ptr %cnt, align 8
  call void @free(ptr %11)
  %12 = load ptr, ptr %j, align 8
  call void @Future_complete_int(ptr %12, i32 2)
  br label %merge

merge:                                            ; preds = %then, %entry
  %13 = load { ptr, ptr }, ptr %self, align 8
  %clos.env = extractvalue { ptr, ptr } %13, 1
  call void @free(ptr %clos.env)
  ret void
}

; Function Attrs: nounwind
define internal void @__lambda9(ptr %env) #0 {
entry:
  ret void
}

; Function Attrs: nounwind
define internal void @__lambda10(ptr %env) #0 {
entry:
  ret void
}

; Function Attrs: nounwind
define internal void @__lambda11(ptr %env) #0 {
entry:
  ret void
}

define internal void @__lambda12(ptr %env) {
entry:
  %cnt = alloca ptr, align 8
  %b = alloca ptr, align 8
  %a = alloca ptr, align 8
  %a.gep = getelementptr inbounds nuw %__lambda12.env, ptr %env, i32 0, i32 0
  %a.val = load ptr, ptr %a.gep, align 8
  store ptr %a.val, ptr %a, align 8
  %b.gep = getelementptr inbounds nuw %__lambda12.env, ptr %env, i32 0, i32 1
  %b.val = load ptr, ptr %b.gep, align 8
  store ptr %b.val, ptr %b, align 8
  %cnt.gep = getelementptr inbounds nuw %__lambda12.env, ptr %env, i32 0, i32 2
  %cnt.val = load ptr, ptr %cnt.gep, align 8
  store ptr %cnt.val, ptr %cnt, align 8
  %0 = load ptr, ptr %cnt, align 8
  call void @free(ptr %0)
  %1 = load ptr, ptr %a, align 8
  call void @Future_drop(ptr %1)
  %2 = load ptr, ptr %b, align 8
  call void @Future_drop(ptr %2)
  ret void
}

; Function Attrs: nounwind
define noundef i32 @Future_poll_int(ptr noundef %f, { ptr, ptr } %resume) #0 {
entry:
  %resume2 = alloca { ptr, ptr }, align 8
  %f1 = alloca ptr, align 8
  store ptr %f, ptr %f1, align 8
  store { ptr, ptr } %resume, ptr %resume2, align 8
  %0 = load ptr, ptr %f1, align 8
  %1 = getelementptr inbounds nuw %Future_int, ptr %0, i32 0, i32 1
  %2 = load { ptr, ptr }, ptr %resume2, align 8
  store { ptr, ptr } %2, ptr %1, align 8
  %3 = load ptr, ptr %f1, align 8
  %4 = getelementptr inbounds nuw %Future_int, ptr %3, i32 0, i32 0
  %5 = cmpxchg ptr %4, i32 0, i32 1 acq_rel acquire, align 4
  %atm.cas.ok = extractvalue { i32, i1 } %5, 1
  br i1 %atm.cas.ok, label %then, label %merge

then:                                             ; preds = %entry
  ret i32 0

merge:                                            ; preds = %entry
  ret i32 1
}

define internal void @__lambda13(ptr %env) {
entry:
  %__fr = alloca ptr, align 8
  %__fr.gep = getelementptr inbounds nuw %__lambda13.env, ptr %env, i32 0, i32 0
  %__fr.val = load ptr, ptr %__fr.gep, align 8
  store ptr %__fr.val, ptr %__fr, align 8
  %0 = load ptr, ptr %__fr, align 8
  %1 = getelementptr inbounds nuw %__one_frame, ptr %0, i32 0, i32 1
  store i32 1, ptr %1, align 4
  %2 = load ptr, ptr %__fr, align 8
  call void @__one_resume(ptr %2)
  ret void
}

define void @Future_free_int(ptr noundef %f) {
entry:
  %f1 = alloca ptr, align 8
  store ptr %f, ptr %f1, align 8
  %0 = load ptr, ptr %f1, align 8
  %on_drop = getelementptr inbounds nuw %Future_int, ptr %0, i32 0, i32 2
  %on_drop2 = load { ptr, ptr }, ptr %on_drop, align 8
  %clos.env = extractvalue { ptr, ptr } %on_drop2, 1
  call void @free(ptr %clos.env)
  %1 = load ptr, ptr %f1, align 8
  call void @free(ptr %1)
  ret void
}

define noundef ptr @alloc___one_frame(i64 noundef %n) {
entry:
  %n1 = alloca i64, align 8
  store i64 %n, ptr %n1, align 8
  %0 = load i64, ptr %n1, align 8
  %1 = call ptr @calloc(i64 %0, i64 80)
  ret ptr %1
}

; Function Attrs: nounwind
define internal void @__lambda14(ptr %env) #0 {
entry:
  ret void
}

define internal void @__lambda15(ptr %env) {
entry:
  %__fr = alloca ptr, align 8
  %__fr.gep = getelementptr inbounds nuw %__lambda15.env, ptr %env, i32 0, i32 0
  %__fr.val = load ptr, ptr %__fr.gep, align 8
  store ptr %__fr.val, ptr %__fr, align 8
  %0 = load ptr, ptr %__fr, align 8
  %awaiting = getelementptr inbounds nuw %__one_frame, ptr %0, i32 0, i32 2
  %awaiting1 = load ptr, ptr %awaiting, align 8
  %1 = icmp ne ptr %awaiting1, null
  br i1 %1, label %then, label %merge

then:                                             ; preds = %entry
  %2 = load ptr, ptr %__fr, align 8
  %awaiting2 = getelementptr inbounds nuw %__one_frame, ptr %2, i32 0, i32 2
  %awaiting3 = load ptr, ptr %awaiting2, align 8
  call void @Future_drop(ptr %awaiting3)
  br label %merge

merge:                                            ; preds = %then, %entry
  ret void
}

attributes #0 = { nounwind }
