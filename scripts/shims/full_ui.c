#include "full_ui.h"
#include "rbp_abi.h"
#include "menu_font.h"
#include "gear_icon.h"
#include <stddef.h>
#include <string.h>

static int deck_screen(int mode) { return mode == 1 || mode == 2 || mode == 5; }

int fu_prepare_layout(uint32_t *menu, uint32_t *title, uint32_t *count)
{
    if (!menu || !title || !count ||
        menu[0]!=17 || menu[2]!=0x000002c8 || menu[3]!=0x01d20500 ||
        title[0]!=0 || title[2]!=0x000e0074 || title[3]!=0x00300118 ||
        count[0]!=0 || count[2]!=0x000e0120 || count[3]!=0x003003b6) return 0;
    menu[2]+=48u<<16; menu[3]+=48u<<16;
    title[2]-=6; title[3]-=6; count[2]-=30;
    return 1;
}

const char *fu_label(int id)
{
    static const char *const labels[FU_COUNT] = {
        "", "BACK", "SOURCE", "BROWSE", "TAG LIST", "PLAYLIST", "SEARCH",
        "MENU", "SHORTCUT", "MENU", "VINYL", "SLIP", "MT", "VINYL", "SLIP", "MT"
    };
    return id > 0 && id < FU_COUNT ? labels[id] : "";
}

int fu_key(int id, int *channel)
{
    static const int keys[] = { 0, K_BACK, K_SOURCE, K_BROWSE, K_TAGLIST,
        K_PLAYLIST, K_SEARCH, K_MENU, K_SHORTCUT };
    *channel = CH_GLOBAL;
    if (id >= FU_VINYL1 && id <= FU_MT2) {
        static const int deck_keys[] = { K_VINYL, K_SLIP, K_MT };
        *channel = 1 + (id - FU_VINYL1) / 3;
        return deck_keys[(id - FU_VINYL1) % 3];
    }
    return id > 0 && id < (int)(sizeof keys / sizeof keys[0]) ? keys[id] : 0;
}

int fu_rect(int mode, int dropdown, int id, struct fu_rect *r)
{
    int x, i, w, narrow;
    if (mode <= 0 || mode > 14) return 0;
    if (id >= FU_VINYL1 && id <= FU_MT2) {
        if (!deck_screen(mode)) return 0;
        r->x = 426 + ((id - FU_VINYL1) % 3) * 68 + ((id - FU_VINYL1) / 3) * 640;
        r->y = 591; r->w = 62; r->h = 36;
        return 1;
    }
    if (mode == 14) {
        if (id == FU_TOGGLE) {
            *r = (struct fu_rect){1075, 4, 95, 36};
            return 1;
        }
        if (!dropdown || id < FU_BACK || id > FU_SHORTCUT) return 0;
        *r = (struct fu_rect){1075, 46 + (id - FU_BACK) * 46, 195, 40};
        return 1;
    }
    if (id < FU_BACK || id > FU_SHORTCUT) return 0;
    /* Retain the native countdown/record timers and INFO. Browse headers have
     * less space: three 40px icons and five 92px labels fit before x970. */
    narrow = !deck_screen(mode);
    x = narrow ? 334 : 188;
    for (i = FU_BACK; i <= id; i++) {
        w = (i == FU_BACK || i == FU_MENU || i == FU_SHORTCUT) ? 40 : (narrow ? 92 : 140);
        if (i == id) { *r = (struct fu_rect){x, narrow ? 5 : 2, w, 40}; return 1; }
        x += w + 6;
    }
    return 0;
}

static int inside(const struct fu_rect *r, int x, int y)
{ return x >= r->x && x < r->x+r->w && y >= r->y && y < r->y+r->h; }

int fu_hit(const struct fu_view *v, int x, int y)
{
    int id;
    struct fu_rect r;
    for (id = FU_BACK; id < FU_COUNT; id++) {
        if (id >= FU_VINYL1 && !v->deck_visible) continue;
        if (!fu_rect(v->mode, v->dropdown, id, &r)) continue;
        if (id >= FU_VINYL1) {
            /* The 36px painted title-row switches need a finger-sized target.
             * Stop below the pad grid (last row 567) and above tempo controls
             * (638), and share each 6px horizontal gap without overlap. */
            r.x -= 3; r.w += 6;
            r.y = 568; r.h = 70;
        }
        if (inside(&r,x,y)) return id;
    }
    return 0;
}

