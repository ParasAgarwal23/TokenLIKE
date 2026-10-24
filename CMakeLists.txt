cmake_minimum_required(VERSION 3.16)
project(TokenLIKE LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 8)
  message(FATAL_ERROR
    "GCC ${CMAKE_CXX_COMPILER_VERSION} lacks full C++17 (<string_view>). "
    "Use a newer compiler, e.g. -DCMAKE_CXX_COMPILER=C:/mingw-w64/mingw64/bin/g++.exe")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# --- Sources by owner. A and B parts are picked up when they exist, so the
# --- repository builds at every stage of the hackathon.
set(C_SOURCES ${SRC}/like_bytes.cpp ${SRC}/baselines.cpp)

set(B_SOURCES ${SRC}/vocab.cpp ${SRC}/column.cpp)
set(HAVE_LOADERS ON)
foreach(f ${B_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/include/tokenlike/vocab.h
          ${CMAKE_CURRENT_SOURCE_DIR}/include/tokenlike/column.h)
  if(NOT EXISTS ${f})
    set(HAVE_LOADERS OFF)
  endif()
endforeach()

set(A_SOURCES)
set(HAVE_MATCHER OFF)
if(EXISTS ${SRC}/matcher.cpp AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/include/tokenlike/matcher.h)
  set(HAVE_MATCHER ON)
  foreach(f pattern nfa dfa lift compress scan matcher)
    if(EXISTS ${SRC}/${f}.cpp)
      list(APPEND A_SOURCES ${SRC}/${f}.cpp)
    endif()
  endforeach()
endif()

message(STATUS "TokenLIKE: loaders (Person B) = ${HAVE_LOADERS}, matcher (Person A) = ${HAVE_MATCHER}")

add_library(tokenlike STATIC ${C_SOURCES})
target_include_directories(tokenlike PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MSVC)
  target_compile_options(tokenlike PRIVATE /W4)
else()
  target_compile_options(tokenlike PRIVATE -Wall -Wextra)
endif()
if(HAVE_LOADERS)
  target_sources(tokenlike PRIVATE ${B_SOURCES})
  target_compile_definitions(tokenlike PUBLIC TOKENLIKE_HAVE_LOADERS=1)
endif()
if(HAVE_MATCHER)
  target_sources(tokenlike PRIVATE ${A_SOURCES})
  target_compile_definitions(tokenlike PUBLIC TOKENLIKE_HAVE_MATCHER=1)
endif()

# Self-contained executables on MinGW: avoids picking up an incompatible
# libstdc++ DLL from another toolchain on PATH.
if(MINGW)
  set(TL_EXE_LINK -static)
endif()

# Test-only fixture I/O (synthetic data; not the official pipeline).
add_library(tokenlike_fixtures STATIC tests/fixture_io.cpp)
target_link_libraries(tokenlike_fixtures PUBLIC tokenlike)

# --- CLI: build/tokenlike(.exe)
add_executable(tokenlike_cli ${SRC}/cli.cpp)
set_target_properties(tokenlike_cli PROPERTIES OUTPUT_NAME tokenlike)
target_link_libraries(tokenlike_cli PRIVATE tokenlike ${TL_EXE_LINK})
if(NOT HAVE_LOADERS)
  message(WARNING "Person B loaders absent: CLI uses TEST FIXTURE loaders (tests/fixture_io.cpp)")
  target_link_libraries(tokenlike_cli PRIVATE tokenlike_fixtures)
  target_include_directories(tokenlike_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_compile_definitions(tokenlike_cli PRIVATE TOKENLIKE_FIXTURE_LOADERS=1)
endif()
if(WIN32)
  target_link_libraries(tokenlike_cli PRIVATE shell32)
endif()

# --- Tests
enable_testing()

add_executable(test_baselines tests/test_baselines.cpp)
target_link_libraries(test_baselines PRIVATE tokenlike_fixtures ${TL_EXE_LINK})
add_test(NAME baselines COMMAND test_baselines)

if(HAVE_MATCHER AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_matcher.cpp)
  add_executable(test_matcher tests/test_matcher.cpp)
  target_link_libraries(test_matcher PRIVATE tokenlike ${TL_EXE_LINK})
  add_test(NAME matcher COMMAND test_matcher)
endif()

# Person B's loader tests. They assert the frozen 65,536-token vocabulary, so
# they run only on the real ClickBench column once it is generated locally.
if(HAVE_LOADERS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_io.cpp)
  add_executable(test_io tests/test_io.cpp)
  target_link_libraries(test_io PRIVATE tokenlike ${TL_EXE_LINK})
  set(DATA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data)
  if(EXISTS ${DATA_DIR}/vocab.bin AND EXISTS ${DATA_DIR}/clickbench/URL.tok)
    add_test(NAME io_clickbench_url COMMAND test_io ${DATA_DIR}/vocab.bin
             ${DATA_DIR}/clickbench/URL ${CMAKE_BINARY_DIR}/io_tmp)
  endif()
endif()

# --- Optional FSST module (Person B experiment; integration/fsst/).
# Off by default: with TOKENLIKE_WITH_FSST=OFF this repository has no FSST
# dependency at all and every target above builds exactly as it did before.
option(TOKENLIKE_WITH_FSST "Build the optional FSST-compressed LIKE module" OFF)
if(TOKENLIKE_WITH_FSST)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/fsst/CMakeLists.txt)
    add_subdirectory(integration/fsst)
    # Upstream FSST's _WIN32 branch calls __cpuidex without including <intrin.h>
    # (MSVC declares it implicitly). MinGW needs the header; upstream stays unmodified.
    if(MINGW AND TARGET fsst_upstream)
      target_compile_options(fsst_upstream PRIVATE -include intrin.h)
    endif()
    # Windows: the module's tools take main(int, char**), whose argv is in the ANSI
    # code page (Cyrillic patterns arrive as '?'). A UTF-8 activeCodePage manifest
    # makes argv UTF-8 without touching the module's sources. Link statically, like
    # the core tools, so no MinGW runtime DLL from another toolchain on PATH is used.
    if(MINGW)
      enable_language(RC)
      set(TL_UTF8_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/cmake/utf8.manifest)
      configure_file(cmake/utf8.rc.in ${CMAKE_CURRENT_BINARY_DIR}/utf8_manifest.rc @ONLY)
      foreach(t fsst_like fsst_encode fsst_liftbench fsst_test_unit)
        if(TARGET ${t})
          target_sources(${t} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/utf8_manifest.rc)
          target_link_options(${t} PRIVATE -static)
        endif()
      endforeach()
    endif()
  else()
    message(FATAL_ERROR "TOKENLIKE_WITH_FSST=ON but integration/fsst/ is not in this tree")
  endif()
endif()

# End-to-end CLI smoke test on a synthetic fixture.
set(FIXTURE_DIR ${CMAKE_BINARY_DIR}/fixture)
add_test(NAME fixture_write COMMAND test_baselines --write-fixture ${FIXTURE_DIR})
set_tests_properties(fixture_write PROPERTIES FIXTURES_SETUP synth)
set(CLI_MODES raw decode)
if(HAVE_MATCHER)
  list(APPEND CLI_MODES token)
endif()
foreach(mode ${CLI_MODES})
  add_test(NAME cli_${mode}
    COMMAND tokenlike_cli --mode ${mode} --pattern "%/v_/%" --runs 3
            --vocab ${FIXTURE_DIR}/vocab.bin --col ${FIXTURE_DIR}/synth/url
            --rowids ${FIXTURE_DIR}/rowids_${mode}.bin)
  set_tests_properties(cli_${mode} PROPERTIES FIXTURES_REQUIRED synth FIXTURES_SETUP cli_out)
endforeach()

# Every mode must write byte-identical row ids (sorted uint32), not just equal counts.
foreach(mode ${CLI_MODES})
  if(NOT mode STREQUAL "raw")
    add_test(NAME cli_rowids_${mode}_eq_raw
      COMMAND ${CMAKE_COMMAND} -E compare_files ${FIXTURE_DIR}/rowids_raw.bin ${FIXTURE_DIR}/rowids_${mode}.bin)
    set_tests_properties(cli_rowids_${mode}_eq_raw PROPERTIES FIXTURES_REQUIRED cli_out)
  endif()
endforeach()

# Invalid inputs must fail cleanly with exit code 1 (never a crash, never exit 0).
set(EXPECT_EXIT ${CMAKE_CURRENT_SOURCE_DIR}/tests/expect_exit.cmake)
foreach(mode ${CLI_MODES})
  add_test(NAME cli_rejects_bad_utf8_${mode}
    COMMAND ${CMAKE_COMMAND} -DEXPECT=1 "-DSTDERR_HAS=not valid UTF-8"
            "-DCMD=$<TARGET_FILE:tokenlike_cli>;--mode;${mode};--pattern-file;${FIXTURE_DIR}/bad_utf8_pattern.txt;--col;${FIXTURE_DIR}/synth/url;--vocab;${FIXTURE_DIR}/vocab.bin"
            -P ${EXPECT_EXIT})
  set_tests_properties(cli_rejects_bad_utf8_${mode} PROPERTIES FIXTURES_REQUIRED synth)
  if(NOT mode STREQUAL "raw")  # raw mode never reads token ids
    add_test(NAME cli_rejects_bad_ids_${mode}
      COMMAND ${CMAKE_COMMAND} -DEXPECT=1 "-DSTDERR_HAS=outside the vocabulary"
              "-DCMD=$<TARGET_FILE:tokenlike_cli>;--mode;${mode};--pattern;%;--col;${FIXTURE_DIR}/synth/bad_ids;--vocab;${FIXTURE_DIR}/vocab.bin"
              -P ${EXPECT_EXIT})
    set_tests_properties(cli_rejects_bad_ids_${mode} PROPERTIES FIXTURES_REQUIRED synth)
  endif()
endforeach()
add_test(NAME cli_help_exit0
  COMMAND ${CMAKE_COMMAND} -DEXPECT=0 -DSTDOUT_HAS=usage "-DCMD=$<TARGET_FILE:tokenlike_cli>;--help" -P ${EXPECT_EXIT})
add_test(NAME cli_rejects_repeated_option
  COMMAND ${CMAKE_COMMAND} -DEXPECT=1 "-DSTDERR_HAS=more than once"
          "-DCMD=$<TARGET_FILE:tokenlike_cli>;--mode;raw;--pattern;%;--col;${FIXTURE_DIR}/synth/url;--runs;1;--runs;2"
          -P ${EXPECT_EXIT})
set_tests_properties(cli_rejects_repeated_option PROPERTIES FIXTURES_REQUIRED synth)
