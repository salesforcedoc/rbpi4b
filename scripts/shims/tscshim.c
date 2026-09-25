/*
 * tscshim.c — LD_PRELOAD shim that emulates the XDJ-RX3's custom tsc2007
 * touch device (/dev/tsc2007_2-0048) on the Denon Prime GO, translating
 * events from the ILI2117 evdev touchscreen (/dev/input/event0).
 *
 * Protocol emulated (from rbp's TouchPanelComm):
 *   open("/dev/tsc2007_2-0048", O_RDWR)
 *   ioctl(fd, 0x80046b00, &x) -> return max X (=3)      (read max_rt)
 *   ioctl(fd, 0x40046b00, &x) -> accept                 (write max_rt)
 *   ioctl(fd, 0x80026b01, &y) -> return max Y (=3900)   (read max_rt)
 *   ioctl(fd, 0x40026b01, &y) -> accept                 (write max_rt)
 *   read(fd, buf, 6) -> ts_data { u8 flag; u8 pad; u16 x; u16 y }
 *
 * Coordinate transform (logical 1280x800 space):
 *   panel raw rx,ry in [0,2048):  px = rx*800/2048, py = ry*1280/2048
 *   logical: lx = 1279 - py, ly = px        (driver rot=90: px=ly, py=1279-lx)
 *
 * Build: arm-linux-gnueabi-gcc -O2 -mfloat-abi=soft -fno-stack-protector
 *        -fPIC -shared -o tscshim.so tscshim.c -lpthread -lc
 *        -Wl,--allow-shlib-undefined -Wl,-rpath-link,/tmp/arm213sysroot/lib
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stdint.h>

/* ---- real syscalls (bypass libc wrappers we might be overriding) ---- */
static int real_open(const char *p, int flags)
{
     return syscall(SYS_openat, AT_FDCWD, p, flags, 0);
}
static int real_ioctl(int fd, unsigned long req, void *arg)
{
     return syscall(SYS_ioctl, fd, req, arg);
}
static int real_read(int fd, void *buf, size_t n)
{
     return syscall(SYS_read, fd, buf, n);
}
static int real_close(int fd)
{
     return syscall(SYS_close, fd);
}
static int real_pipe2(int fds[2])
{
     return syscall(SYS_pipe2, fds, 0);
}

/* ---- evdev ---- */
struct input_event {
     struct { uint32_t sec, usec; } time;
     uint16_t type, code;
     int32_t  value;
};
#define EV_KEY 0x01
#define EV_ABS 0x03
#define EV_SYN 0x00
#define ABS_X  0x00
#define ABS_Y  0x01
#define BTN_TOUCH 0x14a
#define SYN_REPORT 0

/* ---- emulated device ---- */
#define TSC_DEVICE "/dev/tsc2007_2-0048"
#define EVDEV_PATH "/dev/input/event0"
#define RAW_MAX 2048
#define LOGICAL_W 1280
#define LOGICAL_H 800
#define TSC_MAX_X 3      /* values rbp's TouchPanelComm expects */
#define TSC_MAX_Y 3900

/* reader thread device fd */
static int evfd = -1;

/* fake fd table */
static int fake_pipe_rd[64];   /* fd handed to rbp */
static int fake_used[64];
static int out_pipe[2] = {-1, -1};   /* [0]=read end (dup'd for rbp), [1]=write end (reader thread) */
static pthread_t reader_tid;
static int reader_started = 0;

static int cur_flag = 0;

/* ---- transform raw panel -> logical ---- */
static void transform(int rx, int ry, int *lx, int *ly)
{
     int px = (rx * 800) / RAW_MAX;   /* physical x on 800x1280 panel */
     int py = (ry * 1280) / RAW_MAX;  /* physical y */
     *lx = 1279 - py;                 /* driver rot=left: px=ly, py=1279-lx */
     *ly = px;
}

/* ---- push a 6-byte ts_data record ---- */
static void push_touch(int flag, int x, int y)
{
     unsigned char buf[6];
     buf[0] = (unsigned char)(flag ? 1 : 0);
     buf[1] = 0;
     buf[2] = (unsigned char)(x & 0xff);
     buf[3] = (unsigned char)((x >> 8) & 0xff);
     buf[4] = (unsigned char)(y & 0xff);
     buf[5] = (unsigned char)((y >> 8) & 0xff);
     if (out_pipe[1] >= 0)
          (void)write(out_pipe[1], buf, 6);
}

