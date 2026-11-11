
%__lambda0.env = type { ptr }
%FutureHdr = type { i32, { ptr, ptr }, { ptr, ptr } }
%__lambda3.env = type { ptr, ptr, ptr }
%__lambda4.env = type { ptr, ptr, ptr }
%Future_int = type { i32, { ptr, ptr }, { ptr, ptr }, i32 }
%__lambda5.env = type { ptr, ptr }
%__lambda6.env = type { ptr, ptr, ptr }
%__lambda8.env = type { ptr, ptr, ptr }
%__lambda12.env = type { ptr, ptr, ptr }
%__consume3_frame = type { %Future_int, i32, ptr, ptr, i32, ptr, i32, ptr, i32, ptr }
%__lambda14.env = type { ptr }
%__lambda15.env = type { ptr }
%__lambda16.env = type { ptr }
%__lambda18.env = type { ptr }
%Chan_int = type { ptr, i32, i32, i32, ptr }
%__lambda13.env = type { ptr, ptr }

@PENDING = global i32 0
@WAITING = global i32 1
@READY = global i32 2
@CANCELLED = global i32 3
@DETACHED = global i32 4
@ORPHANED = global i32 5
@CLEANED = global i32 6
@0 = private unnamed_addr constant [13 x i8] c"buffered=%d\0A\00", align 1
@1 = private unnamed_addr constant [11 x i8] c"parked=%d\0A\00", align 1

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

