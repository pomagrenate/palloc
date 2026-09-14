cmake_minimum_required(VERSION 3.18)
project(libpalloc C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

option(PA_SECURE            "Use full security mitigations (like guard pages, allocation randomization, double-free mitigation, and free-list corruption detection)" OFF)
option(PA_PADDING           "Enable padding to detect heap block overflow (always on in DEBUG or SECURE mode, or with Valgrind/ASAN)" OFF)
option(PA_OVERRIDE          "Override the standard malloc interface (i.e. define entry points for 'malloc', 'free', etc)" ON)
option(PA_XMALLOC           "Enable abort() call on memory allocation failure by default" OFF)
option(PA_SHOW_ERRORS       "Show error and warning messages by default (only enabled by default in DEBUG mode)" OFF)
option(PA_GUARDED           "Build with guard pages behind certain object allocations (implies PA_NO_PADDING=ON)" OFF)
option(PA_USE_CXX           "Use the C++ compiler to compile the library (instead of the C compiler)" OFF)
option(PA_OPT_ARCH          "Only for optimized builds: turn on architecture specific optimizations (for arm64: '-march=armv8.1-a' (2016))" OFF)
option(PA_SEE_ASM           "Generate assembly files" OFF)
option(PA_OSX_INTERPOSE     "Use interpose to override standard malloc on macOS" ON)
option(PA_OSX_ZONE          "Use malloc zone to override standard malloc on macOS" ON)
option(PA_WIN_REDIRECT      "Use redirection module ('palloc-redirect') on Windows if compiling palloc as a DLL" ON)
option(PA_WIN_USE_FIXED_TLS "Use a fixed TLS slot on Windows to avoid extra tests in the malloc fast path" OFF)
option(PA_LOCAL_DYNAMIC_TLS "Use local-dynamic-tls, a slightly slower but dlopen-compatible thread local storage mechanism (Unix)" OFF)
option(PA_LIBC_MUSL         "Enable this when linking with musl libc" OFF)

option(PA_DEBUG             "Enable assertion checks (enabled by default in a debug build)" OFF)
option(PA_DEBUG_INTERNAL    "Enable assertion and internal invariant checks (enabled by default in a debug build)" OFF)
option(PA_DEBUG_FULL        "Enable assertion checks and expensive internal heap invariant checking" OFF)

option(PA_DEBUG_TSAN        "Build with thread sanitizer (needs clang)" OFF)
option(PA_DEBUG_UBSAN       "Build with undefined-behavior sanitizer (needs clang++)" OFF)
option(PA_TRACK_VALGRIND    "Compile with Valgrind support (adds a small overhead)" OFF)
option(PA_TRACK_ASAN        "Compile with address sanitizer support (adds a small overhead)" OFF)
option(PA_TRACK_ETW         "Compile with Windows event tracing (ETW) support (adds a small overhead)" OFF)

option(PA_BUILD_SHARED      "Build shared library" ON)
option(PA_BUILD_STATIC      "Build static library" ON)
option(PA_BUILD_OBJECT      "Build object library" ON)
option(PA_BUILD_TESTS       "Build test executables" ON)

option(PA_SKIP_COLLECT_ON_EXIT "Skip collecting memory on program exit" OFF)
option(PA_NO_PADDING        "Force no use of padding even in DEBUG mode etc." OFF)
option(PA_INSTALL_TOPLEVEL  "Install directly into $CMAKE_INSTALL_PREFIX instead of PREFIX/lib/palloc-version" OFF)
option(PA_NO_THP            "Disable transparent huge pages support on Linux/Android for the palloc process only" OFF)
option(PA_EXTRA_CPPDEFS     "Extra pre-processor definitions (use as `-DPA_EXTRA_CPPDEFS=\"opt1=val1;opt2=val2\"`)" "")
option(PA_VECTOR            "Build optional vector-oriented APIs (arenas, batch alloc, pool, SIMD alignment)" OFF)

# negated options for vcpkg features
option(PA_NO_USE_CXX        "Use plain C compilation (has priority over PA_USE_CXX)" OFF)
option(PA_NO_OPT_ARCH       "Do not use architecture specific optimizations (like '-march=armv8.1-a' for example) (has priority over PA_OPT_ARCH)" OFF)

# deprecated options
option(PA_WIN_USE_FLS       "Use Fiber local storage on Windows to detect thread termination (deprecated)" OFF)
option(PA_CHECK_FULL        "Use full internal invariant checking in DEBUG mode (deprecated, use PA_DEBUG_FULL instead)" OFF)
option(PA_USE_LIBATOMIC     "Explicitly link with -latomic (on older systems) (deprecated and detected automatically)" OFF)

# Dual-mode build: USER (mmap/heap) or KERNEL (physical memory for PoOS). When set, core + one backend only.
set(PALLOC_BUILD_MODE "" CACHE STRING "palloc build mode: USER or KERNEL (empty = legacy prim-based build)")
option(PALLOC_KERNEL_RUN_ON_HOST "KERNEL mode: use mmap backend so tests/bench run on host (no 0x40000000)" OFF)

include(CheckLinkerFlag)    # requires cmake 3.18
include(CheckIncludeFiles)
include(GNUInstallDirs)
include("cmake/palloc-config-version.cmake")

set(pa_sources
    src/alloc.c
    src/alloc-aligned.c
    src/alloc-posix.c
    src/arena.c
    src/arena_pomai.c
    src/bitmap.c
    src/heap.c
    src/init.c
    src/libc.c
    src/options.c
    src/os.c
    src/page.c
    src/random.c
    src/segment.c
    src/segment-map.c
    src/stats.c
    src/prim/prim.c)

if(PA_VECTOR)
  list(APPEND pa_sources src/palloc_vector.c)
endif()

set(pa_cflags "")
set(pa_cflags_static "")            # extra flags for a static library build
set(pa_cflags_dynamic "")           # extra flags for a shared-object library build
set(pa_libraries "")

if(PA_EXTRA_CPPDEFS)
 set(pa_defines ${PA_EXTRA_CPPDEFS})
else()
 set(pa_defines "")
endif()

# pass git revision as a define
if(EXISTS "${CMAKE_SOURCE_DIR}/.git/index")
  find_package(Git)
  if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} "describe" OUTPUT_VARIABLE pa_git_describe RESULT_VARIABLE pa_git_res ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(pa_git_res EQUAL "0")
      list(APPEND pa_defines "PA_GIT_DESCRIBE=${pa_git_describe}")
      # add to dependencies so we rebuild if the git head commit changes
      set_property(GLOBAL APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/.git/index")
    endif()
  endif()
endif()

# -----------------------------------------------------------------------------
# Convenience: set default build type and compiler depending on the build directory
# -----------------------------------------------------------------------------

message(STATUS "")
if (NOT CMAKE_BUILD_TYPE)
  if ("${CMAKE_BINARY_DIR}" MATCHES ".*((D|d)ebug|asan|tsan|ubsan|valgrind)$")
    message(STATUS "No build type selected, default to 'Debug'")
    set(CMAKE_BUILD_TYPE "Debug")
  else()
    message(STATUS "No build type selected, default to 'Release'")
    set(CMAKE_BUILD_TYPE "Release")
  endif()
endif()

if (CMAKE_GENERATOR MATCHES "^Visual Studio.*$")
  message(STATUS "Note: when building with Visual Studio the build type is specified when building.")
  message(STATUS "For example: 'cmake --build . --config=Release")
endif()

if("${CMAKE_BINARY_DIR}" MATCHES ".*(S|s)ecure$")
  message(STATUS "Default to secure build")
  set(PA_SECURE "ON")
endif()


# Determine architecture
set(PA_OPT_ARCH_FLAGS "")
set(PA_ARCH "unknown")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86|i[3456]86)$" OR CMAKE_GENERATOR_PLATFORM MATCHES "^(x86|Win32)$")
  set(PA_ARCH "x86")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|x64|amd64|AMD64)$" OR CMAKE_GENERATOR_PLATFORM STREQUAL "x64" OR "x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES) # must be before arm64
  set(PA_ARCH "x64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|armv[89].?|ARM64)$" OR CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64" OR "arm64" IN_LIST CMAKE_OSX_ARCHITECTURES)
  set(PA_ARCH "arm64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|armv[34567].?|ARM)$")
  set(PA_ARCH "arm32")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(riscv|riscv32|riscv64)$")
  if(CMAKE_SIZEOF_VOID_P==4)
    set(PA_ARCH "riscv32")
  else()
    set(PA_ARCH "riscv64")
  endif()
else()
  set(PA_ARCH ${CMAKE_SYSTEM_PROCESSOR})
endif()
message(STATUS "Architecture: ${PA_ARCH}") # (${CMAKE_SYSTEM_PROCESSOR}, ${CMAKE_GENERATOR_PLATFORM}, ${CMAKE_GENERATOR})")

# negative overrides (mainly to support vcpkg features)
if(PA_NO_USE_CXX)
  set(PA_USE_CXX "OFF")
endif()
if(PA_NO_OPT_ARCH)
  set(PA_OPT_ARCH "OFF")
elseif(PA_ARCH STREQUAL "arm64")
  set(PA_OPT_ARCH "ON")  # enable armv8.1-a by default on arm64 unless PA_NO_OPT_ARCH is set
endif()


# -----------------------------------------------------------------------------
# Process options
# -----------------------------------------------------------------------------

if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  set(PA_CLANG_CL "ON")
endif()

# put -Wall early so other warnings can be disabled selectively
if(CMAKE_C_COMPILER_ID MATCHES "AppleClang|Clang")
  if (PA_CLANG_CL)
    list(APPEND pa_cflags -W)
  else()
    list(APPEND pa_cflags -Wall -Wextra -Wpedantic)
  endif()
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU")
    list(APPEND pa_cflags -Wall -Wextra)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "Intel")
    list(APPEND pa_cflags -Wall)
