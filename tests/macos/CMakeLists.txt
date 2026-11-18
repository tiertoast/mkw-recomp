cmake_minimum_required(VERSION 3.24)
project(mkw_darwin_foundations LANGUAGES C CXX)
if(NOT APPLE OR NOT CMAKE_C_COMPILER_ID STREQUAL "AppleClang" OR NOT CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64"
   OR NOT MKW_GAME_ID OR NOT MKW_HOST_EXPORTS)
  message(FATAL_ERROR "Configure via tools/build/build.py --target macos-x86_64")
endif()

set(rt "${CMAKE_CURRENT_LIST_DIR}/../../runtime")
if(MKW_FRAME_POINTERS)
  add_compile_options(-fno-omit-frame-pointer)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
set(CMAKE_INSTALL_RPATH "@loader_path")
add_compile_options(-Wall -Wextra -Werror -march=x86-64-v3)
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>")
add_link_options(-Wl,-dead_strip "$<$<LINK_LANGUAGE:CXX>:-stdlib=libc++>")
add_compile_definitions(RT_GAME_ID="${MKW_GAME_ID}")
include_directories("${rt}" "${rt}/core/base" "${rt}/core/boot" "${rt}/core/diag" "${rt}/cpu")

include("${CMAKE_CURRENT_LIST_DIR}/../../tools/build/darwin.cmake")
add_library(mkwgame SHARED module.c)
set_target_properties(mkwgame PROPERTIES PREFIX "" INSTALL_NAME_DIR "@rpath" BUILD_WITH_INSTALL_NAME_DIR ON C_VISIBILITY_PRESET hidden)
target_link_options(mkwgame PRIVATE "LINKER:-exported_symbol,_mkwgame_tables")
target_link_libraries(mkwgame PRIVATE mkw-host)

add_library(registrations STATIC declarations.cpp)
add_executable(foundations fixture.c support.c "${rt}/cpu/hostfn.c" "${rt}/cpu/dispatch.c" "${rt}/cpu/sjlj.c"
               "${rt}/core/boot/gamelib.c")
set_target_properties(foundations PROPERTIES C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden)
target_link_libraries(foundations PRIVATE darwin-platform mkw-host "$<LINK_LIBRARY:WHOLE_ARCHIVE,registrations>")
add_dependencies(foundations mkwgame)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mkwgame.id" CONTENT "${MKW_GAME_ID}")

enable_testing()
add_executable(thread-test threads.c)
target_link_libraries(thread-test PRIVATE darwin-platform)
add_test(NAME darwin-threads COMMAND thread-test)
add_test(NAME macho-roundtrip COMMAND foundations "$<TARGET_FILE_DIR:mkwgame>")
set_tests_properties(darwin-threads macho-roundtrip PROPERTIES TIMEOUT 20)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME macho-contract COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/check.py"
         "$<TARGET_FILE:foundations>" "${MKW_GAME_ID}" "${MKW_HOST_EXPORTS}")
set_tests_properties(macho-contract PROPERTIES TIMEOUT 30)

# No entries of any kind. Exercise ld's synthesized section bounds with the real lookup code.
add_executable(empty-registry empty.c "${rt}/cpu/hostfn.c")
add_test(NAME macho-empty-registry COMMAND empty-registry)
set_tests_properties(macho-empty-registry PROPERTIES TIMEOUT 10)