define void @__consume3_resume(ptr noundef %__fr) {
entry:
  %__lambda16.fat = alloca { ptr, ptr }, align 8
  %__lambda15.fat = alloca { ptr, ptr }, align 8
  %__lambda14.fat = alloca { ptr, ptr }, align 8
  %__fr1 = alloca ptr, align 8
  store ptr %__fr, ptr %__fr1, align 8
  br label %while

while:                                            ; preds = %merge, %entry
  br i1 true, label %while_body, label %while_exit

while_body:                                       ; preds = %while
  %0 = load ptr, ptr %__fr1, align 8
  %st = getelementptr inbounds nuw %__consume3_frame, ptr %0, i32 0, i32 1
  %st2 = load i32, ptr %st, align 4
  %1 = icmp eq i32 %st2, 0
  br i1 %1, label %then, label %else

while_exit:                                       ; preds = %while
  ret void

then:                                             ; preds = %while_body
  %2 = load ptr, ptr %__fr1, align 8
  %3 = getelementptr inbounds nuw %__consume3_frame, ptr %2, i32 0, i32 5
  %4 = load ptr, ptr %__fr1, align 8
  %ch = getelementptr inbounds nuw %__consume3_frame, ptr %4, i32 0, i32 3
  %ch3 = load ptr, ptr %ch, align 8
  %5 = call ptr @Chan_recv_int(ptr %ch3)
  store ptr %5, ptr %3, align 8
  %6 = load ptr, ptr %__fr1, align 8
  %__aw0 = getelementptr inbounds nuw %__consume3_frame, ptr %6, i32 0, i32 5
  %__aw04 = load ptr, ptr %__aw0, align 8
  %__lambda14.env.heap = call ptr @malloc(i64 8)
  %__fr5 = load ptr, ptr %__fr1, align 8
  %7 = getelementptr inbounds nuw %__lambda14.env, ptr %__lambda14.env.heap, i32 0, i32 0
  store ptr %__fr5, ptr %7, align 8
  %8 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda14.fat, i32 0, i32 0
  %9 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda14.fat, i32 0, i32 1
  store ptr @__lambda14, ptr %8, align 8
  store ptr %__lambda14.env.heap, ptr %9, align 8
  %__lambda14.fat.val = load { ptr, ptr }, ptr %__lambda14.fat, align 8
  %10 = call i32 @Future_poll_int(ptr %__aw04, { ptr, ptr } %__lambda14.fat.val)
  %11 = icmp eq i32 %10, 0
  br i1 %11, label %then6, label %merge7

merge:                                            ; preds = %merge13, %merge7
  br label %while

else:                                             ; preds = %while_body
  %12 = load ptr, ptr %__fr1, align 8
  %st10 = getelementptr inbounds nuw %__consume3_frame, ptr %12, i32 0, i32 1
  %st11 = load i32, ptr %st10, align 4
  %13 = icmp eq i32 %st11, 1
  br i1 %13, label %then12, label %else14

then6:                                            ; preds = %then
  %14 = load ptr, ptr %__fr1, align 8
  %15 = getelementptr inbounds nuw %__consume3_frame, ptr %14, i32 0, i32 2
  %16 = load ptr, ptr %__fr1, align 8
  %__aw08 = getelementptr inbounds nuw %__consume3_frame, ptr %16, i32 0, i32 5
  %__aw09 = load ptr, ptr %__aw08, align 8
  store ptr %__aw09, ptr %15, align 8
  ret void

merge7:                                           ; preds = %then
  %17 = load ptr, ptr %__fr1, align 8
  %18 = getelementptr inbounds nuw %__consume3_frame, ptr %17, i32 0, i32 1
  store i32 1, ptr %18, align 4
  br label %merge

then12:                                           ; preds = %else
  %19 = load ptr, ptr %__fr1, align 8
  %20 = getelementptr inbounds nuw %__consume3_frame, ptr %19, i32 0, i32 2
  store ptr null, ptr %20, align 8
  %21 = load ptr, ptr %__fr1, align 8
  %22 = getelementptr inbounds nuw %__consume3_frame, ptr %21, i32 0, i32 4
  %23 = load ptr, ptr %__fr1, align 8
  %__aw015 = getelementptr inbounds nuw %__consume3_frame, ptr %23, i32 0, i32 5
  %__aw016 = load ptr, ptr %__aw015, align 8
  %value = getelementptr inbounds nuw %Future_int, ptr %__aw016, i32 0, i32 3
  %value17 = load i32, ptr %value, align 4
  store i32 %value17, ptr %22, align 4
  %24 = load ptr, ptr %__fr1, align 8
  %__aw018 = getelementptr inbounds nuw %__consume3_frame, ptr %24, i32 0, i32 5
  %__aw019 = load ptr, ptr %__aw018, align 8
  %waker = getelementptr inbounds nuw %Future_int, ptr %__aw019, i32 0, i32 1
  %waker20 = load { ptr, ptr }, ptr %waker, align 8
  %clos.env = extractvalue { ptr, ptr } %waker20, 1
  call void @free(ptr %clos.env)
  %25 = load ptr, ptr %__fr1, align 8
  %__aw021 = getelementptr inbounds nuw %__consume3_frame, ptr %25, i32 0, i32 5
  %__aw022 = load ptr, ptr %__aw021, align 8
  call void @Future_free_int(ptr %__aw022)
  %26 = load ptr, ptr %__fr1, align 8
  %27 = getelementptr inbounds nuw %__consume3_frame, ptr %26, i32 0, i32 7
  %28 = load ptr, ptr %__fr1, align 8
  %ch23 = getelementptr inbounds nuw %__consume3_frame, ptr %28, i32 0, i32 3
  %ch24 = load ptr, ptr %ch23, align 8
  %29 = call ptr @Chan_recv_int(ptr %ch24)
  store ptr %29, ptr %27, align 8
  %30 = load ptr, ptr %__fr1, align 8
  %__aw1 = getelementptr inbounds nuw %__consume3_frame, ptr %30, i32 0, i32 7
  %__aw125 = load ptr, ptr %__aw1, align 8
  %__lambda15.env.heap = call ptr @malloc(i64 8)
  %__fr26 = load ptr, ptr %__fr1, align 8
  %31 = getelementptr inbounds nuw %__lambda15.env, ptr %__lambda15.env.heap, i32 0, i32 0
  store ptr %__fr26, ptr %31, align 8
  %32 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda15.fat, i32 0, i32 0
  %33 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda15.fat, i32 0, i32 1
  store ptr @__lambda15, ptr %32, align 8
  store ptr %__lambda15.env.heap, ptr %33, align 8
  %__lambda15.fat.val = load { ptr, ptr }, ptr %__lambda15.fat, align 8
  %34 = call i32 @Future_poll_int(ptr %__aw125, { ptr, ptr } %__lambda15.fat.val)
  %35 = icmp eq i32 %34, 0
  br i1 %35, label %then27, label %merge28

merge13:                                          ; preds = %merge34, %merge28
  br label %merge

else14:                                           ; preds = %else
  %36 = load ptr, ptr %__fr1, align 8
  %st31 = getelementptr inbounds nuw %__consume3_frame, ptr %36, i32 0, i32 1
  %st32 = load i32, ptr %st31, align 4
  %37 = icmp eq i32 %st32, 2
  br i1 %37, label %then33, label %else35

then27:                                           ; preds = %then12
  %38 = load ptr, ptr %__fr1, align 8
  %39 = getelementptr inbounds nuw %__consume3_frame, ptr %38, i32 0, i32 2
  %40 = load ptr, ptr %__fr1, align 8
  %__aw129 = getelementptr inbounds nuw %__consume3_frame, ptr %40, i32 0, i32 7
  %__aw130 = load ptr, ptr %__aw129, align 8
  store ptr %__aw130, ptr %39, align 8
  ret void

merge28:                                          ; preds = %then12
  %41 = load ptr, ptr %__fr1, align 8
  %42 = getelementptr inbounds nuw %__consume3_frame, ptr %41, i32 0, i32 1
  store i32 2, ptr %42, align 4
  br label %merge13

then33:                                           ; preds = %else14
  %43 = load ptr, ptr %__fr1, align 8
  %44 = getelementptr inbounds nuw %__consume3_frame, ptr %43, i32 0, i32 2
  store ptr null, ptr %44, align 8
  %45 = load ptr, ptr %__fr1, align 8
  %46 = getelementptr inbounds nuw %__consume3_frame, ptr %45, i32 0, i32 6
  %47 = load ptr, ptr %__fr1, align 8
  %__aw136 = getelementptr inbounds nuw %__consume3_frame, ptr %47, i32 0, i32 7
  %__aw137 = load ptr, ptr %__aw136, align 8
  %value38 = getelementptr inbounds nuw %Future_int, ptr %__aw137, i32 0, i32 3
  %value39 = load i32, ptr %value38, align 4
  store i32 %value39, ptr %46, align 4
  %48 = load ptr, ptr %__fr1, align 8
  %__aw140 = getelementptr inbounds nuw %__consume3_frame, ptr %48, i32 0, i32 7
  %__aw141 = load ptr, ptr %__aw140, align 8
  %waker42 = getelementptr inbounds nuw %Future_int, ptr %__aw141, i32 0, i32 1
  %waker43 = load { ptr, ptr }, ptr %waker42, align 8
  %clos.env44 = extractvalue { ptr, ptr } %waker43, 1
  call void @free(ptr %clos.env44)
  %49 = load ptr, ptr %__fr1, align 8
  %__aw145 = getelementptr inbounds nuw %__consume3_frame, ptr %49, i32 0, i32 7
  %__aw146 = load ptr, ptr %__aw145, align 8
  call void @Future_free_int(ptr %__aw146)
  %50 = load ptr, ptr %__fr1, align 8
  %51 = getelementptr inbounds nuw %__consume3_frame, ptr %50, i32 0, i32 9
  %52 = load ptr, ptr %__fr1, align 8
  %ch47 = getelementptr inbounds nuw %__consume3_frame, ptr %52, i32 0, i32 3
  %ch48 = load ptr, ptr %ch47, align 8
  %53 = call ptr @Chan_recv_int(ptr %ch48)
  store ptr %53, ptr %51, align 8
  %54 = load ptr, ptr %__fr1, align 8
  %__aw2 = getelementptr inbounds nuw %__consume3_frame, ptr %54, i32 0, i32 9
  %__aw249 = load ptr, ptr %__aw2, align 8
  %__lambda16.env.heap = call ptr @malloc(i64 8)
  %__fr50 = load ptr, ptr %__fr1, align 8
  %55 = getelementptr inbounds nuw %__lambda16.env, ptr %__lambda16.env.heap, i32 0, i32 0
  store ptr %__fr50, ptr %55, align 8
  %56 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda16.fat, i32 0, i32 0
  %57 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda16.fat, i32 0, i32 1
  store ptr @__lambda16, ptr %56, align 8
  store ptr %__lambda16.env.heap, ptr %57, align 8
  %__lambda16.fat.val = load { ptr, ptr }, ptr %__lambda16.fat, align 8
  %58 = call i32 @Future_poll_int(ptr %__aw249, { ptr, ptr } %__lambda16.fat.val)
  %59 = icmp eq i32 %58, 0
  br i1 %59, label %then51, label %merge52

merge34:                                          ; preds = %merge58, %merge52
  br label %merge13

else35:                                           ; preds = %else14
  %60 = load ptr, ptr %__fr1, align 8
  %st55 = getelementptr inbounds nuw %__consume3_frame, ptr %60, i32 0, i32 1
  %st56 = load i32, ptr %st55, align 4
  %61 = icmp eq i32 %st56, 3
  br i1 %61, label %then57, label %else59

then51:                                           ; preds = %then33
  %62 = load ptr, ptr %__fr1, align 8
  %63 = getelementptr inbounds nuw %__consume3_frame, ptr %62, i32 0, i32 2
  %64 = load ptr, ptr %__fr1, align 8
  %__aw253 = getelementptr inbounds nuw %__consume3_frame, ptr %64, i32 0, i32 9
  %__aw254 = load ptr, ptr %__aw253, align 8
  store ptr %__aw254, ptr %63, align 8
  ret void

merge52:                                          ; preds = %then33
  %65 = load ptr, ptr %__fr1, align 8
  %66 = getelementptr inbounds nuw %__consume3_frame, ptr %65, i32 0, i32 1
  store i32 3, ptr %66, align 4
  br label %merge34

then57:                                           ; preds = %else35
  %67 = load ptr, ptr %__fr1, align 8
  %68 = getelementptr inbounds nuw %__consume3_frame, ptr %67, i32 0, i32 2
  store ptr null, ptr %68, align 8
  %69 = load ptr, ptr %__fr1, align 8
  %70 = getelementptr inbounds nuw %__consume3_frame, ptr %69, i32 0, i32 8
  %71 = load ptr, ptr %__fr1, align 8
  %__aw260 = getelementptr inbounds nuw %__consume3_frame, ptr %71, i32 0, i32 9
  %__aw261 = load ptr, ptr %__aw260, align 8
  %value62 = getelementptr inbounds nuw %Future_int, ptr %__aw261, i32 0, i32 3
  %value63 = load i32, ptr %value62, align 4
  store i32 %value63, ptr %70, align 4
  %72 = load ptr, ptr %__fr1, align 8
  %__aw264 = getelementptr inbounds nuw %__consume3_frame, ptr %72, i32 0, i32 9
  %__aw265 = load ptr, ptr %__aw264, align 8
  %waker66 = getelementptr inbounds nuw %Future_int, ptr %__aw265, i32 0, i32 1
  %waker67 = load { ptr, ptr }, ptr %waker66, align 8
  %clos.env68 = extractvalue { ptr, ptr } %waker67, 1
  call void @free(ptr %clos.env68)
  %73 = load ptr, ptr %__fr1, align 8
  %__aw269 = getelementptr inbounds nuw %__consume3_frame, ptr %73, i32 0, i32 9
  %__aw270 = load ptr, ptr %__aw269, align 8
  call void @Future_free_int(ptr %__aw270)
  %74 = load ptr, ptr %__fr1, align 8
  %75 = getelementptr inbounds nuw %__consume3_frame, ptr %74, i32 0, i32 0
  %76 = getelementptr inbounds nuw %Future_int, ptr %75, i32 0, i32 3
  %77 = load ptr, ptr %__fr1, align 8
  %a = getelementptr inbounds nuw %__consume3_frame, ptr %77, i32 0, i32 4
  %a71 = load i32, ptr %a, align 4
  %78 = load ptr, ptr %__fr1, align 8
  %b = getelementptr inbounds nuw %__consume3_frame, ptr %78, i32 0, i32 6
  %b72 = load i32, ptr %b, align 4
  %79 = add i32 %a71, %b72
  %80 = load ptr, ptr %__fr1, align 8
  %c = getelementptr inbounds nuw %__consume3_frame, ptr %80, i32 0, i32 8
  %c73 = load i32, ptr %c, align 4
  %81 = add i32 %79, %c73
  store i32 %81, ptr %76, align 4
  %82 = load ptr, ptr %__fr1, align 8
  %83 = getelementptr inbounds nuw %__consume3_frame, ptr %82, i32 0, i32 0
  %84 = getelementptr inbounds nuw %Future_int, ptr %83, i32 0, i32 0
  %85 = atomicrmw xchg ptr %84, i32 2 acq_rel, align 4
  %86 = icmp eq i32 %85, 1
  br i1 %86, label %then74, label %merge75

merge58:                                          ; No predecessors!
  br label %merge34

else59:                                           ; preds = %else35
  ret void

then74:                                           ; preds = %then57
  %87 = load ptr, ptr %__fr1, align 8
  %88 = getelementptr inbounds nuw %__consume3_frame, ptr %87, i32 0, i32 0
  %waker76 = getelementptr inbounds nuw %Future_int, ptr %88, i32 0, i32 1
  %waker77 = load { ptr, ptr }, ptr %waker76, align 8
  %fn.ptr = extractvalue { ptr, ptr } %waker77, 0
  %env.ptr = extractvalue { ptr, ptr } %waker77, 1
  call void %fn.ptr(ptr %env.ptr)
  br label %merge75

merge75:                                          ; preds = %then74, %then57
  ret void
}

