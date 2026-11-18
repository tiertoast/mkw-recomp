// A fresh ASLR layout for mem_init's bounded retry of fixed module windows.
#include "platform.h"
#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#include <unistd.h>

int plat_setenv(const char *name, const char *value) {
    return (value && *value ? setenv(name, value, 1) : unsetenv(name)) == 0;
}

_Noreturn void plat_reexec(void) {
    uint32_t size = 0;
    _NSGetExecutablePath(NULL, &size);
    char *path = malloc(size);
    if (path && !_NSGetExecutablePath(path, &size)) execv(path, *_NSGetArgv());
    perror("Darwin VM: re-executing the process");
    _Exit(70);
}

uintptr_t plat_exe_base(void) { return (uintptr_t)_dyld_get_image_header(0); }

int plat_exe_path(char *out, size_t cap) {
    uint32_t size = cap > UINT32_MAX ? UINT32_MAX : (uint32_t)cap;
    return size && !_NSGetExecutablePath(out, &size);
}