endif()

# force C++ compilation with msvc or clang-cl to use modern C++ atomics
if(CMAKE_C_COMPILER_ID MATCHES "MSVC|Intel" OR PA_CLANG_CL)
  set(PA_USE_CXX "ON")
endif()

if(PA_OVERRIDE)
  message(STATUS "Override standard malloc (PA_OVERRIDE=ON)")
  if(APPLE)
    if(PA_OSX_ZONE)
      # use zone's on macOS
      message(STATUS "  Use malloc zone to override malloc (PA_OSX_ZONE=ON)")
      list(APPEND pa_sources src/prim/osx/alloc-override-zone.c)
      list(APPEND pa_defines PA_OSX_ZONE=1)
      if (NOT PA_OSX_INTERPOSE)
        message(STATUS "  WARNING: zone overriding usually also needs interpose (use -DPA_OSX_INTERPOSE=ON)")
      endif()
    endif()
    if(PA_OSX_INTERPOSE)
      # use interpose on macOS
      message(STATUS "  Use interpose to override malloc (PA_OSX_INTERPOSE=ON)")
      list(APPEND pa_defines PA_OSX_INTERPOSE=1)
      if (NOT PA_OSX_ZONE)
        message(STATUS "  WARNING: interpose usually also needs zone overriding (use -DPA_OSX_ZONE=ON)")
      endif()
    endif()
    if(PA_USE_CXX AND PA_OSX_INTERPOSE)
      message(STATUS "  WARNING: if dynamically overriding malloc/free, it is more reliable to build palloc as C code (use -DPA_USE_CXX=OFF)")
    endif()
  endif()
endif()

if(WIN32)
  if (NOT PA_WIN_REDIRECT)
    # use a negative define for backward compatibility
    list(APPEND pa_defines PA_WIN_NOREDIRECT=1)
  endif()
endif()

if(PA_SECURE)
  message(STATUS "Set full secure build (PA_SECURE=ON)")
  list(APPEND pa_defines PA_SECURE=4)
endif()