define noundef ptr @consume3(ptr noundef %ch) {
entry:
  %__lambda18.fat = alloca { ptr, ptr }, align 8
  %__lambda17.fat = alloca { ptr, ptr }, align 8
  %__fr = alloca ptr, align 8
  %ch1 = alloca ptr, align 8
  store ptr %ch, ptr %ch1, align 8
  %0 = call ptr @alloc___consume3_frame(i64 1)
  store ptr %0, ptr %__fr, align 8
  %1 = load ptr, ptr %__fr, align 8
  %2 = getelementptr inbounds nuw %__consume3_frame, ptr %1, i32 0, i32 1
  store i32 0, ptr %2, align 4
  %3 = load ptr, ptr %__fr, align 8
  %4 = getelementptr inbounds nuw %__consume3_frame, ptr %3, i32 0, i32 2
  store ptr null, ptr %4, align 8
  %5 = load ptr, ptr %__fr, align 8
  %6 = getelementptr inbounds nuw %__consume3_frame, ptr %5, i32 0, i32 0
  %7 = getelementptr inbounds nuw %Future_int, ptr %6, i32 0, i32 0
  store i32 0, ptr %7, align 4
  %8 = load ptr, ptr %__fr, align 8
  %9 = getelementptr inbounds nuw %__consume3_frame, ptr %8, i32 0, i32 0
  %10 = getelementptr inbounds nuw %Future_int, ptr %9, i32 0, i32 1
  %11 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda17.fat, i32 0, i32 0
  %12 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda17.fat, i32 0, i32 1
  store ptr @__lambda17, ptr %11, align 8
  store ptr null, ptr %12, align 8
  %__lambda17.fat.val = load { ptr, ptr }, ptr %__lambda17.fat, align 8
  store { ptr, ptr } %__lambda17.fat.val, ptr %10, align 8
  %13 = load ptr, ptr %__fr, align 8
  %14 = getelementptr inbounds nuw %__consume3_frame, ptr %13, i32 0, i32 0
  %15 = getelementptr inbounds nuw %Future_int, ptr %14, i32 0, i32 2
  %__lambda18.env.heap = call ptr @malloc(i64 8)
  %__fr2 = load ptr, ptr %__fr, align 8
  %16 = getelementptr inbounds nuw %__lambda18.env, ptr %__lambda18.env.heap, i32 0, i32 0
  store ptr %__fr2, ptr %16, align 8
  %17 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda18.fat, i32 0, i32 0
  %18 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda18.fat, i32 0, i32 1
  store ptr @__lambda18, ptr %17, align 8
  store ptr %__lambda18.env.heap, ptr %18, align 8
  %__lambda18.fat.val = load { ptr, ptr }, ptr %__lambda18.fat, align 8
  store { ptr, ptr } %__lambda18.fat.val, ptr %15, align 8
  %19 = load ptr, ptr %__fr, align 8
  %20 = getelementptr inbounds nuw %__consume3_frame, ptr %19, i32 0, i32 3
  %21 = load ptr, ptr %ch1, align 8
  store ptr %21, ptr %20, align 8
  %22 = load ptr, ptr %__fr, align 8
  call void @__consume3_resume(ptr %22)
  %23 = load ptr, ptr %__fr, align 8
  %24 = getelementptr inbounds nuw %__consume3_frame, ptr %23, i32 0, i32 0
  ret ptr %24
}

