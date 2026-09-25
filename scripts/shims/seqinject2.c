/*
 * seqinject2.c — static ARM test tool: inject MIDI events into the knobshim2
 * sequencer input port (default 128:0, overridable with --dest) to exercise
 * the full Prime GO -> RX3 mapping without touching hardware.
 *
 *   seqinject2 note <ch> <note> <on|off>       note event (0-based ch)
 *   seqinject2 cc   <ch> <cc> <val>            control change
 *   seqinject2 jog  <deck> <pos14>             jog pair CCs 0x37/0x4D (deck 0|1)
 *   seqinject2 pitch <deck> <pos14>            pitch fader pair 0x1F/0x4B
 *   seqinject2 knob <val>                      browse knob CC5 ch15
 *   seqinject2 --dest C:P note ...             destination client:port
 *
 * Build:
 *   arm-linux-gnueabi-gcc -O2 -static -o seqinject2 seqinject2.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sound/asequencer.h>

static int seq_fd = -1;
static int seq_client = -1;
static int seq_port = -1;
static int dest_client = 128;
static int dest_port = 0;

static void die(const char *msg)
{
     fprintf(stderr, "seqinject2: %s: %s\n", msg, strerror(errno));
     exit(1);
}

static void seq_init(void)
{
     struct snd_seq_client_info cinfo;
     struct snd_seq_port_info pinfo;
     struct snd_seq_port_subscribe sub;
     int ver;

     seq_fd = open("/dev/snd/seq", O_RDWR);
     if (seq_fd < 0) die("open /dev/snd/seq");
     if (ioctl(seq_fd, SNDRV_SEQ_IOCTL_PVERSION, &ver) < 0) die("PVERSION");
     if (ioctl(seq_fd, SNDRV_SEQ_IOCTL_CLIENT_ID, &seq_client) < 0) die("CLIENT_ID");

     memset(&cinfo, 0, sizeof(cinfo));
     cinfo.client = seq_client;
     cinfo.type = USER_CLIENT;
     snprintf(cinfo.name, sizeof(cinfo.name), "seqinject2");
     if (ioctl(seq_fd, SNDRV_SEQ_IOCTL_SET_CLIENT_INFO, &cinfo) < 0) die("SET_CLIENT_INFO");

     memset(&pinfo, 0, sizeof(pinfo));
     pinfo.addr.client = seq_client;
     snprintf(pinfo.name, sizeof(pinfo.name), "seqinject2-out");
     pinfo.capability = SNDRV_SEQ_PORT_CAP_READ | SNDRV_SEQ_PORT_CAP_SUBS_READ;
     pinfo.type = SNDRV_SEQ_PORT_TYPE_MIDI_GENERIC | SNDRV_SEQ_PORT_TYPE_APPLICATION;
     if (ioctl(seq_fd, SNDRV_SEQ_IOCTL_CREATE_PORT, &pinfo) < 0) die("CREATE_PORT");
     seq_port = pinfo.addr.port;

     memset(&sub, 0, sizeof(sub));
     sub.sender.client = seq_client;
     sub.sender.port = seq_port;
     sub.dest.client = dest_client;
     sub.dest.port = dest_port;
     sub.queue = SNDRV_SEQ_QUEUE_DIRECT;
     if (ioctl(seq_fd, SNDRV_SEQ_IOCTL_SUBSCRIBE_PORT, &sub) < 0) {
          fprintf(stderr, "seqinject2: SUBSCRIBE %d:%d -> %d:%d failed: %s\n",
                  seq_client, seq_port, dest_client, dest_port, strerror(errno));
          exit(1);
     }
     fprintf(stderr, "seqinject2: %d:%d -> %d:%d\n", seq_client, seq_port,
             dest_client, dest_port);
}

static void send_event(const struct snd_seq_event *ev)
{
     struct snd_seq_event e = *ev;
     e.queue = SNDRV_SEQ_QUEUE_DIRECT;      /* 253 — direct dispatch */
     e.flags = SNDRV_SEQ_EVENT_LENGTH_FIXED; /* 0 */
     e.source.client = seq_client;
     e.source.port = seq_port;
     e.dest.client = dest_client;
     e.dest.port = dest_port;
     if (write(seq_fd, &e, sizeof(e)) != (ssize_t)sizeof(e))
          die("write event");
}