if(PA_TRACK_VALGRIND)
  CHECK_INCLUDE_FILES("valgrind/valgrind.h;valgrind/memcheck.h" PA_HAS_VALGRINDH)
  if (NOT PA_HAS_VALGRINDH)
    set(PA_TRACK_VALGRIND OFF)
    message(WARNING "Cannot find the 'valgrind/valgrind.h' and 'valgrind/memcheck.h' -- install valgrind first?")
    message(STATUS  "Disabling Valgrind support (PA_TRACK_VALGRIND=OFF)")
  else()
    message(STATUS "Compile with Valgrind support (PA_TRACK_VALGRIND=ON)")
    list(APPEND pa_defines PA_TRACK_VALGRIND=1)
  endif()
endif()

if(PA_TRACK_ASAN)
  if (APPLE AND PA_OVERRIDE)
    set(PA_TRACK_ASAN OFF)
    message(WARNING "Cannot enable address sanitizer support on macOS if PA_OVERRIDE is ON (PA_TRACK_ASAN=OFF)")
  endif()
  if (PA_TRACK_VALGRIND)
    set(PA_TRACK_ASAN OFF)
    message(WARNING "Cannot enable address sanitizer support with also Valgrind support enabled (PA_TRACK_ASAN=OFF)")
  endif()
  if(PA_TRACK_ASAN)
    CHECK_INCLUDE_FILES("sanitizer/asan_interface.h" PA_HAS_ASANH)
    if (NOT PA_HAS_ASANH)
      set(PA_TRACK_ASAN OFF)
      message(WARNING "Cannot find the 'sanitizer/asan_interface.h' -- install address sanitizer support first")
      message(STATUS  "Compile **without** address sanitizer support (PA_TRACK_ASAN=OFF)")
    else()
      message(STATUS "Compile with address sanitizer support (PA_TRACK_ASAN=ON)")
      list(APPEND pa_defines PA_TRACK_ASAN=1)
      list(APPEND pa_cflags -fsanitize=address)
      list(APPEND pa_libraries -fsanitize=address)
    endif()
  endif()
endif()

if(PA_TRACK_ETW)
  if(NOT WIN32)
    set(PA_TRACK_ETW OFF)
    message(WARNING "Can only enable ETW support on Windows (PA_TRACK_ETW=OFF)")
  endif()
  if (PA_TRACK_VALGRIND OR PA_TRACK_ASAN)
    set(PA_TRACK_ETW OFF)
    message(WARNING "Cannot enable ETW support with also Valgrind or ASAN support enabled (PA_TRACK_ETW=OFF)")
  endif()
  if(PA_TRACK_ETW)
    message(STATUS "Compile with Windows event tracing support (PA_TRACK_ETW=ON)")
    list(APPEND pa_defines PA_TRACK_ETW=1)
  endif()
endif()

if(PA_GUARDED)
  message(STATUS "Compile guard pages behind certain object allocations (PA_GUARDED=ON)")
  list(APPEND pa_defines PA_GUARDED=1)
  if(NOT PA_NO_PADDING)
    message(STATUS "  Disabling padding due to guard pages (PA_NO_PADDING=ON)")
    set(PA_NO_PADDING ON)
  endif()
endif()

if(PA_VECTOR)
  message(STATUS "Build vector extension (PA_VECTOR=ON)")
  list(APPEND pa_defines PA_VECTOR=1)
endif()

if(PA_SEE_ASM)
  message(STATUS "Generate assembly listings (PA_SEE_ASM=ON)")
  list(APPEND pa_cflags -save-temps)
  if(CMAKE_C_COMPILER_ID MATCHES "AppleClang|Clang")
    message(STATUS "No GNU Line marker")
    list(APPEND pa_cflags -Wno-gnu-line-marker)
  endif()
endif()

if(PA_CHECK_FULL)
  message(STATUS "The PA_CHECK_FULL option is deprecated, use PA_DEBUG_FULL instead")
  set(PA_DEBUG_FULL "ON")
endif()

if (PA_SKIP_COLLECT_ON_EXIT)
  message(STATUS "Skip collecting memory on program exit (PA_SKIP_COLLECT_ON_EXIT=ON)")
  list(APPEND pa_defines PA_SKIP_COLLECT_ON_EXIT=1)
endif()

if(PA_DEBUG_FULL)
  message(STATUS "Set debug level to full assertion and internal invariant checking (PA_DEBUG_FULL=ON, expensive)")
  list(APPEND pa_defines PA_DEBUG=3)   # full invariant checking (pa_assert, pa_assert_internal, and pa_assert_expensive)
elseif(PA_DEBUG_INTERNAL)
  message(STATUS "Set debug level to internal assertion and invariant checking (PA_DEBUG_INTERNAL=ON)")
  list(APPEND pa_defines PA_DEBUG=2)   # invariant checking (pa_assert and pa_assert_internal)
elseif(PA_DEBUG)
  message(STATUS "Set debug level to assertion checking (PA_DEBUG=ON)")
  list(APPEND pa_defines PA_DEBUG=1)   # assertion checking (pa_assert)
elseif(CMAKE_BUILD_TYPE MATCHES "Debug")
  message(STATUS "Set debug level to internal assertion and invariant checking (CMAKE_BUILD_TYPE=Debug)")
  set(PA_DEBUG_INTERNAL ON)
  list(APPEND pa_defines PA_DEBUG=2)   # invariant checking (pa_assert and pa_assert_internal)
endif()

if(PA_NO_PADDING)
  message(STATUS "Suppress any padding of heap blocks (PA_NO_PADDING=ON)")
  list(APPEND pa_defines PA_PADDING=0)
else()
  if(PA_PADDING)
    message(STATUS "Enable explicit padding of heap blocks (PA_PADDING=ON)")
    list(APPEND pa_defines PA_PADDING=1)
  endif()
endif()

if(PA_XMALLOC)
  message(STATUS "Enable abort() calls on memory allocation failure (PA_XMALLOC=ON)")
  list(APPEND pa_defines PA_XMALLOC=1)
endif()

if(PA_SHOW_ERRORS)
  message(STATUS "Enable printing of error and warning messages by default (PA_SHOW_ERRORS=ON)")
  list(APPEND pa_defines PA_SHOW_ERRORS=1)
endif()