define noundef i32 @main() {
entry:
  %__lambda21.fat = alloca { ptr, ptr }, align 8
  %fp = alloca ptr, align 8
  %p = alloca ptr, align 8
  %__lambda19.fat = alloca { ptr, ptr }, align 8
  %fb = alloca ptr, align 8
  %b = alloca ptr, align 8
  %0 = call ptr @chan_new_int(i32 8)
  store ptr %0, ptr %b, align 8
  %1 = load ptr, ptr %b, align 8
  %2 = call i1 @Chan_send_int(ptr %1, i32 10)
  %3 = load ptr, ptr %b, align 8
  %4 = call i1 @Chan_send_int(ptr %3, i32 20)
  %5 = load ptr, ptr %b, align 8
  %6 = call i1 @Chan_send_int(ptr %5, i32 30)
  %7 = load ptr, ptr %b, align 8
  %8 = call ptr @consume3(ptr %7)
  store ptr %8, ptr %fb, align 8
  %9 = load ptr, ptr %fb, align 8
  %10 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda19.fat, i32 0, i32 0
  %11 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda19.fat, i32 0, i32 1
  store ptr @__lambda19, ptr %10, align 8
  store ptr null, ptr %11, align 8
  %__lambda19.fat.val = load { ptr, ptr }, ptr %__lambda19.fat, align 8
  %12 = call i32 @Future_poll_int(ptr %9, { ptr, ptr } %__lambda19.fat.val)
  %13 = load ptr, ptr %fb, align 8
  %value = getelementptr inbounds nuw %Future_int, ptr %13, i32 0, i32 3
  %value1 = load i32, ptr %value, align 4
  %14 = call i32 (ptr, ...) @printf(ptr @0, i32 %value1)
  %15 = load ptr, ptr %fb, align 8
  call void @Future_free_int(ptr %15)
  %16 = load ptr, ptr %b, align 8
  call void @Chan_free_int(ptr %16)
  %17 = call ptr @chan_new_int(i32 8)
  store ptr %17, ptr %p, align 8
  %18 = load ptr, ptr %p, align 8
  %19 = call ptr @consume3(ptr %18)
  store ptr %19, ptr %fp, align 8
  %20 = load ptr, ptr %fp, align 8
  %21 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda21.fat, i32 0, i32 0
  %22 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda21.fat, i32 0, i32 1
  store ptr @__lambda21, ptr %21, align 8
  store ptr null, ptr %22, align 8
  %__lambda21.fat.val = load { ptr, ptr }, ptr %__lambda21.fat, align 8
  %23 = call i32 @Future_poll_int(ptr %20, { ptr, ptr } %__lambda21.fat.val)
  %24 = load ptr, ptr %p, align 8
  %25 = call i1 @Chan_send_int(ptr %24, i32 1)
  %26 = load ptr, ptr %p, align 8
  %27 = call i1 @Chan_send_int(ptr %26, i32 2)
  %28 = load ptr, ptr %p, align 8
  %29 = call i1 @Chan_send_int(ptr %28, i32 4)
  %30 = load ptr, ptr %fp, align 8
  %value2 = getelementptr inbounds nuw %Future_int, ptr %30, i32 0, i32 3
  %value3 = load i32, ptr %value2, align 4
  %31 = call i32 (ptr, ...) @printf(ptr @1, i32 %value3)
  %32 = load ptr, ptr %fp, align 8
  call void @Future_free_int(ptr %32)
  %33 = load ptr, ptr %p, align 8
  call void @Chan_free_int(ptr %33)
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

define noundef ptr @Chan_recv_int(ptr noundef %ch) {
entry:
  %__lambda13.fat = alloca { ptr, ptr }, align 8
  %v = alloca i32, align 4
  %f = alloca ptr, align 8
  %ch1 = alloca ptr, align 8
  store ptr %ch, ptr %ch1, align 8
  %0 = call ptr @future_new_int()
  store ptr %0, ptr %f, align 8
  %1 = load ptr, ptr %ch1, align 8
  %count = getelementptr inbounds nuw %Chan_int, ptr %1, i32 0, i32 3
  %count2 = load i32, ptr %count, align 4
  %2 = icmp sgt i32 %count2, 0
  br i1 %2, label %then, label %else

then:                                             ; preds = %entry
  %3 = load ptr, ptr %ch1, align 8
  %head = getelementptr inbounds nuw %Chan_int, ptr %3, i32 0, i32 2
  %head3 = load i32, ptr %head, align 4
  %4 = sext i32 %head3 to i64
  %5 = load ptr, ptr %ch1, align 8
  %buf = getelementptr inbounds nuw %Chan_int, ptr %5, i32 0, i32 0
  %buf4 = load ptr, ptr %buf, align 8
  %6 = getelementptr i32, ptr %buf4, i64 %4
  %7 = load i32, ptr %6, align 4
  store i32 %7, ptr %v, align 4
  %8 = load ptr, ptr %ch1, align 8
  %9 = getelementptr inbounds nuw %Chan_int, ptr %8, i32 0, i32 2
  %10 = load i32, ptr %9, align 4
  %11 = add i32 %10, 1
  store i32 %11, ptr %9, align 4
  %12 = load ptr, ptr %ch1, align 8
  %head5 = getelementptr inbounds nuw %Chan_int, ptr %12, i32 0, i32 2
  %head6 = load i32, ptr %head5, align 4
  %13 = load ptr, ptr %ch1, align 8
  %cap = getelementptr inbounds nuw %Chan_int, ptr %13, i32 0, i32 1
  %cap7 = load i32, ptr %cap, align 4
  %14 = icmp sge i32 %head6, %cap7
  br i1 %14, label %then8, label %merge9

merge:                                            ; preds = %else, %merge9
  %15 = load ptr, ptr %f, align 8
  ret ptr %15

else:                                             ; preds = %entry
  %16 = load ptr, ptr %ch1, align 8
  %17 = getelementptr inbounds nuw %Chan_int, ptr %16, i32 0, i32 4
  %18 = load ptr, ptr %f, align 8
  store ptr %18, ptr %17, align 8
  %19 = load ptr, ptr %f, align 8
  %20 = getelementptr inbounds nuw %Future_int, ptr %19, i32 0, i32 2
  %__lambda13.env.heap = call ptr @malloc(i64 16)
  %ch10 = load ptr, ptr %ch1, align 8
  %21 = getelementptr inbounds nuw %__lambda13.env, ptr %__lambda13.env.heap, i32 0, i32 0
  store ptr %ch10, ptr %21, align 8
  %f11 = load ptr, ptr %f, align 8
  %22 = getelementptr inbounds nuw %__lambda13.env, ptr %__lambda13.env.heap, i32 0, i32 1
  store ptr %f11, ptr %22, align 8
  %23 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda13.fat, i32 0, i32 0
  %24 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda13.fat, i32 0, i32 1
  store ptr @__lambda13, ptr %23, align 8
  store ptr %__lambda13.env.heap, ptr %24, align 8
  %__lambda13.fat.val = load { ptr, ptr }, ptr %__lambda13.fat, align 8
  store { ptr, ptr } %__lambda13.fat.val, ptr %20, align 8
  br label %merge

then8:                                            ; preds = %then
  %25 = load ptr, ptr %ch1, align 8
  %26 = getelementptr inbounds nuw %Chan_int, ptr %25, i32 0, i32 2
  store i32 0, ptr %26, align 4
  br label %merge9

merge9:                                           ; preds = %then8, %then
  %27 = load ptr, ptr %ch1, align 8
  %28 = getelementptr inbounds nuw %Chan_int, ptr %27, i32 0, i32 3
  %29 = load i32, ptr %28, align 4
  %30 = sub i32 %29, 1
  store i32 %30, ptr %28, align 4
  %31 = load ptr, ptr %f, align 8
  %32 = load i32, ptr %v, align 4
  call void @Future_complete_int(ptr %31, i32 %32)
  br label %merge
}

; Function Attrs: nounwind
define internal void @__lambda13(ptr %env) #0 {
entry:
  %f = alloca ptr, align 8
  %ch = alloca ptr, align 8
  %ch.gep = getelementptr inbounds nuw %__lambda13.env, ptr %env, i32 0, i32 0
  %ch.val = load ptr, ptr %ch.gep, align 8
  store ptr %ch.val, ptr %ch, align 8
  %f.gep = getelementptr inbounds nuw %__lambda13.env, ptr %env, i32 0, i32 1
  %f.val = load ptr, ptr %f.gep, align 8
  store ptr %f.val, ptr %f, align 8
  %0 = load ptr, ptr %ch, align 8
  %waiter = getelementptr inbounds nuw %Chan_int, ptr %0, i32 0, i32 4
  %waiter1 = load ptr, ptr %waiter, align 8
  %1 = load ptr, ptr %f, align 8
  %2 = icmp eq ptr %waiter1, %1
  br i1 %2, label %then, label %merge

then:                                             ; preds = %entry
  %3 = load ptr, ptr %ch, align 8
  %4 = getelementptr inbounds nuw %Chan_int, ptr %3, i32 0, i32 4
  store ptr null, ptr %4, align 8
  br label %merge

merge:                                            ; preds = %then, %entry
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

define internal void @__lambda14(ptr %env) {
entry:
  %__fr = alloca ptr, align 8
  %__fr.gep = getelementptr inbounds nuw %__lambda14.env, ptr %env, i32 0, i32 0
  %__fr.val = load ptr, ptr %__fr.gep, align 8
  store ptr %__fr.val, ptr %__fr, align 8
  %0 = load ptr, ptr %__fr, align 8
  %1 = getelementptr inbounds nuw %__consume3_frame, ptr %0, i32 0, i32 1
  store i32 1, ptr %1, align 4
  %2 = load ptr, ptr %__fr, align 8
  call void @__consume3_resume(ptr %2)
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

define internal void @__lambda15(ptr %env) {
entry:
  %__fr = alloca ptr, align 8
  %__fr.gep = getelementptr inbounds nuw %__lambda15.env, ptr %env, i32 0, i32 0
  %__fr.val = load ptr, ptr %__fr.gep, align 8
  store ptr %__fr.val, ptr %__fr, align 8
  %0 = load ptr, ptr %__fr, align 8
  %1 = getelementptr inbounds nuw %__consume3_frame, ptr %0, i32 0, i32 1
  store i32 2, ptr %1, align 4
  %2 = load ptr, ptr %__fr, align 8
  call void @__consume3_resume(ptr %2)
  ret void
}

define internal void @__lambda16(ptr %env) {
entry:
  %__fr = alloca ptr, align 8
  %__fr.gep = getelementptr inbounds nuw %__lambda16.env, ptr %env, i32 0, i32 0
  %__fr.val = load ptr, ptr %__fr.gep, align 8
  store ptr %__fr.val, ptr %__fr, align 8
  %0 = load ptr, ptr %__fr, align 8
  %1 = getelementptr inbounds nuw %__consume3_frame, ptr %0, i32 0, i32 1
  store i32 3, ptr %1, align 4
  %2 = load ptr, ptr %__fr, align 8
  call void @__consume3_resume(ptr %2)
  ret void
}

define noundef ptr @alloc___consume3_frame(i64 noundef %n) {
entry:
  %n1 = alloca i64, align 8
  store i64 %n, ptr %n1, align 8
  %0 = load i64, ptr %n1, align 8
  %1 = call ptr @calloc(i64 %0, i64 120)
  ret ptr %1
}

; Function Attrs: nounwind
define internal void @__lambda17(ptr %env) #0 {
entry:
  ret void
}

define internal void @__lambda18(ptr %env) {
entry:
  %__fr = alloca ptr, align 8
  %__fr.gep = getelementptr inbounds nuw %__lambda18.env, ptr %env, i32 0, i32 0
  %__fr.val = load ptr, ptr %__fr.gep, align 8
  store ptr %__fr.val, ptr %__fr, align 8
  %0 = load ptr, ptr %__fr, align 8
  %awaiting = getelementptr inbounds nuw %__consume3_frame, ptr %0, i32 0, i32 2
  %awaiting1 = load ptr, ptr %awaiting, align 8
  %1 = icmp ne ptr %awaiting1, null
  br i1 %1, label %then, label %merge

then:                                             ; preds = %entry
  %2 = load ptr, ptr %__fr, align 8
  %awaiting2 = getelementptr inbounds nuw %__consume3_frame, ptr %2, i32 0, i32 2
  %awaiting3 = load ptr, ptr %awaiting2, align 8
  call void @Future_drop(ptr %awaiting3)
  br label %merge

merge:                                            ; preds = %then, %entry
  ret void
}

define noundef ptr @chan_new_int(i32 noundef %cap) {
entry:
  %ch = alloca ptr, align 8
  %cap1 = alloca i32, align 4
  store i32 %cap, ptr %cap1, align 4
  %0 = call ptr @alloc_Chan_int(i64 1)
  store ptr %0, ptr %ch, align 8
  %1 = load ptr, ptr %ch, align 8
  %2 = getelementptr inbounds nuw %Chan_int, ptr %1, i32 0, i32 0
  %3 = load i32, ptr %cap1, align 4
  %4 = sext i32 %3 to i64
  %5 = call ptr @alloc_int(i64 %4)
  store ptr %5, ptr %2, align 8
  %6 = load ptr, ptr %ch, align 8
  %7 = getelementptr inbounds nuw %Chan_int, ptr %6, i32 0, i32 1
  %8 = load i32, ptr %cap1, align 4
  store i32 %8, ptr %7, align 4
  %9 = load ptr, ptr %ch, align 8
  %10 = getelementptr inbounds nuw %Chan_int, ptr %9, i32 0, i32 2
  store i32 0, ptr %10, align 4
  %11 = load ptr, ptr %ch, align 8
  %12 = getelementptr inbounds nuw %Chan_int, ptr %11, i32 0, i32 3
  store i32 0, ptr %12, align 4
  %13 = load ptr, ptr %ch, align 8
  %14 = getelementptr inbounds nuw %Chan_int, ptr %13, i32 0, i32 4
  store ptr null, ptr %14, align 8
  %15 = load ptr, ptr %ch, align 8
  ret ptr %15
}

define noundef ptr @alloc_Chan_int(i64 noundef %n) {
entry:
  %n1 = alloca i64, align 8
  store i64 %n, ptr %n1, align 8
  %0 = load i64, ptr %n1, align 8
  %1 = call ptr @calloc(i64 %0, i64 32)
  ret ptr %1
}

define noundef zeroext i1 @Chan_send_int(ptr noundef %ch, i32 noundef %v) {
entry:
  %tail = alloca i32, align 4
  %w = alloca ptr, align 8
  %v2 = alloca i32, align 4
  %ch1 = alloca ptr, align 8
  store ptr %ch, ptr %ch1, align 8
  store i32 %v, ptr %v2, align 4
  %0 = load ptr, ptr %ch1, align 8
  %waiter = getelementptr inbounds nuw %Chan_int, ptr %0, i32 0, i32 4
  %waiter3 = load ptr, ptr %waiter, align 8
  %1 = icmp ne ptr %waiter3, null
  br i1 %1, label %then, label %merge

then:                                             ; preds = %entry
  %2 = load ptr, ptr %ch1, align 8
  %waiter4 = getelementptr inbounds nuw %Chan_int, ptr %2, i32 0, i32 4
  %waiter5 = load ptr, ptr %waiter4, align 8
  store ptr %waiter5, ptr %w, align 8
  %3 = load ptr, ptr %ch1, align 8
  %4 = getelementptr inbounds nuw %Chan_int, ptr %3, i32 0, i32 4
  store ptr null, ptr %4, align 8
  %5 = load ptr, ptr %w, align 8
  %6 = load i32, ptr %v2, align 4
  call void @Future_complete_int(ptr %5, i32 %6)
  ret i1 true

merge:                                            ; preds = %entry
  %7 = load ptr, ptr %ch1, align 8
  %count = getelementptr inbounds nuw %Chan_int, ptr %7, i32 0, i32 3
  %count6 = load i32, ptr %count, align 4
  %8 = load ptr, ptr %ch1, align 8
  %cap = getelementptr inbounds nuw %Chan_int, ptr %8, i32 0, i32 1
  %cap7 = load i32, ptr %cap, align 4
  %9 = icmp sge i32 %count6, %cap7
  br i1 %9, label %then8, label %merge9

then8:                                            ; preds = %merge
  ret i1 false

merge9:                                           ; preds = %merge
  %10 = load ptr, ptr %ch1, align 8
  %head = getelementptr inbounds nuw %Chan_int, ptr %10, i32 0, i32 2
  %head10 = load i32, ptr %head, align 4
  %11 = load ptr, ptr %ch1, align 8
  %count11 = getelementptr inbounds nuw %Chan_int, ptr %11, i32 0, i32 3
  %count12 = load i32, ptr %count11, align 4
  %12 = add i32 %head10, %count12
  store i32 %12, ptr %tail, align 4
  %13 = load i32, ptr %tail, align 4
  %14 = load ptr, ptr %ch1, align 8
  %cap13 = getelementptr inbounds nuw %Chan_int, ptr %14, i32 0, i32 1
  %cap14 = load i32, ptr %cap13, align 4
  %15 = icmp sge i32 %13, %cap14
  br i1 %15, label %then15, label %merge16

then15:                                           ; preds = %merge9
  %16 = load i32, ptr %tail, align 4
  %17 = load ptr, ptr %ch1, align 8
  %cap17 = getelementptr inbounds nuw %Chan_int, ptr %17, i32 0, i32 1
  %cap18 = load i32, ptr %cap17, align 4
  %18 = sub i32 %16, %cap18
  store i32 %18, ptr %tail, align 4
  br label %merge16

merge16:                                          ; preds = %then15, %merge9
  %19 = load i32, ptr %tail, align 4
  %20 = sext i32 %19 to i64
  %21 = load ptr, ptr %ch1, align 8
  %buf = getelementptr inbounds nuw %Chan_int, ptr %21, i32 0, i32 0
  %buf19 = load ptr, ptr %buf, align 8
  %22 = getelementptr i32, ptr %buf19, i64 %20
  %23 = load i32, ptr %v2, align 4
  store i32 %23, ptr %22, align 4
  %24 = load ptr, ptr %ch1, align 8
  %25 = getelementptr inbounds nuw %Chan_int, ptr %24, i32 0, i32 3
  %26 = load i32, ptr %25, align 4
  %27 = add i32 %26, 1
  store i32 %27, ptr %25, align 4
  ret i1 true
}

; Function Attrs: nounwind
define internal void @__lambda19(ptr %env) #0 {
entry:
  ret void
}