static void note(int ch, int n, int on)
{
     struct snd_seq_event ev;
     memset(&ev, 0, sizeof(ev));
     ev.flags = SNDRV_SEQ_EVENT_LENGTH_FIXED;
     ev.type = on ? SNDRV_SEQ_EVENT_NOTEON : SNDRV_SEQ_EVENT_NOTEOFF;
     ev.data.note.channel = ch;
     ev.data.note.note = n;
     ev.data.note.velocity = on ? 100 : 0;
     send_event(&ev);
     fprintf(stderr, "note ch%d n%d %s\n", ch, n, on ? "on" : "off");
}

static void cc(int ch, int param, int val)
{
     struct snd_seq_event ev;
     memset(&ev, 0, sizeof(ev));
     ev.flags = SNDRV_SEQ_EVENT_LENGTH_FIXED;
     ev.type = SNDRV_SEQ_EVENT_CONTROLLER;
     ev.data.control.channel = ch;
     ev.data.control.param = param;
     ev.data.control.value = val;
     send_event(&ev);
     fprintf(stderr, "cc ch%d cc%d val%d\n", ch, param, val);
}

int main(int argc, char **argv)
{
     int i = 1;
     while (i < argc && strcmp(argv[i], "--dest") == 0 && i + 1 < argc) {
          char *p;
          dest_client = strtol(argv[i + 1], &p, 10);
          if (*p == ':') dest_port = strtol(p + 1, NULL, 10);
          i += 2;
     }
     if (i >= argc) {
          fprintf(stderr,
                  "usage: %s [--dest C:P] note <ch> <n> <on|off>\n"
                  "                    | cc <ch> <cc> <val>\n"
                  "                    | jog <deck0|1> <pos14>\n"
                  "                    | pitch <deck0|1> <pos14>\n"
                  "                    | knob <val>\n",
                  argv[0]);
          return 1;
     }
     seq_init();
     if (strcmp(argv[i], "note") == 0 && i + 3 < argc) {
          int ch = atoi(argv[i + 1]);
          int n = atoi(argv[i + 2]);
          int on = strcmp(argv[i + 3], "off") != 0;
          note(ch, n, on);
     } else if (strcmp(argv[i], "cc") == 0 && i + 3 < argc) {
          cc(atoi(argv[i + 1]), atoi(argv[i + 2]), atoi(argv[i + 3]));
     } else if (strcmp(argv[i], "jog") == 0 && i + 2 < argc) {
          int d = atoi(argv[i + 1]) == 0 ? 2 : 3;   /* deck rch 2/3 */
          int pos = atoi(argv[i + 2]);
          if (pos < 0) pos = 0;
          if (pos > 0x3FFF) pos = 0x3FFF;
          cc(d, 0x37, (pos >> 7) & 0x7F);
          cc(d, 0x4D, pos & 0x7F);
     } else if (strcmp(argv[i], "pitch") == 0 && i + 2 < argc) {
          int d = atoi(argv[i + 1]) == 0 ? 2 : 3;
          int pos = atoi(argv[i + 2]);
          if (pos < 0) pos = 0;
          if (pos > 0x3FFF) pos = 0x3FFF;
          cc(d, 0x1F, (pos >> 7) & 0x7F);
          cc(d, 0x4B, pos & 0x7F);
     } else if (strcmp(argv[i], "knob") == 0 && i + 1 < argc) {
          cc(15, 5, atoi(argv[i + 1]));
     } else {
          fprintf(stderr, "unknown command '%s'\n", argv[i]);
          return 1;
     }
     usleep(100000);
     return 0;
}