if(PA_DEBUG_TSAN)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(STATUS "Build with thread sanitizer (PA_DEBUG_TSAN=ON)")
    list(APPEND pa_defines PA_TSAN=1)
    list(APPEND pa_cflags -fsanitize=thread -g -O1)
    list(APPEND pa_libraries -fsanitize=thread)
  else()
    message(WARNING "Can only use thread sanitizer with clang (PA_DEBUG_TSAN=ON but ignored)")
  endif()
endif()

if(PA_DEBUG_UBSAN)
  if(CMAKE_BUILD_TYPE MATCHES "Debug")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      message(STATUS "Build with undefined-behavior sanitizer (PA_DEBUG_UBSAN=ON)")
      list(APPEND pa_defines PA_UBSAN=1)
      list(APPEND pa_cflags -fsanitize=undefined -g -fno-sanitize-recover=undefined)
      list(APPEND pa_libraries -fsanitize=undefined)
      if (NOT PA_USE_CXX)
        message(STATUS "(switch to use C++ due to PA_DEBUG_UBSAN)")
        set(PA_USE_CXX "ON")
      endif()
    else()
      message(WARNING "Can only use undefined-behavior sanitizer with clang++ (PA_DEBUG_UBSAN=ON but ignored)")
    endif()
  else()
    message(WARNING "Can only use undefined-behavior sanitizer with a debug build (CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE})")
  endif()
endif()

if(PA_USE_CXX)
  message(STATUS "Use the C++ compiler to compile (PA_USE_CXX=ON)")
  set_source_files_properties(${pa_sources} PROPERTIES LANGUAGE CXX )
  set_source_files_properties(src/static.c test/test-api.c test/test-api-fill test/test-stress PROPERTIES LANGUAGE CXX )
  if(CMAKE_CXX_COMPILER_ID MATCHES "AppleClang|Clang")
    list(APPEND pa_cflags -Wno-deprecated)
  endif()
  if(CMAKE_CXX_COMPILER_ID MATCHES "Intel" AND NOT CMAKE_CXX_COMPILER_ID MATCHES "IntelLLVM")
    list(APPEND pa_cflags -Kc++)
  endif()
endif()

if(CMAKE_SYSTEM_NAME MATCHES "Linux|Android")
  if(PA_NO_THP)
    message(STATUS "Disable transparent huge pages support (PA_NO_THP=ON)")
    list(APPEND pa_defines PA_NO_THP=1)
  endif()
endif()

if(PA_LIBC_MUSL)
  message(STATUS "Assume using musl libc (PA_LIBC_MUSL=ON)")
  list(APPEND pa_defines PA_LIBC_MUSL=1)
endif()

if(PA_WIN_USE_FLS)
  message(STATUS "Use the Fiber API to detect thread termination (deprecated) (PA_WIN_USE_FLS=ON)")
  list(APPEND pa_defines PA_WIN_USE_FLS=1)
endif()

if(PA_WIN_USE_FIXED_TLS)
  message(STATUS "Use fixed TLS slot on Windows to avoid extra tests in the malloc fast path (PA_WIN_USE_FIXED_TLS=ON)")
  list(APPEND pa_defines PA_WIN_USE_FIXED_TLS=1)
endif()

# Check /proc/cpuinfo for an SV39 MMU and limit the virtual address bits.
# (this will skip the aligned hinting in that case. Issue #939, #949)
# Only on Linux: /proc/cpuinfo does not exist on Windows/macOS.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND EXISTS "/proc/cpuinfo")
  file(STRINGS /proc/cpuinfo pa_sv39_mmu REGEX "^mmu[ \t]+:[ \t]+sv39$")
  if (pa_sv39_mmu)
    MESSAGE( STATUS "Set virtual address bits to 39 (SV39 MMU detected)" )
    list(APPEND pa_defines PA_DEFAULT_VIRTUAL_ADDRESS_BITS=39)
  endif()
endif()

# On Haiku use `-DCMAKE_INSTALL_PREFIX` instead, issue #788
# if(CMAKE_SYSTEM_NAME MATCHES "Haiku")
#   SET(CMAKE_INSTALL_LIBDIR ~/config/non-packaged/lib)
#   SET(CMAKE_INSTALL_INCLUDEDIR ~/config/non-packaged/headers)
# endif()

# Compiler flags
if(CMAKE_C_COMPILER_ID MATCHES "AppleClang|Clang|GNU" AND NOT PA_CLANG_CL)
  list(APPEND pa_cflags -Wno-unknown-pragmas -fvisibility=hidden)
  if(NOT PA_USE_CXX)
    list(APPEND pa_cflags -Wstrict-prototypes)
  endif()
  if(CMAKE_C_COMPILER_ID MATCHES "AppleClang|Clang")
    list(APPEND pa_cflags -Wno-static-in-inline)
  endif()
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Intel")
  list(APPEND pa_cflags -fvisibility=hidden)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "AppleClang|Clang|GNU|Intel" AND NOT CMAKE_SYSTEM_NAME MATCHES "Haiku" AND NOT PA_CLANG_CL)
  if(PA_LOCAL_DYNAMIC_TLS)
    list(APPEND pa_cflags -ftls-model=local-dynamic)
  else()
    if(PA_LIBC_MUSL)
      # with musl we use local-dynamic for the static build, see issue #644
      list(APPEND pa_cflags_static  -ftls-model=local-dynamic)
      list(APPEND pa_cflags_dynamic -ftls-model=initial-exec)
      message(STATUS "Use local dynamic TLS for the static build (since PA_LIBC_MUSL=ON)")
    else()
      list(APPEND pa_cflags -ftls-model=initial-exec)
    endif()
  endif()
  if(PA_OVERRIDE)
    list(APPEND pa_cflags -fno-builtin-malloc)
  endif()
endif()