define void @Chan_free_int(ptr noundef %ch) {
entry:
  %__lambda20.fat = alloca { ptr, ptr }, align 8
  %w = alloca ptr, align 8
  %ch1 = alloca ptr, align 8
  store ptr %ch, ptr %ch1, align 8
  %0 = load ptr, ptr %ch1, align 8
  %waiter = getelementptr inbounds nuw %Chan_int, ptr %0, i32 0, i32 4
  %waiter2 = load ptr, ptr %waiter, align 8
  %1 = icmp ne ptr %waiter2, null
  br i1 %1, label %then, label %merge

then:                                             ; preds = %entry
  %2 = load ptr, ptr %ch1, align 8
  %waiter3 = getelementptr inbounds nuw %Chan_int, ptr %2, i32 0, i32 4
  %waiter4 = load ptr, ptr %waiter3, align 8
  store ptr %waiter4, ptr %w, align 8
  %3 = load ptr, ptr %ch1, align 8
  %4 = getelementptr inbounds nuw %Chan_int, ptr %3, i32 0, i32 4
  store ptr null, ptr %4, align 8
  %5 = load ptr, ptr %w, align 8
  %on_drop = getelementptr inbounds nuw %Future_int, ptr %5, i32 0, i32 2
  %on_drop5 = load { ptr, ptr }, ptr %on_drop, align 8
  %clos.env = extractvalue { ptr, ptr } %on_drop5, 1
  call void @free(ptr %clos.env)
  %6 = load ptr, ptr %w, align 8
  %7 = getelementptr inbounds nuw %Future_int, ptr %6, i32 0, i32 2
  %8 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda20.fat, i32 0, i32 0
  %9 = getelementptr inbounds nuw { ptr, ptr }, ptr %__lambda20.fat, i32 0, i32 1
  store ptr @__lambda20, ptr %8, align 8
  store ptr null, ptr %9, align 8
  %__lambda20.fat.val = load { ptr, ptr }, ptr %__lambda20.fat, align 8
  store { ptr, ptr } %__lambda20.fat.val, ptr %7, align 8
  br label %merge

merge:                                            ; preds = %then, %entry
  %10 = load ptr, ptr %ch1, align 8
  %buf = getelementptr inbounds nuw %Chan_int, ptr %10, i32 0, i32 0
  %buf6 = load ptr, ptr %buf, align 8
  call void @free(ptr %buf6)
  %11 = load ptr, ptr %ch1, align 8
  call void @free(ptr %11)
  ret void
}

; Function Attrs: nounwind
define internal void @__lambda20(ptr %env) #0 {
entry:
  ret void
}

; Function Attrs: nounwind
define internal void @__lambda21(ptr %env) #0 {
entry:
  ret void
}

attributes #0 = { nounwind }
