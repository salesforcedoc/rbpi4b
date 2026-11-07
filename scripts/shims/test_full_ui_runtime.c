/* Exercise the production Flip hook with a fake DirectFB primary. */
#include "full_ui.h"
#include "rbp_abi.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void *iface[17];
static unsigned char pixels[1280*800*4];
static struct fu_view firmware={.mode=1,.deck_visible=1};
static unsigned int format=0x00200801;
static int locked,lock_fail,flip_result,popup,flips,sends,presentations;
static int last_key,last_op,last_ch;
static const void *region_arg;
static int flags_arg;
void full_ui_read_view(struct fu_view *v) { *v=firmware; }
void full_ui_redraw(int deck) { (void)deck; }
int menu_is_open(void) { return popup; }
int side_any_open(void) { return 0; }
int settings_is_open(void) { return 0; }
int fxlist_is_open(void) { return 0; }
int is_rbp_process(void) { return 0; }
void pointsrc_log(const char *fmt,...)
{ if (strstr(fmt,"presenting navigation")) presentations++; }
void keylog_from(const char *s) { (void)s; }
void send_rx_key(int key,int op,int ch,long param)
{ last_key=key;last_op=op;last_ch=ch;sends++;(void)param; }
static int get_size(void *p,int *w,int *h)
{ assert(p==iface);*w=1280;*h=800;return 0; }
static int get_format(void *p,unsigned int *f)
{ assert(p==iface);*f=format;return 0; }
static int lock_surface(void *p,int flags,void **out,int *pitch)
{
    assert(p==iface && flags==2 && !locked);
    if(lock_fail) return -1;
    locked=1;*out=pixels;*pitch=1280*(format==0x00200801?2:4);return 0;
}
static int unlock_surface(void *p)
{ assert(p==iface && locked);locked=0;return 0; }
static int flip(void *p,const void *r,int flags)
{
    assert(p==iface && !locked);
    region_arg=r;flags_arg=flags;flips++;
    return flip_result;
}
static void frame(void)
{
    int marker=0;
    memset(pixels,0xa5,sizeof pixels);
    assert(((int (*)(void *,const void *,int))iface[16])(iface,&marker,123)==flip_result);
    assert(region_arg==&marker && flags_arg==123);
}
static void tap(int id)
{
    struct fu_rect r;
    assert(fu_rect(firmware.mode,0,id,&r));
    assert(full_ui_touch(1,r.x+10,r.y+10));
    assert(full_ui_touch(0,r.x+10,r.y+10));
}
int main(void)
{
    iface[6]=(void *)get_size;iface[8]=(void *)get_format;
    iface[13]=(void *)lock_surface;iface[15]=(void *)unlock_surface;iface[16]=(void *)flip;
    assert(!full_ui_touch(1,250,20)); /* off by default */
    assert(!full_ui_attach(NULL));
    assert(full_ui_attach(iface));assert(full_ui_attach(iface));
    assert(!full_ui_touch(1,250,20)); /* hook installed, no painted frame yet */
    full_ui_reset();
    frame();
    assert(((uint16_t *)pixels)[2*1280+188]==0x5acb); /* BACK border before real Flip */
    tap(FU_SOURCE); assert(sends==2 && last_key==K_SOURCE && last_op==OP_RELEASE && last_ch==1);
    tap(FU_MT2); assert(sends==4 && last_key==K_MT && last_ch==2);
    popup=1;frame();
    assert(((uint16_t *)pixels)[2*1280+188]==0xa5a5);
    assert(!full_ui_touch(1,200,20));assert(!full_ui_touch(0,200,20));
    popup=0;format=0x00400c03;frame();
    assert(((uint32_t *)pixels)[2*1280+188]==0xff5a5a5a);
    format=0x00418c04;frame();
    assert(((uint32_t *)pixels)[2*1280+188]==0xff5a5a5a);
    format=0x00200800;frame(); /* RGB555 refused, not treated as RGB565 */
    assert(!full_ui_touch(1,200,20));assert(!full_ui_touch(0,200,20));
    format=0x00200801;lock_fail=1;frame();
    assert(!full_ui_touch(1,200,20));assert(!full_ui_touch(0,200,20));
    lock_fail=0;frame();
    assert(full_ui_touch(1,200,20));
    firmware.mode=3;firmware.deck_visible=0;frame();
    assert(full_ui_touch(0,200,20));assert(sends==4); /* screen changed under held BACK */
    firmware.mode=1;firmware.deck_visible=1;frame();
    assert(full_ui_touch(1,200,20));full_ui_reset();
    assert(!full_ui_touch(0,200,20));assert(sends==4); /* disconnect releases no key */
    flip_result=-17;frame();
    tap(FU_SOURCE);assert(sends==6); /* painted controls survive native Flip errors */
    flip_result=0;frame();
    tap(FU_SOURCE);assert(sends==8);
    assert(flips==11);
    {
        int logged=presentations, sent=sends;
        struct fu_rect r;
        fu_rect(1,0,FU_SLIP1,&r);
        assert(full_ui_touch(1,r.x+10,r.y+10));
        lock_fail=1;frame(); /* skipped repaint during an owned deck tap */
        assert(full_ui_touch(0,r.x+10,r.y+10));
        assert(sends==sent+2 && last_key==K_SLIP && last_ch==1);
        tap(FU_MT2);assert(sends==sent+4 && last_key==K_MT && last_ch==2);
        lock_fail=0;flip_result=-17;
        for(int i=0;i<20;i++) frame();
        tap(FU_SHORTCUT);assert(sends==sent+6 && last_key==K_SHORTCUT);
        assert(presentations==logged); /* no per-frame recovery log flood */
        flip_result=0;
        /* Native screen changes invalidate a touch even BEFORE the next Flip. */
        assert(full_ui_touch(1,r.x+10,r.y+10));
        firmware.mode=3;firmware.deck_visible=0;
        assert(full_ui_touch(0,r.x+10,r.y+10));
        assert(sends==sent+6);
        lock_fail=1;frame();
        assert(!full_ui_touch(1,350,20));assert(!full_ui_touch(0,350,20));
        lock_fail=0;frame();
        tap(FU_SOURCE);assert(sends==sent+8);
        /* Opening a modal without a Flip also cancels captured navigation. */
        assert(full_ui_touch(1,350,20));
        popup=1;
        assert(full_ui_touch(0,350,20));assert(sends==sent+8);
        popup=0;
    }
    puts("full ui runtime: frame hook, formats, modal gating, input and recovery passed");
    return 0;
}