if(CMAKE_C_COMPILER_ID MATCHES "AppleClang|Clang|GNU|Intel" AND NOT CMAKE_SYSTEM_NAME MATCHES "Haiku")
  if(PA_OPT_ARCH)
    if(APPLE AND CMAKE_C_COMPILER_ID MATCHES "AppleClang|Clang" AND CMAKE_OSX_ARCHITECTURES)   # to support multi-arch binaries (#999)
      if("arm64" IN_LIST CMAKE_OSX_ARCHITECTURES)
        list(APPEND PA_OPT_ARCH_FLAGS "-Xarch_arm64;-march=armv8.1-a")
      endif()
    elseif(PA_ARCH STREQUAL "arm64")
      set(PA_OPT_ARCH_FLAGS "-march=armv8.1-a")  # fast atomics
    elseif(PA_ARCH STREQUAL "x64")
      set(PA_OPT_ARCH_FLAGS "-march=native")  # max throughput: AVX2, FSRM, etc.
    endif()
  endif()
endif()

if (MSVC AND MSVC_VERSION GREATER_EQUAL 1914)
  list(APPEND pa_cflags /Zc:__cplusplus)
  if(PA_OPT_ARCH AND NOT PA_CLANG_CL)
    if(PA_ARCH STREQUAL "arm64")
      set(PA_OPT_ARCH_FLAGS "/arch:armv8.1")           # fast atomics
    endif()
  endif()
endif()

if(MINGW)
  add_definitions(-D_WIN32_WINNT=0x600)                # issue #976
endif()

if(PA_OPT_ARCH_FLAGS)
  list(APPEND pa_cflags ${PA_OPT_ARCH_FLAGS})
  message(STATUS "Architecture specific optimization is enabled (with ${PA_OPT_ARCH_FLAGS}) (PA_OPT_ARCH=ON)")
endif()

# extra needed libraries

# we prefer -l<lib> test over `find_library` as sometimes core libraries
# like `libatomic` are not on the system path (see issue #898)
function(find_link_library libname outlibname)
  check_linker_flag(C "-l${libname}" pa_has_lib${libname})
  if (pa_has_lib${libname})
    message(VERBOSE "link library: -l${libname}")
    set(${outlibname} ${libname} PARENT_SCOPE)
  else()
    find_library(PA_LIBPATH_${libname} ${libname})
    if (PA_LIBPATH_${libname})
      message(VERBOSE "link library ${libname} at ${PA_LIBPATH_${libname}}")
      set(${outlibname} ${PA_LIBPATH_${libname}} PARENT_SCOPE)
    else()
      message(VERBOSE "link library not found: ${libname}")
      set(${outlibname} "" PARENT_SCOPE)
    endif()
  endif()
endfunction()

if(WIN32)
  list(APPEND pa_libraries psapi shell32 user32 advapi32 bcrypt)
else()
  find_link_library("pthread" PA_LIB_PTHREAD)
  if(PA_LIB_PTHREAD)
    list(APPEND pa_libraries "${PA_LIB_PTHREAD}")
  endif()
  find_link_library("rt" PA_LIB_RT)
  if(PA_LIB_RT)
    list(APPEND pa_libraries "${PA_LIB_RT}")
  endif()
  find_link_library("atomic" PA_LIB_ATOMIC)
  if(PA_LIB_ATOMIC)
    list(APPEND pa_libraries "${PA_LIB_ATOMIC}")
  endif()
endif()


# -----------------------------------------------------------------------------
# Install and output names
# -----------------------------------------------------------------------------

# dynamic/shared library and symlinks always go to /usr/local/lib equivalent
# we use ${CMAKE_INSTALL_BINDIR} and ${CMAKE_INSTALL_LIBDIR}.

# static libraries and object files, includes, and cmake config files
# are either installed at top level, or use versioned directories for side-by-side installation (default)
if (PA_INSTALL_TOPLEVEL)
  set(pa_install_objdir     "${CMAKE_INSTALL_LIBDIR}")
  set(pa_install_incdir     "${CMAKE_INSTALL_INCLUDEDIR}")
  set(pa_install_cmakedir   "${CMAKE_INSTALL_LIBDIR}/cmake/palloc")
else()
  set(pa_install_objdir     "${CMAKE_INSTALL_LIBDIR}/palloc-${pa_version}")       # for static library and object files
  set(pa_install_incdir     "${CMAKE_INSTALL_INCLUDEDIR}/palloc-${pa_version}")   # for includes
  set(pa_install_cmakedir   "${CMAKE_INSTALL_LIBDIR}/cmake/palloc-${pa_version}") # for cmake package info
endif()

set(pa_libname "palloc")
if(PA_SECURE)
  set(pa_libname "${pa_libname}-secure")
endif()
if(PA_TRACK_VALGRIND)
  set(pa_libname "${pa_libname}-valgrind")
endif()
if(PA_TRACK_ASAN)
  set(pa_libname "${pa_libname}-asan")
endif()
string(TOLOWER "${CMAKE_BUILD_TYPE}" CMAKE_BUILD_TYPE_LC)
list(APPEND pa_defines "PA_CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE_LC}")  #todo: multi-config project needs $<CONFIG> ?
if(CMAKE_BUILD_TYPE_LC MATCHES "^(release|relwithdebinfo|minsizerel|none)$")
  list(APPEND pa_defines PA_BUILD_RELEASE)
else()
  set(pa_libname "${pa_libname}-${CMAKE_BUILD_TYPE_LC}") #append build type (e.g. -debug) if not a release version
endif()

if(PA_BUILD_SHARED)
  list(APPEND pa_build_targets "shared")
endif()
if(PA_BUILD_STATIC)
  list(APPEND pa_build_targets "static")
endif()
if(PA_BUILD_OBJECT)
  list(APPEND pa_build_targets "object")
endif()
if(PA_BUILD_TESTS)
  list(APPEND pa_build_targets "tests")
endif()

message(STATUS "")
message(STATUS "Library name     : ${pa_libname}")
message(STATUS "Version          : ${pa_version}.${pa_version_patch}")
message(STATUS "Build type       : ${CMAKE_BUILD_TYPE_LC}")
if(PA_USE_CXX)
  message(STATUS "C++ Compiler     : ${CMAKE_CXX_COMPILER}")
else()
  message(STATUS "C Compiler       : ${CMAKE_C_COMPILER}")
endif()
message(STATUS "Compiler flags   : ${pa_cflags}")
message(STATUS "Compiler defines : ${pa_defines}")
message(STATUS "Link libraries   : ${pa_libraries}")
message(STATUS "Build targets    : ${pa_build_targets}")
message(STATUS "")

