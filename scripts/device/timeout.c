/*
 * timeout.c — minimal `timeout SECONDS COMMAND [ARG]...` for the SC Live 4.
 *
 * The device's busybox has no timeout applet, and usb-watch.sh needs one so a
 * FIFO write to a dead rbp can never block the watcher forever. This is a
 * tiny MIT-licensed replacement for the coreutils tool.
 *
 * Build (hard-float ARM, runs directly on the RK3288):
 *   arm-linux-gnueabihf-gcc -O2 -static -o timeout timeout.c
 *
 * Exit status follows the usual convention:
 *   command's status, or 124 if the time limit was reached, 125/127 on errors.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t g_timed_out = 0;
static pid_t g_child = -1;

static void on_alarm(int sig)
{
    (void)sig;
    g_timed_out = 1;
    if (g_child > 0)
        kill(g_child, SIGKILL);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: timeout SECONDS COMMAND [ARG]...\n");
        return 125;
    }

    double secs = atof(argv[1]);

    /* A non-positive duration means "no limit": just run the command. */
    if (secs <= 0.0) {
        execvp(argv[2], &argv[2]);
        perror("timeout: exec");
        return 127;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);

    g_child = fork();
    if (g_child < 0) {
        perror("timeout: fork");
        return 125;
    }
    if (g_child == 0) {
        execvp(argv[2], &argv[2]);
        perror("timeout: exec");
        _exit(127);
    }

    struct itimerval it;
    memset(&it, 0, sizeof(it));
    it.it_value.tv_sec = (time_t)secs;
    it.it_value.tv_usec = (suseconds_t)((secs - (double)it.it_value.tv_sec) * 1e6);
    setitimer(ITIMER_REAL, &it, NULL);

    int status;
    while (waitpid(g_child, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        perror("timeout: waitpid");
        return 125;
    }

    if (g_timed_out)
        return 124;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 125;
}
