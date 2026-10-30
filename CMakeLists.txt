cmake_minimum_required(VERSION 3.20)
project(eskiuc VERSION 0.14.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -DESKIU_STATIC=ON links LLVM statically (used for release binaries)
option(ESKIU_STATIC "Link LLVM libraries statically" OFF)

# -DESKIU_WERROR=ON turns the compiler's own warnings into errors (CI)
option(ESKIU_WERROR "Treat warnings in the eskiuc sources as errors" OFF)

find_package(LLVM REQUIRED CONFIG)

# The compiler uses dynamic_cast throughout (AST dispatch), and its TUs derive from LLVM
# classes (cl::opt, passes), so they need the typeinfo an RTTI build of LLVM exports. The
# llvm-project release tarballs are built without RTTI and fail at link time with
# "undefined symbol: typeinfo for llvm::cl::GenericOptionValue": stop here instead.
if(NOT LLVM_ENABLE_RTTI)
    message(FATAL_ERROR
        "LLVM at ${LLVM_DIR} was built without RTTI (LLVM_ENABLE_RTTI=OFF), which eskiuc "
        "needs. Use a packaged LLVM (apt.llvm.org, Homebrew, your distribution, MSYS2) "
        "instead of the llvm-project release tarball. See docs/lang/build.md.")
endif()

# SYSTEM: LLVM's headers are not ours, so their -Wunused-parameter and
# -Wmissing-field-initializers do not bury the compiler's own warnings.
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
# LLVM_DEFINITIONS is a single space-separated string. Split it into a list so a
# define carrying a value (e.g. MinGW's `-D_GLIBCXX_USE_CXX11_ABI=1`) stays one
# token instead of being mangled with the defs that follow (which made the compiler
# choke on `=` in a preprocessor expression). No-op where LLVM_DEFINITIONS is simple.
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
add_definitions(${LLVM_DEFINITIONS_LIST})
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

set(SOURCES
    main.cpp
    main_support.cpp
    repl.cpp
    pkg.cpp
    lexer/lexer.cpp
    lexer/preprocessor.cpp
    parser/parser.cpp
    parser/parse_decl.cpp
    parser/parse_stmt.cpp
    parser/parse_expr.cpp
    ast/ast.cpp
    ast/ast_printer.cpp
    sema/type.cpp
    sema/type_checker.cpp
    sema/typecheck_decl.cpp
    sema/typecheck_stmt.cpp
    sema/typecheck_expr.cpp
    sema/typecheck_type.cpp
    sema/async_transform.cpp
    codegen/codegen_module.cpp
    codegen/codegen_type.cpp
    codegen/codegen_scope.cpp
    codegen/codegen_decl.cpp
    codegen/codegen_stmt.cpp
    codegen/codegen_expr.cpp
    codegen/codegen_call.cpp
    codegen/codegen_closure.cpp
    codegen/codegen_adt.cpp
    codegen/codegen_cabi.cpp
    codegen/codegen_repl.cpp
    eir/eir.cpp
    eir/eir_builder.cpp
    eir/eir_vm.cpp
    eir/eir_asm_arm64.cpp
    eir/eir_c.cpp
    eir/eir_obj.cpp
    eir/eir_runtime.cpp
)

add_executable(eskiuc ${SOURCES})

# X86 backend available unless static build on Apple Silicon
if(NOT (ESKIU_STATIC AND APPLE))
    target_compile_definitions(eskiuc PRIVATE ESKIU_HAS_X86)
endif()

if(ESKIU_STATIC)
    # Static LLVM — release binaries.
    # On Apple Silicon, X86 static libs are not available in Homebrew LLVM.
    if(APPLE)
        set(STATIC_BACKENDS "aarch64codegen aarch64asmparser aarch64desc aarch64info armcodegen armasmparser armdesc arminfo webassemblycodegen webassemblyasmparser webassemblydesc webassemblyinfo")
    else()
        set(STATIC_BACKENDS "aarch64codegen aarch64asmparser aarch64desc aarch64info x86codegen x86asmparser x86desc x86info armcodegen armasmparser armdesc arminfo webassemblycodegen webassemblyasmparser webassemblydesc webassemblyinfo")
    endif()
    execute_process(
        COMMAND ${LLVM_TOOLS_BINARY_DIR}/llvm-config --link-static --libs
                support core irreader object nativecodegen orcjit ${STATIC_BACKENDS}
        OUTPUT_VARIABLE LLVM_STATIC_LIBS
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(
        COMMAND ${LLVM_TOOLS_BINARY_DIR}/llvm-config --system-libs
        OUTPUT_VARIABLE LLVM_SYSTEM_LIBS
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    separate_arguments(LLVM_STATIC_LIBS UNIX_COMMAND "${LLVM_STATIC_LIBS}")
    separate_arguments(LLVM_SYSTEM_LIBS UNIX_COMMAND "${LLVM_SYSTEM_LIBS}")
    target_link_libraries(eskiuc ${LLVM_STATIC_LIBS} ${LLVM_SYSTEM_LIBS})
else()
    # Dynamic LLVM — default for development (X86 available as dylib on all platforms)
    llvm_map_components_to_libnames(llvm_libs
        support core irreader object
        nativecodegen orcjit
        aarch64codegen aarch64asmparser aarch64desc aarch64info
        x86codegen x86asmparser x86desc x86info
        armcodegen armasmparser armdesc arminfo
        webassemblycodegen webassemblyasmparser webassemblydesc webassemblyinfo)
    target_link_libraries(eskiuc ${llvm_libs})
endif()

# The sanitizer instrumentation (ASan) is emitted by this exact LLVM, so its
# runtime must match — bake in the LLVM toolchain's clang for sanitized links.
target_compile_definitions(eskiuc PRIVATE
    ESKIU_LLVM_BINDIR="${LLVM_TOOLS_BINARY_DIR}")

if(MSVC)
    target_compile_options(eskiuc PRIVATE /W4)
    if(ESKIU_WERROR)
        target_compile_options(eskiuc PRIVATE /WX)
    endif()
else()
    target_compile_options(eskiuc PRIVATE -Wall -Wextra -Wpedantic)
    if(ESKIU_WERROR)
        target_compile_options(eskiuc PRIVATE -Werror)
    endif()
endif()

# ── Dual-build: the Eskiu-written compiler ────────────────────────────
# The C++ `eskiuc` above is the bootstrap SEED and the differential ORACLE. It also builds
# the Eskiu-WRITTEN compiler from selfhost/esk_main.esk as `eskiuc-esk`, so the self-hosted
# binary is a first-class build artifact (its behavior is dogfood-verified in CI by the
# bootstrap and corpus-parity gates). It is deliberately NOT the shipped artifact: it emits
# LLVM IR text and shells out to clang to link (no LLVM bindings, by design), so it needs
# clang at runtime, whereas the C++ eskiuc bundles LLVM and is self-contained. The
# distribution keeps shipping the C++ binary. See docs/dev/self-hosting.md.
find_program(ESKIU_SELFHOST_CLANG NAMES clang clang-22 clang-19 clang-18 clang-17)
file(GLOB ESKIU_SELFHOST_SRC CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/selfhost/*.esk")
if(ESKIU_SELFHOST_CLANG AND NOT MSVC)
    add_custom_command(
        OUTPUT "${CMAKE_BINARY_DIR}/eskiuc-esk"
        COMMAND "${CMAKE_COMMAND}" -E env
                "ESKIU_ROOT=${CMAKE_SOURCE_DIR}" "CLANG=${ESKIU_SELFHOST_CLANG}"
                "$<TARGET_FILE:eskiuc>" "${CMAKE_SOURCE_DIR}/selfhost/esk_main.esk"
                -o "${CMAKE_BINARY_DIR}/eskiuc-esk"
        DEPENDS eskiuc ${ESKIU_SELFHOST_SRC}
        COMMENT "Building the Eskiu-written compiler (eskiuc-esk) with the C++ seed"
        VERBATIM)
    # Target name differs from the output basename to avoid a self-referential dependency.
    add_custom_target(eskiuc-selfhost ALL DEPENDS "${CMAKE_BINARY_DIR}/eskiuc-esk")
else()
    message(STATUS "eskiuc-esk (self-hosted compiler) skipped: clang not found or MSVC")
endif()