# -----------------------------------------------------------------------------
# Main targets
# -----------------------------------------------------------------------------

# Dual-mode: core + one backend (USER or KERNEL)
if(PALLOC_BUILD_MODE STREQUAL "USER" OR PALLOC_BUILD_MODE STREQUAL "KERNEL")
  set(pa_core_sources
    src/core/alloc_core.c
    src/core/vector_core.c
  )
  if(PALLOC_BUILD_MODE STREQUAL "USER")
    set(pa_backend_source src/backends/backend_user.c)
    set(pa_mode_defines PALLOC_USER_MODE=1)
    set(pa_mode_freestanding OFF)
  else()
    if(PALLOC_KERNEL_RUN_ON_HOST)
      set(pa_backend_source src/backends/backend_kernel_host.c)
      set(pa_mode_defines PALLOC_KERNEL_MODE=1)
      set(pa_mode_freestanding OFF)
    else()
      set(pa_backend_source src/backends/backend_kernel.c)
      set(pa_mode_defines PALLOC_KERNEL_MODE=1)
      set(pa_mode_freestanding ON)
    endif()
  endif()

  add_library(palloc STATIC ${pa_core_sources} ${pa_backend_source})
  set_target_properties(palloc PROPERTIES OUTPUT_NAME "palloc")
  target_compile_definitions(palloc PRIVATE ${pa_mode_defines})
  target_include_directories(palloc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${pa_install_incdir}>
  )
  if(pa_mode_freestanding)
    target_compile_options(palloc PRIVATE -ffreestanding -nostdlib)
  else()
    target_compile_options(palloc PRIVATE ${pa_cflags})
    target_link_libraries(palloc PUBLIC ${pa_libraries})
  endif()
  message(STATUS "palloc dual-mode: ${PALLOC_BUILD_MODE} (core + ${pa_backend_source})")

  install(TARGETS palloc EXPORT palloc DESTINATION ${pa_install_objdir} LIBRARY)
  install(FILES include/palloc_backend_api.h DESTINATION ${pa_install_incdir})
  install(FILES include/palloc/core_types.h include/palloc/core_api.h DESTINATION ${pa_install_incdir}/palloc)

  # Dual-mode tests: USER (runnable on host) and KERNEL (build/link only)
  if(PA_BUILD_TESTS)
    enable_testing()
    if(PALLOC_BUILD_MODE STREQUAL "USER")
      add_executable(palloc-test-user-core test/test-user-core.c)
      target_link_libraries(palloc-test-user-core PRIVATE palloc)
      target_include_directories(palloc-test-user-core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
      add_test(NAME test-user-core COMMAND palloc-test-user-core)
    elseif(PALLOC_BUILD_MODE STREQUAL "KERNEL")
      if(PALLOC_KERNEL_RUN_ON_HOST)
        add_executable(palloc-test-kernel-core test/test-kernel-core.c)
        target_link_libraries(palloc-test-kernel-core PRIVATE palloc ${pa_libraries})
        target_include_directories(palloc-test-kernel-core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        add_test(NAME test-kernel-core COMMAND palloc-test-kernel-core)
      else()
        add_executable(palloc-test-kernel-core test/test-kernel-core.c test/crt0_kernel.c)
        target_link_libraries(palloc-test-kernel-core PRIVATE palloc)
        target_include_directories(palloc-test-kernel-core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_compile_options(palloc-test-kernel-core PRIVATE -ffreestanding -nostdlib)
        target_link_options(palloc-test-kernel-core PRIVATE -nostdlib -Wl,-e,_start)
        add_test(NAME test-kernel-core-built COMMAND ${CMAKE_COMMAND} -E echo "Kernel test built (run under PoOS/QEMU)")
      endif()
    endif()
  endif()

  # Dual-mode benchmarks: core API only (USER = timing on host, KERNEL = build/run under PoOS or QEMU)
  if(PALLOC_BUILD_MODE STREQUAL "USER")
    add_executable(bench-user-core bench/bench_core.c)
    target_compile_definitions(bench-user-core PRIVATE PALLOC_USER_MODE=1)
    target_link_libraries(bench-user-core PRIVATE palloc)
    target_include_directories(bench-user-core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(UNIX AND NOT APPLE)
      target_link_libraries(bench-user-core PRIVATE rt)
    endif()
    message(STATUS "Benchmark (USER): bench-user-core")
  elseif(PALLOC_BUILD_MODE STREQUAL "KERNEL")
    add_executable(bench-kernel-core bench/bench_core.c)
    target_compile_definitions(bench-kernel-core PRIVATE PALLOC_KERNEL_MODE=1)
    if(PALLOC_KERNEL_RUN_ON_HOST)
      target_compile_definitions(bench-kernel-core PRIVATE PALLOC_KERNEL_RUN_ON_HOST=1)
    endif()
    target_link_libraries(bench-kernel-core PRIVATE palloc)
    target_include_directories(bench-kernel-core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(PALLOC_KERNEL_RUN_ON_HOST)
      if(UNIX AND NOT APPLE)
        target_link_libraries(bench-kernel-core PRIVATE rt)
      endif()
      message(STATUS "Benchmark (KERNEL): bench-kernel-core (host-safe, run on host)")
    else()
      target_sources(bench-kernel-core PRIVATE test/crt0_kernel.c)
      target_compile_options(bench-kernel-core PRIVATE -ffreestanding -nostdlib)
      target_link_options(bench-kernel-core PRIVATE -nostdlib -Wl,-e,_start)
      message(STATUS "Benchmark (KERNEL): bench-kernel-core (run under PoOS/QEMU)")
    endif()
  endif()
endif()

# Legacy build: prim-based (when PALLOC_BUILD_MODE is not set)
if(NOT PALLOC_BUILD_MODE)

# shared library
if(PA_BUILD_SHARED)
  add_library(palloc SHARED ${pa_sources})
  set_target_properties(palloc PROPERTIES VERSION ${pa_version} SOVERSION ${pa_version_major} OUTPUT_NAME ${pa_libname} )
  target_compile_definitions(palloc PRIVATE ${pa_defines} PA_SHARED_LIB PA_SHARED_LIB_EXPORT)
  target_compile_options(palloc PRIVATE ${pa_cflags} ${pa_cflags_dynamic})
  target_link_libraries(palloc PRIVATE ${pa_libraries})
  target_include_directories(palloc PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${pa_install_incdir}>
  )
  install(TARGETS palloc EXPORT palloc ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(EXPORT palloc DESTINATION ${pa_install_cmakedir})

  if(WIN32 AND NOT MINGW)
    # On windows, the import library name for the dll would clash with the static palloc.lib library
    # so we postfix the dll import library with `.dll.lib` (and also the .pdb debug file)
    set_property(TARGET palloc PROPERTY ARCHIVE_OUTPUT_NAME "${pa_libname}.dll" )
    install(FILES "$<TARGET_FILE_DIR:palloc>/${pa_libname}.dll.lib" DESTINATION ${CMAKE_INSTALL_LIBDIR})
    set_property(TARGET palloc PROPERTY PDB_NAME "${pa_libname}.dll")
    # don't try to install the pdb since it may not be generated depending on the configuration
    # install(FILES "$<TARGET_FILE_DIR:palloc>/${pa_libname}.dll.pdb" DESTINATION ${CMAKE_INSTALL_LIBDIR})
  endif()
  if(WIN32 AND PA_WIN_REDIRECT)
    if(MINGW)
      set_property(TARGET palloc PROPERTY PREFIX "")
    endif()
    # On windows, link and copy the palloc redirection dll too.
    if(CMAKE_GENERATOR_PLATFORM STREQUAL "arm64ec")
      set(PALLOC_REDIRECT_SUFFIX "-arm64ec")
    elseif(PA_ARCH STREQUAL "x64")
      set(PALLOC_REDIRECT_SUFFIX "")
      if(CMAKE_SYSTEM_PROCESSOR STREQUAL "ARM64")
        message(STATUS "Note: x64 code emulated on Windows for arm64 should use an arm64ec build of 'palloc.dll'")
        message(STATUS "      together with 'palloc-redirect-arm64ec.dll'. See the 'bin\\readme.md' for more information.")
      endif()
    elseif(PA_ARCH STREQUAL "x86")
      set(PALLOC_REDIRECT_SUFFIX "32")
    else()
      set(PALLOC_REDIRECT_SUFFIX "-${PA_ARCH}")  # -arm64 etc.
    endif()

    # Build redirect DLL from bin/ (or use prebuilt .lib/.dll in bin/ if present)
    set(PA_REDIRECT_PREBUILT_LIB "${CMAKE_CURRENT_SOURCE_DIR}/bin/palloc-redirect${PALLOC_REDIRECT_SUFFIX}.lib")
    set(PA_REDIRECT_PREBUILT_DLL "${CMAKE_CURRENT_SOURCE_DIR}/bin/palloc-redirect${PALLOC_REDIRECT_SUFFIX}.dll")
    if(EXISTS "${PA_REDIRECT_PREBUILT_LIB}" AND EXISTS "${PA_REDIRECT_PREBUILT_DLL}")
      target_link_libraries(palloc PRIVATE "${PA_REDIRECT_PREBUILT_LIB}")
      add_custom_command(TARGET palloc POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy "${PA_REDIRECT_PREBUILT_DLL}" $<TARGET_FILE_DIR:palloc>
        COMMENT "Copy palloc-redirect${PALLOC_REDIRECT_SUFFIX}.dll to output directory")
      install(FILES "$<TARGET_FILE_DIR:palloc>/palloc-redirect${PALLOC_REDIRECT_SUFFIX}.dll" DESTINATION ${CMAKE_INSTALL_BINDIR})
    else()
      add_subdirectory(bin)
      target_link_libraries(palloc PRIVATE palloc-redirect)
      add_custom_command(TARGET palloc POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy $<TARGET_FILE:palloc-redirect> $<TARGET_FILE_DIR:palloc>
        COMMENT "Copy palloc-redirect DLL to output directory")
      install(FILES $<TARGET_FILE:palloc-redirect> DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
  endif()
endif()


# static library
if (PA_BUILD_STATIC)
  add_library(palloc-static STATIC ${pa_sources})
  set_property(TARGET palloc-static PROPERTY OUTPUT_NAME ${pa_libname})
  set_property(TARGET palloc-static PROPERTY POSITION_INDEPENDENT_CODE ON)
  target_compile_definitions(palloc-static PRIVATE ${pa_defines} PA_STATIC_LIB)
  target_compile_options(palloc-static PRIVATE ${pa_cflags} ${pa_cflags_static})
  target_link_libraries(palloc-static PRIVATE ${pa_libraries})
  target_include_directories(palloc-static PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${pa_install_incdir}>
  )
  install(TARGETS palloc-static EXPORT palloc DESTINATION ${pa_install_objdir} LIBRARY)
  install(EXPORT palloc DESTINATION ${pa_install_cmakedir})
endif()

# install include files
install(FILES include/palloc.h DESTINATION ${pa_install_incdir})
install(FILES include/palloc-override.h DESTINATION ${pa_install_incdir})
install(FILES include/palloc-new-delete.h DESTINATION ${pa_install_incdir})
install(FILES include/palloc-stats.h DESTINATION ${pa_install_incdir})
install(FILES include/palloc_cxx_arena.h DESTINATION ${pa_install_incdir})
install(FILES include/palloc/arena_pomai.h DESTINATION ${pa_install_incdir}/palloc)
if(PA_VECTOR)
  install(FILES include/palloc_vector.h DESTINATION ${pa_install_incdir})
endif()
install(FILES cmake/palloc-config.cmake DESTINATION ${pa_install_cmakedir})
install(FILES cmake/palloc-config-version.cmake DESTINATION ${pa_install_cmakedir})


# single object file for more predictable static overriding
if (PA_BUILD_OBJECT)
  add_library(palloc-obj OBJECT src/static.c)
  set_property(TARGET palloc-obj PROPERTY POSITION_INDEPENDENT_CODE ON)
  target_compile_definitions(palloc-obj PRIVATE ${pa_defines})
  target_compile_options(palloc-obj PRIVATE ${pa_cflags} ${pa_cflags_static})
  target_include_directories(palloc-obj PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${pa_install_incdir}>
  )

  # Copy the generated object file (`static.o`) to the output directory (as `palloc.o`)
  if(CMAKE_GENERATOR MATCHES "^Visual Studio.*$")
    set(palloc-obj-static "${CMAKE_CURRENT_BINARY_DIR}/palloc-obj.dir/$<CONFIG>/static${CMAKE_C_OUTPUT_EXTENSION}")
  else()
    set(palloc-obj-static "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/palloc-obj.dir/src/static.c${CMAKE_C_OUTPUT_EXTENSION}")
  endif()
  set(palloc-obj-out "${CMAKE_CURRENT_BINARY_DIR}/${pa_libname}${CMAKE_C_OUTPUT_EXTENSION}")
  add_custom_command(OUTPUT ${palloc-obj-out} DEPENDS palloc-obj COMMAND "${CMAKE_COMMAND}" -E copy "${palloc-obj-static}" "${palloc-obj-out}")
  add_custom_target(palloc-obj-target ALL DEPENDS ${palloc-obj-out})


  # the following seems to lead to cmake warnings/errors on some systems, disable for now :-(
  # install(TARGETS palloc-obj EXPORT palloc DESTINATION ${pa_install_objdir})

  # the FILES expression can also be: $<TARGET_OBJECTS:palloc-obj>
  # but that fails cmake versions less than 3.10 so we leave it as is for now
  install(FILES ${palloc-obj-static}
          DESTINATION ${pa_install_objdir}
          RENAME ${pa_libname}${CMAKE_C_OUTPUT_EXTENSION} )
endif()


# pkg-config file support
set(pa_pc_libraries "")
foreach(item IN LISTS pa_libraries)
  if(item MATCHES " *[-].*")
    set(pa_pc_libraries "${pa_pc_libraries} ${item}")
  else()
    set(pa_pc_libraries "${pa_pc_libraries} -l${item}")
  endif()
endforeach()

include("cmake/JoinPaths.cmake")
join_paths(pa_pc_includedir "\${prefix}" "${CMAKE_INSTALL_INCLUDEDIR}")
join_paths(pa_pc_libdir "\${prefix}" "${CMAKE_INSTALL_LIBDIR}")

configure_file(palloc.pc.in palloc.pc @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/palloc.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig/")



# -----------------------------------------------------------------------------
# API surface testing
# -----------------------------------------------------------------------------

if (PA_BUILD_TESTS)
  enable_testing()

  # static link tests
  foreach(TEST_NAME api api-fill stress arena-pomai vector basic)
    add_executable(palloc-test-${TEST_NAME} test/test-${TEST_NAME}.c)
    target_compile_definitions(palloc-test-${TEST_NAME} PRIVATE ${pa_defines})
    target_compile_options(palloc-test-${TEST_NAME} PRIVATE ${pa_cflags})
    target_include_directories(palloc-test-${TEST_NAME} PRIVATE include)
    if(PA_BUILD_STATIC AND NOT PA_DEBUG_TSAN)
      target_link_libraries(palloc-test-${TEST_NAME} PRIVATE palloc-static ${pa_libraries})
    elseif(PA_BUILD_SHARED)
      target_link_libraries(palloc-test-${TEST_NAME} PRIVATE palloc ${pa_libraries})
    else()
      message(STATUS "cannot build TSAN tests without PA_BUILD_SHARED being enabled")
    endif()
    add_test(NAME test-${TEST_NAME} COMMAND palloc-test-${TEST_NAME})
  endforeach()

  # dynamic override test
  if(PA_BUILD_SHARED AND NOT (PA_TRACK_ASAN OR PA_DEBUG_TSAN OR PA_DEBUG_UBSAN))
    add_executable(palloc-test-stress-dynamic test/test-stress.c)
    target_compile_definitions(palloc-test-stress-dynamic PRIVATE ${pa_defines} "USE_STD_MALLOC=1")
    target_compile_options(palloc-test-stress-dynamic PRIVATE ${pa_cflags})
    target_include_directories(palloc-test-stress-dynamic PRIVATE include)
    if(WIN32)
      target_compile_definitions(palloc-test-stress-dynamic PRIVATE "PA_LINK_VERSION=1")  # link pa_version
      target_link_libraries(palloc-test-stress-dynamic PRIVATE palloc ${pa_libraries})  # link pa_version
      add_test(NAME test-stress-dynamic COMMAND ${CMAKE_COMMAND} -E env PALLOC_VERBOSE=1 $<TARGET_FILE:palloc-test-stress-dynamic>)
    else()
      target_link_libraries(palloc-test-stress-dynamic PRIVATE ${pa_libraries}) # pthreads, issue 1158
      if(APPLE)
        set(LD_PRELOAD "DYLD_INSERT_LIBRARIES")
      else()
        set(LD_PRELOAD "LD_PRELOAD")
      endif()
      add_test(NAME test-stress-dynamic COMMAND ${CMAKE_COMMAND} -E env PALLOC_VERBOSE=1 ${LD_PRELOAD}=$<TARGET_FILE:palloc> $<TARGET_FILE:palloc-test-stress-dynamic>)
    endif()
  endif()
endif()

# -----------------------------------------------------------------------------
# Set override properties
# -----------------------------------------------------------------------------
if (PA_OVERRIDE)
  if (PA_BUILD_SHARED)
    target_compile_definitions(palloc PRIVATE PA_MALLOC_OVERRIDE)
  endif()
  if(NOT WIN32)
    # It is only possible to override malloc on Windows when building as a DLL.
    if (PA_BUILD_STATIC)
      target_compile_definitions(palloc-static PRIVATE PA_MALLOC_OVERRIDE)
    endif()
    if (PA_BUILD_OBJECT)
      target_compile_definitions(palloc-obj PRIVATE PA_MALLOC_OVERRIDE)
    endif()
  endif()
endif()

endif()  # NOT PALLOC_BUILD_MODE (legacy build)
