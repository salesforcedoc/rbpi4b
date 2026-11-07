/* Frame-boundary compositor for the optional full-touch controls. The primary
 * is logical 1280x800, BEFORE the Pi's depth conversion/rotation/scaling. Do not
 * interpret /dev/fb0 or consume DRM planes here. No dependency on knobshim.
 */
#define _GNU_SOURCE
#include "full_ui.h"
#include "rbp_abi.h"
#include "rbp_key.h"
#include "pointsrc.h"
#include "menu_zone.h"
#include "side_zone.h"
#include "fx_zone.h"
#include "settings_zone.h"
#include "envutil.h"
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* A slot's byte offset is for the 32-bit firmware ABI; spelling it as an index
 * also lets the production compositor run against a host mock interface. */
#define SLOT(p, off) (((void **)(p))[(off)/4])
typedef int (*flip_fn)(void *, const void *, int);
typedef int (*size_fn)(void *, int *, int *);
typedef int (*format_fn)(void *, unsigned int *);
typedef int (*lock_fn)(void *, int, void **, int *);
typedef int (*unlock_fn)(void *);
static pthread_mutex_t ui_lock = PTHREAD_MUTEX_INITIALIZER;
static flip_fn saved_flip;
static void *surface;
static int started, ready;
static int announced_bpp;
static struct fu_state gesture;
static struct fu_view displayed;

/* Private seam for mock tests. All firmware reads live here; the renderer and
 * input state machine never call a firmware function or read a fixed address. */
/* Weak only so the runtime test can replace firmware memory with a snapshot;
 * production links the implementation here, with no external dependency. */
__attribute__((weak)) void full_ui_read_view(struct fu_view *v)
{
    uintptr_t engine, begin, end;
    int deck,k;
    memset(v,0,sizeof *v);
    v->mode = *(volatile int *)UIBROWSE_GLOBAL;
    v->deck_visible = v->mode==1 || v->mode==2 || v->mode==5;
    for(k=0;k<6;k++) v->lit[k]=-1;
    /* Leave native touch-calibration targets alone. MY SETTINGS is moved
     * below our header, using guarded layout records at startup. */
    if (v->mode==7 && *(volatile int *)(UIBROWSE_GLOBAL+UIBROWSE_OFF_CALIB_A) &&
        *(volatile int *)(UIBROWSE_GLOBAL+UIBROWSE_OFF_CALIB_B)) v->mode=0;
    engine=*(volatile uint32_t *)PLAYENGINE_GLOBAL;
    if (!engine) return;
    begin=*(volatile uint32_t *)(engine+PE_PLAYERS_BEGIN_OFF);
    end=*(volatile uint32_t *)(engine+PE_PLAYERS_END_OFF);
    if (!begin || end<begin || end-begin<8) return;
    for(deck=0;deck<2;deck++) {
        uintptr_t player=*(volatile uint32_t *)(begin+4*deck);
        if (!player) continue;
        v->lit[deck*3]=!!*(volatile unsigned char *)(player+PE_PLAYER_VINYL_OFF);
        v->lit[deck*3+1]=!!(*(volatile unsigned char *)(player+PE_PLAYER_SLIP_OFF)&1);
        v->lit[deck*3+2]=!!*(volatile unsigned char *)(player+PE_PLAYER_MT_OFF);
    }
}

static int modal(void)
{ return menu_is_open() || side_any_open() || settings_is_open() || fxlist_is_open(); }

__attribute__((weak)) void full_ui_redraw(int deck)
{
    int mode=*(volatile int *)UIBROWSE_GLOBAL;
    if (deck<0) {
        /* Full redraw is safe on SEARCH. On the deck screen it can erase the
         * native timer/header, so only refresh the title group there. */
        if (mode==14) ((void (*)(void))UI_REFRESH_WS)();
    } else if (mode==1 || mode==2 || mode==5) {
        ((void (*)(void *,int))UI_REFRESH_OBJECT)(NULL,deck?UI_TITLE_GROUP2:UI_TITLE_GROUP1);
    }
}

