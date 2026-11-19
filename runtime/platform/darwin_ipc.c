// Local Discord stream sockets. A busy listener never stalls startup.
#include "platform.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct PlatIpc { int fd; };

PlatIpc *plat_ipc_open(const char *name) {
    const char *dirs[4] = {getenv("XDG_RUNTIME_DIR"), getenv("TMPDIR"), getenv("TMP"), "/tmp"};
    static const char *const subs[] = {"", "app/com.discordapp.Discord/", "snap.discord/"};
    for (size_t d = 0; d < 4; d++) {
        if (!dirs[d] || !*dirs[d]) continue;
        for (size_t s = 0; s < sizeof subs / sizeof *subs; s++) {
            struct sockaddr_un a = {.sun_family = AF_UNIX};
            int n = snprintf(a.sun_path, sizeof a.sun_path, "%s/%s%s", dirs[d], subs[s], name);
            if (n <= 0 || (size_t)n >= sizeof a.sun_path) continue;
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) return NULL;
            int one = 1;
            if (fcntl(fd, F_SETFD, FD_CLOEXEC) || fcntl(fd, F_SETFL, O_NONBLOCK) ||
                setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one)) {
                close(fd);
                continue;
            }
            if (!connect(fd, (struct sockaddr *)&a, sizeof a) && !fcntl(fd, F_SETFL, 0)) {
                PlatIpc *c = malloc(sizeof *c);
                if (!c) return close(fd), NULL;
                c->fd = fd;
                return c;
            }
            close(fd);
        }
    }
    return NULL;
}

int plat_ipc_send(PlatIpc *c, const void *data, size_t n) {
    for (const char *p = data; n;) {
        ssize_t r = send(c->fd, p, n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return 0;
        p += r, n -= (size_t)r;
    }
    return 1;
}

long plat_ipc_recv(PlatIpc *c, void *buf, size_t cap) {
    for (;;) {
        ssize_t r = recv(c->fd, buf, cap, MSG_DONTWAIT);
        if (r > 0) return (long)r;
        if (r == 0) return -1;   // the peer closed
        if (errno == EINTR) continue;
        return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
    }
}

void plat_ipc_close(PlatIpc *c) {
    if (!c) return;
    close(c->fd);
    free(c);
}
