/*
 * usb_devices.c -- see usb_devices.h.
 *
 * The walk is the one controllers_cli.c used to carry inline, lifted here with its
 * behaviour unchanged so that the mixer-presence check and the `detect` diagnostic cannot
 * answer differently about the same bus.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "usb_devices.h"

/* Long enough for a sysfs device path with room to spare, and short enough to be one
 * stack frame. Truncation is checked at every use rather than assumed. */
#define USB_PATH_MAX 512

/* sysfs gives a newline; take it off. */
static void trim_eol(char *s)
{
     size_t n = strlen(s);

     while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
          s[--n] = '\0';
}

/* One attribute of one device, or 0. `out` is emptied on failure, so a caller can test the
 * return value and the string with the same expectation. */
static int read_attr(const char *dir, const char *attr, char *out, size_t outsz)
{
     char path[USB_PATH_MAX];
     FILE *f;

     out[0] = '\0';
     if (snprintf(path, sizeof path, "%s/%s", dir, attr) >= (int)sizeof path)
          return 0;
     f = fopen(path, "r");
     if (!f)
          return 0;
     if (fgets(out, (int)outsz, f) == NULL) {
          fclose(f);
          out[0] = '\0';
          return 0;
     }
     fclose(f);
     trim_eol(out);
     return out[0] != '\0';
}

int usb_table_walk_in(const char *root, usb_match_fn fn, void *ctx)
{
     DIR *d;
     struct dirent *e;
     int found = 0;

     if (!root || !fn)
          return -1;
     d = opendir(root);
     if (!d)
          return -1;

     while ((e = readdir(d)) != NULL) {
          char dir[USB_PATH_MAX], vid[16], pid[16], product[128], usb[16];
          const struct controller *c;

          if (e->d_name[0] == '.')
               continue;
          if (snprintf(dir, sizeof dir, "%s/%s", root, e->d_name) >= (int)sizeof dir)
               continue;
          if (!read_attr(dir, "idVendor", vid, sizeof vid))
               continue;
          if (!read_attr(dir, "idProduct", pid, sizeof pid))
               continue;
          if (snprintf(usb, sizeof usb, "%s:%s", vid, pid) >= (int)sizeof usb)
               continue;

          /* The id is the first ask, and it is matched rather than case-folded: sysfs
           * prints lowercase hex, which is controllers.h's spelling, and a silent fold
           * here would hide a source that changed its case. */
          c = controllers_by_usb(usb);
          /* Then the model name, which is the only signal for a mixer whose id is not
           * recorded (see the mixer rows in controllers.c). Surfaces record no product
           * token, so this can never reach one -- controllers_mixer_by_product filters on
           * the mixer flag. */
          if (!c && read_attr(dir, "product", product, sizeof product))
               c = controllers_mixer_by_product(product);
          if (!c)
               continue;

          found++;
          if (fn(c, ctx))
               break;
     }
     closedir(d);
     return found;
}

struct first_mixer {
     const struct controller *found;
};

static int mixer_visit(const struct controller *c, void *ctx)
{
     struct first_mixer *m = ctx;

     /* A surface is not what this is for, so it neither stops the walk nor is reported --
        a unit with an FLX4 and no mixer is exactly the case the caller must be able to
        tell apart from "a mixer is here". */
     if (!controller_is_mixer(c))
          return 0;
     m->found = c;
     return 1;                    /* stop at the first one */
}

const struct controller *mixer_present_in(const char *root)
{
     struct first_mixer m;

     m.found = NULL;
     if (usb_table_walk_in(root, mixer_visit, &m) < 0)
          return NULL;
     return m.found;
}

const struct controller *mixer_present(void)
{
     return mixer_present_in(USB_DEVICES_ROOT);
}