static int compose_flip(void *self, const void *region, int flags)
{
    struct fu_view v;
    void *pixels=NULL;
    unsigned int format=0;
    int w=0,h=0,pitch=0,bpp=0,painted=0,compatible=0,result;
    flip_fn next;
    full_ui_read_view(&v);
    pthread_mutex_lock(&ui_lock);
    next=saved_flip;
    if (self==surface && !((size_fn)SLOT(self,UI_SURF_SIZE_OFF))(self,&w,&h) &&
        !((format_fn)SLOT(self,UI_SURF_FORMAT_OFF))(self,&format)) {
        /* Exact DirectFB 1.4 formats: RGB16, RGB32 and ARGB. Never infer
         * layout from screen colours or confuse RGB555 with RGB565. */
        if (format==0x00200801u) bpp=16;
        if (format==0x00400c03u || format==0x00418c04u) bpp=32;
        compatible=bpp && w==1280 && h==800;
        if (modal()) v.mode=0;
        if (gesture.owned && gesture.mode!=v.mode) gesture.cancelled=1;
        if (gesture.mode!=v.mode) gesture.dropdown=0;
        v.dropdown=gesture.dropdown;
        v.pressed=gesture.owned && !gesture.cancelled ? gesture.anchor : 0;
        if (compatible &&
            !((lock_fn)SLOT(self,UI_SURF_LOCK_OFF))(self,2,&pixels,&pitch)) {
            painted=fu_paint(pixels,w,h,pitch,bpp,&v);
            ((unlock_fn)SLOT(self,UI_SURF_UNLOCK_OFF))(self);
        }
    }
    /* A transient Lock/Flip error does not erase the already painted primary.
     * Dropping ready on every such error made visible controls untappable and
     * flooded the log with a new "presenting" line on every recovered frame.
     * Keep the last painted hit map only while geometry and native screen agree. */
    if (painted && v.mode>0) {
        if (announced_bpp!=bpp) {
            pointsrc_log("full ui: presenting navigation and deck controls (%d bpp)",bpp);
            announced_bpp=bpp;
        }
        ready=1;
        displayed=v;
    } else if (!compatible || !v.mode || displayed.mode!=v.mode ||
               displayed.deck_visible!=v.deck_visible) {
        ready=0;
        displayed.mode=0;
    }
    pthread_mutex_unlock(&ui_lock);
    result=next(self,region,flags);
    /* Preserve the native return code, but don't treat it as an instruction
     * to discard controls already drawn into the persistent primary surface. */
    return result;
}

