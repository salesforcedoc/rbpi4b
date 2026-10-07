/*
 * controllers_cli.c -- the controller table, asked from a shell.
 *
 * This is controllers.py's other half in the sibling port: their table is one
 * dict that is imported in-process AND shelled out to, so the mapping, the
 * detection and the install read the same rows. This is the shelled-out half.
 * It links the PRODUCTION controllers.c, so `doctor.sh` and `install.sh` do not
 * keep a second copy of any surface's name -- which is the drift the table
 * exists to remove, and the copy would be in shell, where nothing checks it.
 *
 * The contract is deliberately narrow, because its callers are shell:
 *
 *   - a value goes to stdout, bare, with no label to strip;
 *   - exit 0 means the value was printed;
 *   - exit 1 means the row or the value does not exist (stdout stays empty);
 *   - exit 2 means the command could not be ANSWERED -- a bad argument, or a
 *     machine this tool cannot look at. That is a different thing from "no", and
 *     a caller that collapsed the two would report "no controller attached" on a
 *     machine where /sys simply was not readable.
 *
 * Build (static, so the deploy root needs no runtime for it):
 *     make controllers_cli
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "controllers.h"

static void usage(FILE *f)
{
     fprintf(f,
"usage: controllers_cli <command> [id]\n"
"\n"
"Every value below comes from the controller table (scripts/shims/controllers.c),\n"
"which is the one place a control surface's map, port-name hint, card id and USB\n"
"id are written down. Nothing here keeps a copy.\n"
"\n"
"  detect          the row for each controller plugged into this machine, one\n"
"                  'id name usb' line each; prints 'none' and exits 1 when the\n"
"                  machine was read and no row matched, and exits 2 when it\n"
"                  could not be read at all\n"
"  names           'id name' for every row\n"
"  usbids          'id usb' for every row, '-' where the table records none\n"
"  card-id <id>    the ALSA card id -- the CARD= half of hw:CARD=<id>,DEV=<n>\n"
"  match <id>      the ALSA sequencer port-name substring the surface is found\n"
"                  by (MIDI_IN_MATCH's default for that row)\n"
"\n"
"Exit: 0 printed, 1 no such row or no such value, 2 could not answer.\n");
}

/* Trailing whitespace off a sysfs read. */
static void trim(char *s)
{
     size_t n = strlen(s);

     while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
          s[--n] = '\0';
}

/* Ask the USB ids in sysfs about the table. Returns how many rows matched, or -1
 * when /sys could not be read.
 *
 * USB IS NOT HOW THIS PORT FINDS ITS SURFACE, and that is worth saying where a
 * reader will see it. The shim matches the ALSA sequencer port NAME; the USB id
 * was a documentation fact until it was put in this table. `detect` exists so a
 * diagnostic can compare what is PLUGGED IN against what was SELECTED, which is
 * exactly the shape of drift the table is here to make impossible to miss -- not
 * so anything can start detecting by USB. sysfs is read directly rather than
 * shelling out to lsusb, so this works on a unit where lsusb is not installed. */
static int detect(void)
{
     DIR *d;
     struct dirent *e;
     int printed = 0;

     d = opendir("/sys/bus/usb/devices");
     if (!d) {
          fprintf(stderr, "controllers_cli: cannot read /sys/bus/usb/devices: %s\n",
                  strerror(errno));
          return -1;
     }

     while ((e = readdir(d)) != NULL) {
          char path[512], id[32], v[16], p[16];
          const struct controller *c;
          FILE *f;

          if (e->d_name[0] == '.')
               continue;

          snprintf(path, sizeof path, "/sys/bus/usb/devices/%s/idVendor",
                   e->d_name);
          f = fopen(path, "r");
          if (!f)
               continue;
          if (!fgets(v, sizeof v, f)) { fclose(f); continue; }
          fclose(f);

          snprintf(path, sizeof path, "/sys/bus/usb/devices/%s/idProduct",
                   e->d_name);
          f = fopen(path, "r");
          if (!f)
               continue;
          if (!fgets(p, sizeof p, f)) { fclose(f); continue; }
          fclose(f);

          trim(v);
          trim(p);
          if (!v[0] || !p[0])
               continue;

          snprintf(id, sizeof id, "%s:%s", v, p);
          c = controllers_by_usb(id);
          if (!c)
               continue;

          printf("%s %s %s\n", c->id, c->name, c->usb);
          printed++;
     }
     closedir(d);
     return printed;
}

/* One value out of one row, for the two lookups that differ only in the field.
 * `want_card` picks it; the noun in the stderr line comes from the same flag,
 * because "no such value" with no noun sends the reader to the wrong place. */
static int one_value(const char *id, int want_card)
{
     const struct controller *c = controllers_find(id);
     const char *v;

     if (!c) {
          fprintf(stderr, "controllers_cli: '%s' is not a controller this build "
                  "has\n", id ? id : "(no id)");
          return 1;
     }
     v = want_card ? c->card_id : c->alsa_hint;
     if (!v || !v[0]) {
          fprintf(stderr, "controllers_cli: the table records no %s for '%s'\n",
                  want_card ? "card id" : "port-name hint", c->id);
          return 1;
     }
     printf("%s\n", v);
     return 0;
}

int main(int argc, char **argv)
{
     const char *cmd;

     if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
          usage(argc < 2 ? stderr : stdout);
          return argc < 2 ? 2 : 0;
     }
     cmd = argv[1];

     if (strcmp(cmd, "detect") == 0) {
          int n = detect();

          if (n < 0)
               return 2;
          if (n == 0) {
               printf("none\n");
               return 1;
          }
          return 0;
     }

     if (strcmp(cmd, "names") == 0 || strcmp(cmd, "usbids") == 0) {
          const int usb = strcmp(cmd, "usbids") == 0;
          unsigned i;

          for (i = 0; i < controllers_count(); i++) {
               const struct controller *c = controllers_at(i);

               if (!c)
                    continue;
               if (!usb)
                    printf("%s %s\n", c->id, c->name);
               else
                    printf("%s %s\n", c->id, c->usb ? c->usb : "-");
          }
          return 0;
     }

     if (strcmp(cmd, "card-id") == 0 || strcmp(cmd, "match") == 0) {
          const int card = strcmp(cmd, "card-id") == 0;

          if (argc != 3) {
               fprintf(stderr, "controllers_cli: %s needs exactly one controller "
                       "id\n", cmd);
               return 2;
          }
          return one_value(argv[2], card);
     }

     fprintf(stderr, "controllers_cli: unknown command '%s'\n\n", cmd);
     usage(stderr);
     return 2;
}
