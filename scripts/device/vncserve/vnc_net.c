/*
 * vnc_net.c -- see vnc_net.h.
 */
#include "vnc_net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static FILE *g_log;

FILE *vnc_logfp(void) { return g_log ? g_log : stdout; }
void vnc_log_to(FILE *f) { g_log = f; }

void vlog(const char *fmt, ...)
{
    va_list ap;
    struct timespec ts;
    struct tm tm;
    FILE *f = vnc_logfp();

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    fprintf(f, "%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec,
            ts.tv_nsec / 1000000);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
}

int vnc_net_listen(const char *bindaddr, int port)
{
    int fd, on = 1;
    struct sockaddr_in sa;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { vlog("socket: %s", strerror(errno)); return -1; }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);

    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = bindaddr ? inet_addr(bindaddr) : htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        vlog("bind %s:%d: %s", bindaddr ? bindaddr : "0.0.0.0", port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 4) < 0) {
        vlog("listen: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

int vnc_net_write_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w; n -= (size_t)w;
    }
    return 0;
}

int vnc_net_read_to(int fd, void *buf, size_t n, int ms)
{
    uint8_t *p = buf;
    while (n) {
        struct pollfd pf;
        int r;
        ssize_t g;
        pf.fd = fd;
        pf.events = POLLIN;
        pf.revents = 0;
        r = poll(&pf, 1, ms);
        if (r <= 0) return 0;
        g = recv(fd, p, n, 0);
        if (g <= 0) {
            if (g < 0 && errno == EINTR) continue;
            return 0;
        }
        p += g; n -= (size_t)g;
    }
    return 1;
}

uint32_t vnc_net_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