int full_ui_attach(void *primary)
{
    int w=0,h=0;
    if (!primary || !SLOT(primary,UI_SURF_SIZE_OFF) || !SLOT(primary,UI_SURF_FORMAT_OFF) ||
        !SLOT(primary,UI_SURF_LOCK_OFF) || !SLOT(primary,UI_SURF_UNLOCK_OFF) || !SLOT(primary,UI_SURF_FLIP_OFF)) return 0;
    if (((size_fn)SLOT(primary,UI_SURF_SIZE_OFF))(primary,&w,&h) || w!=1280 || h!=800) return 0;
    pthread_mutex_lock(&ui_lock);
    if (!surface) {
        saved_flip=(flip_fn)SLOT(primary,UI_SURF_FLIP_OFF);
        surface=primary;
        started=1;
        __atomic_store_n(&SLOT(primary,UI_SURF_FLIP_OFF),(void *)compose_flip,__ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&ui_lock);
    return surface==primary;
}

int full_ui_touch(int down, int x, int y)
{
    struct fu_view v,live;
    struct fu_action a;
    struct timespec ts;
    int take,dropdown_changed;
    if (!started) return 0;
    full_ui_read_view(&live);
    clock_gettime(CLOCK_MONOTONIC,&ts);
    pthread_mutex_lock(&ui_lock);
    v=displayed;
    if (!ready || modal() || live.mode!=v.mode ||
        live.deck_visible!=v.deck_visible) v.mode=0;
    dropdown_changed=gesture.dropdown;
    take=fu_feed(&gesture,&v,down,x,y,(unsigned long long)ts.tv_sec*1000+ts.tv_nsec/1000000,&a);
    dropdown_changed=dropdown_changed!=gesture.dropdown;
    pthread_mutex_unlock(&ui_lock);
    if (a.key) {
        keylog_from("touch");
        pointsrc_log("full ui: key %#x channel %d%s",a.key,a.channel,a.hold?" held (UTILITY)":"");
        send_rx_key(a.key,OP_PRESS,a.channel,0);
        if (a.hold) usleep(500000); /* native MENU's verified hold threshold */
        else if (a.key==K_VINYL || a.key==K_SLIP || a.key==K_MT)
            usleep(80000); /* reference UI's distinct physical key edges */
        send_rx_key(a.key,OP_RELEASE,a.channel,0);
    }
    if (dropdown_changed) full_ui_redraw(-1);
    return take;
}

void full_ui_reset(void)
{
    int redraw;
    pthread_mutex_lock(&ui_lock);
    redraw=gesture.dropdown;
    memset(&gesture,0,sizeof gesture);
    pthread_mutex_unlock(&ui_lock);
    if (redraw) full_ui_redraw(-1);
}

static void *attach_thread(void *unused)
{
    int i;
    (void)unused;
    for(i=0;i<600;i++) {
        uintptr_t ds=*(volatile uint32_t *)(UI_DS_LAYER0_CTX+UI_DS_PRIMARY_OFF);
        void *primary=ds ? (void *)(uintptr_t)*(volatile uint32_t *)(ds+UI_DS_IFACE_OFF) : NULL;
        if (primary && full_ui_attach(primary)) {
            struct fu_view previous={0},v;
            pointsrc_log("full ui: primary Flip hooked");
            /* rbp can leave an idle deck's title undamaged indefinitely.
             * Observe native flags even when there are no Flip calls, and queue
             * a small native redraw only when a deck's three states change. */
            for (;;) {
                int deck;
                full_ui_read_view(&v);
                for(deck=0;deck<2;deck++)
                    if (v.deck_visible && (previous.mode!=v.mode ||
                        memcmp(v.lit+3*deck,previous.lit+3*deck,3*sizeof(int))))
                        full_ui_redraw(deck);
                previous=v;
                usleep(100000);
            }
        }
        usleep(100000);
    }
    pointsrc_log("full ui: no logical primary after 60s; controls disabled");
    return NULL;
}

void full_ui_start(void)
{
    pthread_t tid;
    uint32_t *r, *title, *count;
    if (started || !env_flag("POINT_FULL_UI",0) || !is_rbp_process()) return;
    /* Match all original words before moving firmware 1.20 layout records.
     * Run at tsc open, before rbp builds its windows. Same placement as the
     * reference UI: popup below navigation, filter title/count before x334. */
    r=(uint32_t *)UI_MENU_LAYOUT_REC;
    title=(uint32_t *)UI_FILTER_TITLE_REC;
    count=(uint32_t *)UI_FILTER_COUNT_REC;
    if (!fu_prepare_layout(r,title,count)) {
        pointsrc_log("full ui: firmware layout guards failed; controls disabled");
        return;
    }
    started=1;
    if (pthread_create(&tid,NULL,attach_thread,NULL)) {
        r[2]-=48u<<16; r[3]-=48u<<16;
        title[2]+=6; title[3]+=6; count[2]+=30;
        started=0;
        pointsrc_log("full ui: cannot start primary hook thread");
    } else pthread_detach(tid);
}
