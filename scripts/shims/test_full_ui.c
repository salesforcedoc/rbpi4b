/* Production geometry/gesture/painter: no firmware or device needed. */
#include "full_ui.h"
#include "rbp_abi.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct fu_view view(int mode)
{
    struct fu_view v;
    memset(&v,0,sizeof v);
    v.mode=mode; v.deck_visible=mode==1 || mode==2 || mode==5;
    return v;
}
static void tap(struct fu_state *s, struct fu_view *v, int id, int ms,
                struct fu_action *a)
{
    struct fu_rect r;
    assert(fu_rect(v->mode,s->dropdown,id,&r));
    assert(fu_feed(s,v,1,r.x+r.w/2,r.y+r.h/2,100,a));
    assert(!a->key);
    assert(fu_feed(s,v,0,r.x+r.w/2,r.y+r.h/2,100+ms,a));
}
static void geometry(void)
{
    int mode,id,other,ch;
    for(mode=1;mode<=14;mode++) for(id=1;id<FU_COUNT;id++) {
        struct fu_rect r,t;
        struct fu_view v=view(mode);
        v.dropdown=1;
        if(!fu_rect(mode,1,id,&r)) continue;
        assert(r.x>=0 && r.y>=0 && r.x+r.w<=1280 && r.y+r.h<=800);
        assert(fu_hit(&v,r.x+r.w/2,r.y+r.h/2)==id);
        for(other=id+1;other<FU_COUNT;other++) if(fu_rect(mode,1,other,&t))
            assert(r.x+r.w<=t.x || t.x+t.w<=r.x || r.y+r.h<=t.y || t.y+t.h<=r.y);
    }
    assert(fu_key(FU_VINYL1,&ch)==K_VINYL && ch==1);
    assert(fu_key(FU_SLIP2,&ch)==K_SLIP && ch==2);
    assert(fu_key(FU_MT2,&ch)==K_MT && ch==2);
    assert(fu_key(FU_SHORTCUT,&ch)==K_SHORTCUT && ch==CH_GLOBAL);
}
static void layout_guards(void)
{
    uint32_t menu[]={17,0,0x000002c8,0x01d20500};
    uint32_t title[]={0,1,0x000e0074,0x00300118};
    uint32_t count[]={0,1,0x000e0120,0x003003b6};
    title[2]++;
    assert(!fu_prepare_layout(menu,title,count));
    assert(menu[2]==0x000002c8 && count[2]==0x000e0120);
    title[2]--;
    assert(fu_prepare_layout(menu,title,count));
    assert(menu[2]==0x003002c8 && menu[3]==0x02020500);
    assert(title[2]==0x000e006e && title[3]==0x00300112 && count[2]==0x000e0102);
    assert(!fu_prepare_layout(menu,title,count));
}
static void gestures(void)
{
    struct fu_state s={0};
    struct fu_view v=view(1);
    struct fu_action a;
    struct fu_rect r;
    int id,ch;
    for(id=FU_BACK;id<FU_COUNT;id++) {
        if(id==FU_TOGGLE) continue;
        tap(&s,&v,id,80,&a);
        assert(a.key==fu_key(id,&ch) && a.channel==ch && !a.hold);
    }
    tap(&s,&v,FU_MENU,FU_HOLD_MS-1,&a); assert(!a.hold);
    tap(&s,&v,FU_MENU,FU_HOLD_MS,&a); assert(a.hold && a.key==K_MENU);
    /* Drift off and back cannot activate anything. */
    assert(fu_rect(1,0,FU_SOURCE,&r));
    assert(fu_feed(&s,&v,1,r.x+8,r.y+8,0,&a));
    assert(fu_feed(&s,&v,1,0,400,20,&a));
    assert(fu_feed(&s,&v,0,r.x+8,r.y+8,40,&a)); assert(!a.key);
    /* Native press dragged onto a control keeps both native edges. */
    assert(!fu_feed(&s,&v,1,300,400,0,&a));
    assert(!fu_feed(&s,&v,1,r.x+8,r.y+8,20,&a));
    assert(!fu_feed(&s,&v,0,r.x+8,r.y+8,40,&a)); assert(!a.key);
    /* A screen change or newly raised modal cancels a captured press. */
    assert(fu_feed(&s,&v,1,r.x+8,r.y+8,0,&a));
    v.mode=3;
    assert(fu_feed(&s,&v,0,r.x+8,r.y+8,40,&a)); assert(!a.key);
    v=view(1);
    assert(fu_feed(&s,&v,1,r.x+8,r.y+8,0,&a));
    v.mode=0;
    assert(fu_feed(&s,&v,0,r.x+8,r.y+8,40,&a)); assert(!a.key);
    /* SEARCH dropdown: opening lift does not activate a row; outside
     * dismissal consumes both edges and never reaches the keyboard. */
    v=view(14);
    tap(&s,&v,FU_TOGGLE,60,&a); assert(s.dropdown && !a.key);
    assert(fu_feed(&s,&v,1,100,500,100,&a));
    assert(fu_feed(&s,&v,0,100,500,150,&a)); assert(!a.key && !s.dropdown);
    tap(&s,&v,FU_TOGGLE,60,&a);
    tap(&s,&v,FU_BROWSE,60,&a); assert(a.key==K_BROWSE && !s.dropdown);
    /* Deck buttons are absent on browsing/search screens. */
    assert(!fu_hit(&v,450,600));
    v=view(12); tap(&s,&v,FU_BACK,60,&a); assert(a.key==K_SOURCE);
    v=view(5); tap(&s,&v,FU_BACK,60,&a); assert(a.key==K_INFO);
    /* Deck title targets extend beyond their paint and tolerate a brief
     * excursion, but a lift on a different control still fires nothing. */
    v=view(1);
    assert(fu_hit(&v,425,575)==FU_VINYL1);
    assert(fu_hit(&v,492,637)==FU_SLIP1);
    assert(!fu_hit(&v,450,567) && !fu_hit(&v,450,638));
    assert(fu_feed(&s,&v,1,430,580,0,&a));
    assert(fu_feed(&s,&v,1,430,565,10,&a));
    assert(fu_feed(&s,&v,0,432,582,50,&a)); assert(a.key==K_VINYL);
    assert(fu_feed(&s,&v,1,430,580,0,&a));
    assert(fu_feed(&s,&v,0,500,580,50,&a)); assert(!a.key);
}
static void painting(void)
{
    int bpp,mode;
    for(bpp=16;bpp<=32;bpp+=16) for(mode=0;mode<=14;mode++) {
        int stride=1280*(bpp/8)+16, x,y;
        size_t len=(size_t)stride*800;
        unsigned char *buf=malloc(len+32);
        struct fu_view v=view(mode);
        assert(buf);
        memset(buf,0xa5,len+32);
        v.dropdown=1; v.lit[0]=1; v.lit[2]=-1;
        assert(fu_paint(buf+16,1280,800,stride,bpp,&v));
        for(x=0;x<16;x++) assert(buf[x]==0xa5 && buf[len+16+x]==0xa5);
        for(y=0;y<800;y++) {
            for(x=1280*(bpp/8);x<stride;x++) assert(buf[16+y*stride+x]==0xa5);
            for(x=0;x<1280;x++) {
                /* Pixels outside buttons, including the toolkit's right-hand
                 * panels and tabs, must remain byte-identical. */
                if(!fu_hit(&v,x,y)) {
                    int k;
                    for(k=0;k<bpp/8;k++) assert(buf[16+y*stride+x*(bpp/8)+k]==0xa5);
                }
            }
        }
        if(mode==1) {
            struct fu_rect r;
            fu_rect(1,0,FU_VINYL1,&r);
            if(bpp==16) assert(((uint16_t *)(buf+16+(r.y+2)*stride))[r.x+2]==0x12d6);
            else assert(((uint32_t *)(buf+16+(r.y+2)*stride))[r.x+2]==0xff105ab4);
        }
        assert(!fu_paint(buf+16,640,800,stride,bpp,&v));
        assert(!fu_paint(buf+16,1280,800,1,bpp,&v));
        assert(!fu_paint(buf+16,1280,800,stride,24,&v));
        free(buf);
    }
}
int main(void)
{
    layout_guards(); geometry(); gestures(); painting();
    puts("full ui: geometry, gesture ownership, keys and 16/32-bit paint passed");
    return 0;
}
