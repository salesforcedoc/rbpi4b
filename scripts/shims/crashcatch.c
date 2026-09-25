#define _GNU_SOURCE
#include <signal.h>
#include <unistd.h>
#include <ucontext.h>
#include <fcntl.h>
static void h(int sig, siginfo_t *si, void *uc) {
    ucontext_t *u = (ucontext_t*)uc;
    char b[256];
    static const char hex[] = "0123456789abcdef";
    int i = 0;
    unsigned int pc = (unsigned int)u->uc_mcontext.arm_pc;
    unsigned int ad = (unsigned int)si->si_addr;
    unsigned int lr = (unsigned int)u->uc_mcontext.arm_lr;
    b[i++]='C';b[i++]='R';b[i++]='A';b[i++]='S';b[i++]='H';b[i++]=' ';
    b[i++]='p';b[i++]='c';b[i++]='=';b[i++]='0';b[i++]='x';
    for (int s=28;s>=0;s-=4) b[i++]=hex[(pc>>s)&15];
    b[i++]=' ';b[i++]='a';b[i++]='d';b[i++]='d';b[i++]='r';b[i++]='=';b[i++]='0';b[i++]='x';
    for (int s=28;s>=0;s-=4) b[i++]=hex[(ad>>s)&15];
    b[i++]=' ';b[i++]='l';b[i++]='r';b[i++]='=';b[i++]='0';b[i++]='x';
    for (int s=28;s>=0;s-=4) b[i++]=hex[(lr>>s)&15];
    unsigned int regs[13] = { u->uc_mcontext.arm_r0, u->uc_mcontext.arm_r1,
        u->uc_mcontext.arm_r2, u->uc_mcontext.arm_r3, u->uc_mcontext.arm_r4,
        u->uc_mcontext.arm_r5, u->uc_mcontext.arm_r6, u->uc_mcontext.arm_r7,
        u->uc_mcontext.arm_r8, u->uc_mcontext.arm_r9, u->uc_mcontext.arm_r10,
        u->uc_mcontext.arm_fp, u->uc_mcontext.arm_ip };
    for (int r=0;r<13;r++) {
        b[i++]=' '; b[i++]='r'; b[i++]='0'+r/10; b[i++]='0'+r%10; b[i++]='=';
        b[i++]='0';b[i++]='x';
        for (int s=28;s>=0;s-=4) b[i++]=hex[(regs[r]>>s)&15];
    }
    b[i++]='\n';
    int fd = open("/tmp/crash.log", O_WRONLY|O_CREAT|O_APPEND, 0644);
    if (fd >= 0) { (void)write(fd, b, i); close(fd); }
    _exit(1);
}
__attribute__((constructor)) static void init(void) {
    struct sigaction sa;
    sa.sa_sigaction = h;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, 0);
}
