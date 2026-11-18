cmake_minimum_required(VERSION 3.24)
project(mkw_darwin_runtime LANGUAGES C CXX ASM)
if(NOT APPLE OR NOT CMAKE_C_COMPILER_ID STREQUAL "AppleClang"
   OR NOT CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64" OR NOT MKW_GAME_ID OR NOT MKW_RUNTIME_SOURCES)
  message(FATAL_ERROR "Configure via tools/build/build.py --target macos-x86_64 --runtime-only")
endif()

find_package(SDL3 CONFIG REQUIRED)
set(VULKAN_PREFIX "/usr/local" CACHE PATH "LunarG Vulkan SDK prefix")
find_path(VULKAN_INCLUDE vulkan/vulkan.h PATHS "${VULKAN_PREFIX}/include" NO_DEFAULT_PATH REQUIRED)
find_file(VULKAN_LOADER libvulkan.1.dylib PATHS "${VULKAN_PREFIX}/lib" NO_DEFAULT_PATH REQUIRED)
find_path(OPUS_INCLUDE opus/opus.h REQUIRED)
find_path(VPX_INCLUDE vpx/vpx_decoder.h REQUIRED)
find_path(ZSTD_INCLUDE zstd.h REQUIRED)
find_path(XXHASH_INCLUDE xxhash.h REQUIRED)
find_path(VMA_INCLUDE vk_mem_alloc.h REQUIRED)
find_library(OPUS_LIBRARY opus REQUIRED)
find_library(VPX_LIBRARY vpx REQUIRED)
find_library(ZSTD_LIBRARY zstd REQUIRED)

get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(rt "${root}/runtime")
include("${CMAKE_CURRENT_LIST_DIR}/../build/darwin.cmake")
set(gen "${CMAKE_CURRENT_BINARY_DIR}/rt")
add_custom_command(OUTPUT "${gen}/app_icon.c"
  COMMAND "${MKW_PYTHON}" "${root}/tools/build/icon.py" "${gen}/app_icon.c"
  DEPENDS "${root}/tools/build/icon.py"
  VERBATIM)
file(GLOB translations CONFIGURE_DEPENDS "${gen}/i18n/ui_text_*.c")
add_executable(mkw ${MKW_RUNTIME_SOURCES} "${gen}/defaults.c" "${gen}/app_icon.c" ${translations})
set_target_properties(mkw PROPERTIES C_STANDARD 11 CXX_STANDARD 17
  C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden
  BUILD_RPATH "@loader_path;${VULKAN_PREFIX}/lib")
target_compile_definitions(mkw PRIVATE RT_GAME_ID="${MKW_GAME_ID}")
target_compile_options(mkw PRIVATE -Wall -fno-strict-aliasing -march=x86-64-v3)
if(MKW_FRAME_POINTERS)
  target_compile_options(mkw PRIVATE -fno-omit-frame-pointer)
endif()
file(GLOB_RECURSE headers CONFIGURE_DEPENDS "${rt}/*.h")
set(runtime_includes "${rt}")
foreach(header IN LISTS headers)
  get_filename_component(directory "${header}" DIRECTORY)
  list(APPEND runtime_includes "${directory}")
endforeach()
list(REMOVE_DUPLICATES runtime_includes)
target_include_directories(mkw PRIVATE ${runtime_includes} "${gen}"
  "${VULKAN_INCLUDE}" "${OPUS_INCLUDE}" "${VPX_INCLUDE}" "${ZSTD_INCLUDE}" "${XXHASH_INCLUDE}" "${VMA_INCLUDE}")
target_link_libraries(mkw PRIVATE mkw-host SDL3::SDL3 "${VULKAN_LOADER}"
  "${OPUS_LIBRARY}" "${VPX_LIBRARY}" "${ZSTD_LIBRARY}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mkwgame.id" CONTENT "${MKW_GAME_ID}")