/* ---- evdev reader thread ---- */
static void *reader_thread(void *arg)
{
     struct input_event ev;
     int rx = 0, ry = 0;
     int last_flag = -1, last_x = 0, last_y = 0;
     (void)arg;

     for (;;) {
          int n = real_read(evfd, &ev, sizeof(ev));
          if (n != (int)sizeof(ev)) {
               if (n < 0 && (errno == EINTR || errno == EAGAIN))
                    continue;
               /* device went away: reopen */
               real_close(evfd);
               evfd = -1;
               while (evfd < 0) {
                    evfd = real_open(EVDEV_PATH, O_RDWR);
                    if (evfd < 0)
                         usleep(500000);
               }
               continue;
          }

          switch (ev.type) {
          case EV_ABS:
               if (ev.code == ABS_X)
                    rx = ev.value;
               else if (ev.code == ABS_Y)
                    ry = ev.value;
               break;
          case EV_KEY:
               if (ev.code == BTN_TOUCH)
                    cur_flag = ev.value ? 1 : 0;
               break;
          case EV_SYN:
               if (ev.code == SYN_REPORT) {
                    int lx, ly;
                    transform(rx, ry, &lx, &ly);
                    if (cur_flag != last_flag || lx != last_x || ly != last_y) {
                         push_touch(cur_flag, lx, ly);
                         last_flag = cur_flag;
                         last_x = lx;
                         last_y = ly;
                    }
               }
               break;
          default:
               break;
          }
     }
     return NULL;
}

/* ---- start the emulation (once) ---- */
static void ensure_reader(void)
{
     if (reader_started)
          return;
     evfd = real_open(EVDEV_PATH, O_RDWR);
     if (evfd < 0) {
          return; /* will retry on next open */
     }
     if (real_pipe2(out_pipe) < 0) {
          real_close(evfd);
          evfd = -1;
          return;
     }
     if (pthread_create(&reader_tid, NULL, reader_thread, NULL) != 0) {
          real_close(out_pipe[0]);
          real_close(out_pipe[1]);
          out_pipe[0] = out_pipe[1] = -1;
          real_close(evfd);
          evfd = -1;
          return;
     }
     reader_started = 1;
}

/* ---- find/create a fake fd entry ---- */
static int tsc_open_impl(void)
{
     int i, rd_end;

     ensure_reader();
     if (out_pipe[0] < 0)
          return -1;

     /* hand rbp a dup of the pipe's read end */
     rd_end = dup(out_pipe[0]);
     if (rd_end < 0)
          return -1;

     for (i = 0; i < 64; i++) {
          if (!fake_used[i]) {
               fake_used[i] = 1;
               fake_pipe_rd[i] = rd_end;
               return rd_end;
          }
     }
     real_close(rd_end);
     return -1;
}

/* ---- LD_PRELOAD entry points ---- */

int open(const char *pathname, int flags, ...)
{
     mode_t mode = 0;
     va_list ap;
     if (flags & (O_CREAT | O_TMPFILE)) {
          va_start(ap, flags);
          mode = va_arg(ap, mode_t);
          va_end(ap);
     }
     if (strcmp(pathname, TSC_DEVICE) == 0)
          return tsc_open_impl();
     return real_open(pathname, flags);
}

int open64(const char *pathname, int flags, ...)
{
     mode_t mode = 0;
     va_list ap;
     if (flags & (O_CREAT | O_TMPFILE)) {
          va_start(ap, flags);
          mode = va_arg(ap, mode_t);
          va_end(ap);
     }
     if (strcmp(pathname, TSC_DEVICE) == 0)
          return tsc_open_impl();
     return real_open(pathname, flags | O_LARGEFILE);
}

int ioctl(int fd, unsigned long request, ...)
{
     void *arg;
     va_list ap;
     va_start(ap, request);
     arg = va_arg(ap, void *);
     va_end(ap);

     /* only handle ioctls on our fake fds; else real */
     switch (request) {
     case 0x80046b00: { /* _IOR('k',0,4): read max X */
          unsigned int *p = (unsigned int *)arg;
          if (p) *p = TSC_MAX_X;
          return 0;
     }
     case 0x40046b00: /* _IOW('k',0,4): write max X */
          return 0;
     case 0x80026b01: { /* _IOR('k',1,2): read max Y */
          unsigned short *p = (unsigned short *)arg;
          if (p) *p = TSC_MAX_Y;
          return 0;
     }
     case 0x40026b01: /* _IOW('k',1,2): write max Y */
          return 0;
     default:
          return real_ioctl(fd, request, arg);
     }
}

ssize_t read(int fd, void *buf, size_t count)
{
     if (fd >= 0 && fd < 64 && fake_used[fd] && fake_pipe_rd[fd] == fd)
          return real_read(fake_pipe_rd[fd], buf, count);
     return real_read(fd, buf, count);
}

int close(int fd)
{
     if (fd >= 0 && fd < 64 && fake_used[fd] && fake_pipe_rd[fd] == fd) {
          fake_used[fd] = 0;
          return real_close(fake_pipe_rd[fd]);
     }
     return real_close(fd);
}
