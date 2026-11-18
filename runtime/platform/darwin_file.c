// The read-only subset needed to check a module and its ABI sidecar before mapping code.
#include "platform.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int plat_file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

uint8_t *plat_file_read(const char *path, uint64_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    struct stat st;
    uint8_t *data = NULL;
    if (!fstat(fileno(f), &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= (1ll << 30)) {
        size_t bytes = (size_t)st.st_size;
        data = malloc(bytes + 1);
        if (data && fread(data, 1, bytes, f) == bytes) {
            data[bytes] = 0;
            *size = bytes;
        } else {
            free(data);
            data = NULL;
        }
    }
    fclose(f);
    return data;
}
