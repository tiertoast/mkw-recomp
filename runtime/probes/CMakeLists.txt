cmake_minimum_required(VERSION 3.24)
project(mkw_sparse_probe LANGUAGES C)

# Deliberately independent of the runtime, SDL, VMA, shaders and generated game modules.
find_package(Vulkan REQUIRED)
add_executable(vk-sparse-probe vk_sparse_probe.c)
target_compile_features(vk-sparse-probe PRIVATE c_std_11)
target_compile_definitions(vk-sparse-probe PRIVATE MKW_STANDALONE_SPARSE_PROBE)
target_compile_options(vk-sparse-probe PRIVATE -Wall -Wextra -Werror)
target_link_libraries(vk-sparse-probe PRIVATE Vulkan::Vulkan)
if(APPLE)
  get_filename_component(vulkan_lib_dir "${Vulkan_LIBRARY}" DIRECTORY)
  set_target_properties(vk-sparse-probe PROPERTIES BUILD_RPATH "${vulkan_lib_dir}")
endif()
# Explicit local invocation only: this GPU diagnostic must not run automatically in CI/CTest.
