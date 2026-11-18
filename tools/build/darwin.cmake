# Native targets used by the foundations fixture. The module imports only from the versioned ABI library.
add_library(mkw-host SHARED "${rt}/core/boot/darwin_abi.c")
set_target_properties(mkw-host PROPERTIES VERSION 1.0.0 SOVERSION 1 INSTALL_NAME_DIR "@rpath" BUILD_WITH_INSTALL_NAME_DIR ON)
foreach(symbol IN LISTS MKW_HOST_EXPORTS ITEMS mkw_bind_host_v1)
  target_link_options(mkw-host PRIVATE "LINKER:-exported_symbol,_${symbol}")
endforeach()

add_library(darwin-platform STATIC "${rt}/platform/darwin_fiber.c" "${rt}/platform/darwin_switch.S"
            "${rt}/platform/darwin_file.c" "${rt}/platform/darwin_mem.c"
            "${rt}/platform/darwin_process.c" "${rt}/platform/darwin_sync.c" "${rt}/platform/darwin_thread.c" "${rt}/platform/common.c")

add_library(darwin-kernel STATIC "${rt}/core/kernel/kernel.c" "${rt}/core/kernel/kernel_svc.c"
            "${rt}/core/kernel/kernel_fiber.c" "${rt}/core/kernel/kernel_preempt.c"
            "${rt}/core/kernel/kernel_cores.c" "${rt}/core/kernel/kernel_diag.c")
target_link_libraries(darwin-kernel PUBLIC darwin-platform)
