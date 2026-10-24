# Optional TokenLIKE DuckDB extension (Person A). A standalone CMake project: the main TokenLIKE
# build (top-level CMakeLists.txt) does not include it and does not need DuckDB.
#
#   cmake -S integration/duckdb -B build/duckdb -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe
#   cmake --build build/duckdb        # -> build/duckdb/tokenlike.duckdb_extension
#
# The matcher and vocabulary loader are compiled from the repository's own src/*.cpp files in place
# (no copies), so the extension always runs exactly the shared matcher implementation.
cmake_minimum_required(VERSION 3.16)
if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)  # CMAKE_MSVC_RUNTIME_LIBRARY
endif()
project(tokenlike_duckdb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 8)
  message(FATAL_ERROR "GCC ${CMAKE_CXX_COMPILER_VERSION} is too old; use e.g. C:/msys64/ucrt64/bin/g++.exe")
endif()

get_filename_component(TOKENLIKE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(TL_SRC ${TOKENLIKE_ROOT}/src)

set(DUCKDB_PLATFORM "windows_amd64" CACHE STRING "DuckDB platform written into the extension footer (PRAGMA platform)")
set(DUCKDB_CAPI_VERSION "v1.5.6" CACHE STRING "C extension API version targeted by third_party/duckdb headers")
set(TOKENLIKE_EXT_VERSION "v0.1.0" CACHE STRING "Extension version string")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Person A matcher + Person B vocabulary loader, from the shared sources.
add_library(tokenlike_duckdb_core STATIC
  ${TL_SRC}/pattern.cpp ${TL_SRC}/nfa.cpp ${TL_SRC}/dfa.cpp ${TL_SRC}/lift.cpp
  ${TL_SRC}/compress.cpp ${TL_SRC}/scan.cpp ${TL_SRC}/matcher.cpp ${TL_SRC}/vocab.cpp)
target_include_directories(tokenlike_duckdb_core PUBLIC ${TOKENLIKE_ROOT}/include)
set_target_properties(tokenlike_duckdb_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(tokenlike_duckdb SHARED src/tokenlike_duckdb.cpp)
target_include_directories(tokenlike_duckdb PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/duckdb)
target_link_libraries(tokenlike_duckdb PRIVATE tokenlike_duckdb_core)
if(MSVC)
  target_compile_options(tokenlike_duckdb_core PRIVATE /W3 /EHsc /utf-8)
  # C4141: DuckDB's entry-point macro applies __declspec(dllexport) twice (harmless).
  target_compile_options(tokenlike_duckdb PRIVATE /W3 /EHsc /utf-8 /D_CRT_SECURE_NO_WARNINGS /wd4141)
else()
  target_compile_options(tokenlike_duckdb_core PRIVATE -Wall -Wextra)
  # The DuckDB header's `= {0}` initialiser of the API struct triggers this warning.
  target_compile_options(tokenlike_duckdb PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()
if(MINGW)
  # DuckDB (MSVC-built on Windows) loads this DLL directly: no MinGW runtime DLLs may be required.
  target_link_options(tokenlike_duckdb PRIVATE -static -static-libgcc -static-libstdc++)
endif()
set_target_properties(tokenlike_duckdb PROPERTIES PREFIX "" OUTPUT_NAME "tokenlike_raw")

set(TOKENLIKE_EXT_FILE ${CMAKE_BINARY_DIR}/tokenlike.duckdb_extension)
add_custom_command(TARGET tokenlike_duckdb POST_BUILD
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/append_metadata.py
          --library $<TARGET_FILE:tokenlike_duckdb> --out ${TOKENLIKE_EXT_FILE}
          --platform ${DUCKDB_PLATFORM} --duckdb-version ${DUCKDB_CAPI_VERSION}
          --extension-version ${TOKENLIKE_EXT_VERSION} --abi-type C_STRUCT
  COMMENT "Appending DuckDB extension metadata -> ${TOKENLIKE_EXT_FILE}")

# Data-free smoke test: loads the extension into DuckDB (Python package) with a synthetic
# vocabulary and checks token_like against DuckDB LIKE on adversarial fixtures.
enable_testing()
add_test(NAME duckdb_sql_smoke
  COMMAND ${Python3_EXECUTABLE} -X utf8 ${CMAKE_CURRENT_SOURCE_DIR}/python/test_sql.py --smoke
          --ext ${TOKENLIKE_EXT_FILE})
