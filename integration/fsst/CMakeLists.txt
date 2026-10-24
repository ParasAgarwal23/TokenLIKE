# Optional FSST module for TokenLIKE.
#
# Nothing here is reachable unless the top-level build is configured with
# -DTOKENLIKE_WITH_FSST=ON. With the option off (the default) this file is never
# added, the repository has no FSST dependency, and every pre-existing target
# builds exactly as before.
#
# The upstream FSST snapshot is not vendored. It is located, in order:
#   1. -DTOKENLIKE_FSST_SOURCE_DIR=<path to a cwida/fsst checkout>
#   2. integration/fsst/third_party/fsst   (scripts/fetch_fsst.sh puts it there)
#   3. FetchContent from GitHub at the pinned commit below
# See THIRD_PARTY.md for the pin and the licence.

set(FSST_PINNED_COMMIT "e638d4cf8c26129d73c242a4127b42b975de5b63")
set(FSST_UPSTREAM_URL  "https://github.com/cwida/fsst.git")

set(FSST_DIR "")
if(TOKENLIKE_FSST_SOURCE_DIR AND EXISTS "${TOKENLIKE_FSST_SOURCE_DIR}/fsst.h")
  set(FSST_DIR "${TOKENLIKE_FSST_SOURCE_DIR}")
  message(STATUS "FSST module: using -DTOKENLIKE_FSST_SOURCE_DIR=${FSST_DIR}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fsst/fsst.h")
  set(FSST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fsst")
  message(STATUS "FSST module: using the local snapshot in integration/fsst/third_party/fsst")
else()
  # Clone the pinned revision ourselves, into the same place the fetch script
  # uses, and compile only the two translation units we need.
  #
  # Deliberately NOT FetchContent_MakeAvailable: that runs upstream's own
  # CMakeLists, which declares an `fsst12` target, and upstream's
  # libfsst12.cpp/fsst12.h DO NOT COMPILE with Apple clang on arm64 --
  # write_two_codes_aligned() and write_one_code_aligned() are declared taking
  # `unsigned long*` and called with `unsigned long long*`. That is an upstream
  # bug in a target this module never uses, but MakeAvailable would add it to
  # the default build target and break `cmake --build`. Verified: a bare
  # `clang++ -c libfsst12.cpp` fails the same way, while libfsst.cpp compiles
  # clean. See THIRD_PARTY.md.
  set(FSST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fsst")
  message(STATUS "FSST module: cloning ${FSST_UPSTREAM_URL} @ ${FSST_PINNED_COMMIT}")
  find_package(Git REQUIRED)
  if(NOT EXISTS "${FSST_DIR}/.git")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" clone --quiet "${FSST_UPSTREAM_URL}" "${FSST_DIR}"
      RESULT_VARIABLE git_rc ERROR_VARIABLE git_err)
    if(NOT git_rc EQUAL 0)
      message(FATAL_ERROR
        "FSST module: could not clone ${FSST_UPSTREAM_URL}\n${git_err}\n"
        "Run integration/fsst/scripts/fetch_fsst.sh when you have network, or "
        "pass -DTOKENLIKE_FSST_SOURCE_DIR=<path to a cwida/fsst checkout>.")
    endif()
  endif()
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -c advice.detachedHead=false -C "${FSST_DIR}"
            checkout --quiet "${FSST_PINNED_COMMIT}"
    RESULT_VARIABLE git_rc ERROR_VARIABLE git_err)
  if(NOT git_rc EQUAL 0)
    message(FATAL_ERROR "FSST module: could not check out ${FSST_PINNED_COMMIT}\n${git_err}")
  endif()
endif()
if(NOT EXISTS "${FSST_DIR}/fsst.h")
  message(FATAL_ERROR "FSST module: no usable FSST source at '${FSST_DIR}'. "
                      "Run integration/fsst/scripts/fetch_fsst.sh, or pass "
                      "-DTOKENLIKE_FSST_SOURCE_DIR=<path>.")
endif()

# Upstream sources, compiled unmodified. fsst_avx512.cpp has its own runtime
# capability check and becomes a stub on non-x86 hosts such as Apple Silicon.
add_library(fsst_upstream STATIC "${FSST_DIR}/libfsst.cpp" "${FSST_DIR}/fsst_avx512.cpp")
target_include_directories(fsst_upstream SYSTEM PUBLIC "${FSST_DIR}")
if(NOT MSVC)
  target_compile_options(fsst_upstream PRIVATE -O3)
endif()
set_target_properties(fsst_upstream PROPERTIES CXX_CLANG_TIDY "" CXX_CPPCHECK "")

add_library(fsstlike STATIC
  src/common.cpp
  src/pattern.cpp
  src/dfa.cpp
  src/fsstcol.cpp
  src/lift.cpp
  src/rawopt.cpp
  src/scan.cpp)
target_include_directories(fsstlike PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fsstlike PUBLIC fsst_upstream)
if(MSVC)
  target_compile_options(fsstlike PRIVATE /W4)
else()
  target_compile_options(fsstlike PRIVATE -Wall -Wextra)
endif()

foreach(tool fsst_like:cli fsst_encode:encode fsst_liftbench:liftbench)
  string(REPLACE ":" ";" parts ${tool})
  list(GET parts 0 exe)
  list(GET parts 1 file)
  add_executable(${exe} src/${file}.cpp)
  target_link_libraries(${exe} PRIVATE fsstlike)
  if(MSVC)
    target_compile_options(${exe} PRIVATE /W4)
  else()
    target_compile_options(${exe} PRIVATE -Wall -Wextra)
  endif()
endforeach()

add_executable(fsst_test_unit tests/test_unit.cpp)
target_link_libraries(fsst_test_unit PRIVATE fsstlike)
if(NOT MSVC)
  target_compile_options(fsst_test_unit PRIVATE -Wall -Wextra)
endif()
add_test(NAME fsst_unit COMMAND fsst_test_unit)
# The unit tests write a few deliberately corrupt columns under build/badcol.
set_tests_properties(fsst_unit PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

message(STATUS "FSST module: enabled (fsst_like, fsst_encode, fsst_liftbench, fsst_test_unit)")
