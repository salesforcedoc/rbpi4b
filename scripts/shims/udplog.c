/* udplog.c — tiny static UDP listener capturing rbp's DebugLog output
 * (sent as datagrams to 127.0.0.1:20001), appended to /data/rbp-debug.log.
 *
 * Build (static): arm-linux-gnueabi-gcc -O2 -static -o udplog udplog.c
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>

int main(void)
{
    int fd;
    struct sockaddr_in addr;
    FILE *log;
    char buf[4096];

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(20001);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }

    log = fopen("/data/rbp-debug.log", "a");
    if (!log) { perror("fopen"); return 1; }

    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0)
            continue;
        buf[n] = 0;
        fputs(buf, log);
        fflush(log);
    }
    return 0;
}