int fu_feed(struct fu_state *s, const struct fu_view *v, int down, int x, int y,
            unsigned long long now, struct fu_action *a)
{
    struct fu_view hit_view = *v;
    int hit, take;
    memset(a, 0, sizeof *a);
    if (s->mode != v->mode) s->dropdown = 0;
    hit_view.dropdown = s->dropdown;
    hit = fu_hit(&hit_view, x, y);
    if (down && !s->down) {
        s->anchor = hit;
        s->owned = hit != 0 || s->dropdown;
        s->cancelled = 0;
        s->since = now;
        s->mode = v->mode;
    }
    if (s->down && (s->mode != v->mode ||
        (hit != s->anchor && s->anchor < FU_VINYL1)))
        s->cancelled = 1; /* no re-arm after sliding out and back */
    take = s->owned;
    if (!down && s->down && take) {
        if (!s->cancelled && hit && hit == s->anchor) {
            if (hit == FU_TOGGLE) s->dropdown = !s->dropdown;
            else {
                a->key = fu_key(hit, &a->channel);
                /* Native BACK navigates inside lists. At known top-level
                 * panels their own selector key toggles back to the decks. */
                if (hit == FU_BACK) {
                    switch (v->mode) {
                    case 4: a->key=K_TAGLIST; break;
                    case 5: a->key=K_INFO; break;
                    case 7: a->key=K_MENU; break;
                    case 10: a->key=K_SHORTCUT; break;
                    case 12: a->key=K_SOURCE; break;
                    }
                }
                a->hold = hit == FU_MENU && now >= s->since && now - s->since >= FU_HOLD_MS;
                s->dropdown = 0;
            }
        } else s->dropdown = 0;
    }
    s->down = !!down;
    if (!down) { s->owned = 0; s->anchor = 0; }
    return take;
}

/* Draw only our rectangles. The surrounding pixels, including native title
 * text and toolkit tabs, are not used as a background or format detector. */
struct canvas { unsigned char *p; int pitch, bpp; };
static void pixel(struct canvas *c, int x, int y, uint32_t rgb)
{
    if (c->bpp == 16)
        ((uint16_t *)(c->p + y*c->pitch))[x] = ((rgb>>8)&0xf800) | ((rgb>>5)&0x7e0) | ((rgb>>3)&31);
    else ((uint32_t *)(c->p + y*c->pitch))[x] = 0xff000000u | rgb;
}
static uint32_t blend(uint32_t bg, uint32_t fg, int a)
{
    uint32_t out = 0;
    int shift;
    for (shift=0; shift<24; shift+=8)
        out |= (((((bg>>shift)&255)*(255-a) + ((fg>>shift)&255)*a)/255) << shift);
    return out;
}
static void label(struct canvas *c, struct fu_rect r, const char *s, uint32_t bg, uint32_t ink)
{
    int pen = r.x + (r.w-menu_text_width(s))/2;
    int top = r.y + (r.h-MENU_FONT_LINE)/2;
    for (; *s; s++) {
        const struct menu_glyph *g = menu_font_glyph((unsigned char)*s);
        int x,y;
        for (y=0; y<g->h; y++) for (x=0; x<g->w; x++) {
            int px=pen+g->left+x, py=top+g->top+y;
            if (inside(&r,px,py)) pixel(c,px,py,blend(bg,ink,menu_font_cov(g,x,y)));
        }
        pen += menu_font_adv((unsigned char)*s,(unsigned char)s[1]);
    }
}
static void icon(struct canvas *c, struct fu_rect r, int id, uint32_t bg, uint32_t ink)
{
    int x,y,cx=r.x+r.w/2,cy=r.y+r.h/2;
    if (id == FU_MENU) {
        const struct gear_icon *g = &gear_icons[0];
        for (y=0;y<g->h;y++) for(x=0;x<g->w;x++)
            pixel(c,cx-g->w/2+x,cy-g->h/2+y,blend(bg,ink,g->cov[y*g->w+x]));
    } else if (id == FU_BACK) {
        for(y=-9;y<=9;y++) for(x=0;x<3;x++) pixel(c,cx-4+(y<0?-y:y)+x,cy+y,ink);
    } else { /* lightning bolt; two offset triangles */
        for(y=-12;y<=12;y++) for(x=-8;x<=8;x++)
            if ((y<=0 && x>=-y/2-6 && x<=4) || (y>0 && x>=-3 && x<=6-y/2))
                pixel(c,cx+x,cy+y,ink);
    }
}
int fu_paint(void *pixels, int width, int height, int pitch, int bpp, const struct fu_view *v)
{
    struct canvas c;
    struct fu_rect r;
    int id,x,y;
    if (!pixels || width!=1280 || height!=800 || (bpp!=16 && bpp!=32) || pitch<width*(bpp/8)) return 0;
    c = (struct canvas){pixels,pitch,bpp};
    for(id=FU_BACK;id<FU_COUNT;id++) {
        uint32_t bg=0x2e2e2e, ink=0xe6e6e6;
        if (id>=FU_VINYL1 && !v->deck_visible) continue;
        if (!fu_rect(v->mode,v->dropdown,id,&r)) continue;
        if (id==v->pressed || (id>=FU_VINYL1 && v->lit[id-FU_VINYL1]>0)) bg=0x105ab4;
        if (id>=FU_VINYL1 && v->lit[id-FU_VINYL1]<0) ink=0x777777;
        for(y=r.y;y<r.y+r.h;y++) for(x=r.x;x<r.x+r.w;x++)
            pixel(&c,x,y,(x==r.x || x==r.x+r.w-1 || y==r.y || y==r.y+r.h-1)?0x5a5a5a:bg);
        if (id==FU_BACK || id==FU_MENU || id==FU_SHORTCUT) icon(&c,r,id,bg,ink);
        else label(&c,r,fu_label(id),bg,ink);
    }
    return 1;
}
