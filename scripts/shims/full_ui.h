/* Optional persistent RX3 navigation. Geometry, input and paint are pure;
 * full_ui_runtime.c connects them to rbp/DirectFB. Logical space is 1280x800.
 * Layout informed by salesforcedoc/full-touch-ui-xdj-rx3's uishim-rx3.c.
 */
#ifndef RBPI4B_FULL_UI_H
#define RBPI4B_FULL_UI_H

#include <stdint.h>

enum {
    FU_NONE, FU_BACK, FU_SOURCE, FU_BROWSE, FU_TAGLIST, FU_PLAYLIST,
    FU_SEARCH, FU_MENU, FU_SHORTCUT, FU_TOGGLE,
    FU_VINYL1, FU_SLIP1, FU_MT1, FU_VINYL2, FU_SLIP2, FU_MT2,
    FU_COUNT
};
#define FU_HOLD_MS 350
struct fu_rect { int x, y, w, h; };
struct fu_state {
    int down, owned, anchor, cancelled, mode, dropdown;
    unsigned long long since;
};
struct fu_action { int key, channel, hold; };
/* A negative state means unavailable. deck_visible gates title-row switches. */
struct fu_view { int mode, dropdown, pressed, deck_visible; int lit[6]; };

int fu_rect(int mode, int dropdown, int id, struct fu_rect *r);
const char *fu_label(int id);
int fu_key(int id, int *channel);
int fu_hit(const struct fu_view *v, int x, int y);
int fu_prepare_layout(uint32_t *menu, uint32_t *title, uint32_t *count);
/* Captures whole presses, including cancellations, dropdown dismissals, and
 * screen changes. A native press starting outside our controls never migrates in. */
int fu_feed(struct fu_state *s, const struct fu_view *v, int down, int x, int y,
            unsigned long long now, struct fu_action *a);
/* RGB565 (16) or X/ARGB8888 (32); pitch is BYTES. Other surfaces are refused. */
int fu_paint(void *pixels, int width, int height, int pitch, int bpp,
             const struct fu_view *v);

void full_ui_start(void);
int full_ui_touch(int down, int x, int y);
void full_ui_reset(void);
/* Shared with the runtime mock test: install on an already validated primary. */
int full_ui_attach(void *primary);
#endif
