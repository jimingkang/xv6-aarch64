/* Tiny210 win32k: kernel-owned windows, message queues, GDI batches and a
 * cached compositor.  It follows the rpi3/hdmi implementation but uses a
 * board-reserved arena instead of requiring the later slab allocator. */
#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/memlayout.h"
#include "kernel/aarch64.h"
#include "kernel/spinlock.h"
#include "kernel/sleeplock.h"
#include "kernel/proc.h"
#include "kernel/defs.h"
#include "kernel/workqueue.h"
#include "kernel/input.h"
#include "kernel/module.h"
#include "kernel/win32k.h"
#include "kernel/drivers/video/fbdev/fbdev.h"
#include "kernel/drivers/video/fbdev/font8x16.h"
#include "win32k.h"

#define MAXW 8
#define MAXQ 8
#define QLEN 64
#define RAWQ 128
#define WINPAGES (S5PV210_GUI_SIZE / PGSIZE)

struct kwnd {
  int used, owner, minimized, painted;
  uint gen;
  uint32 style;
  struct w32_rect rc, update, painting;
  char title[64];
  uint32 *front, *back;
  int width, height;
};

struct kqueue {
  int used, tgid;
  uint head, tail;
  struct w32_msg msg[QLEN];
};

static struct sleeplock wm;
static struct spinlock qlock, rawlock, poollock;
static struct kwnd wins[MAXW];
static int z[MAXW], nz, active = -1;
static struct kqueue queues[MAXQ];
static struct fb_kinfo fb;
static uint32 *shadow;
static ushort poolrun[WINPAGES];
static struct workqueue input_wq;
static struct work_struct input_work;
static struct { uint16 type, code; int value; } raw[RAWQ];
static uint rawhead, rawtail;

void fbcon_pause(int pause) __attribute__((weak));

static void *
win_alloc(uint bytes)
{
  uint need = (bytes + PGSIZE - 1) / PGSIZE;
  if(need == 0 || need >= WINPAGES)
    return 0;
  acquire(&poollock);
  for(uint i = 0; i + need <= WINPAGES; i++){
    uint j;
    for(j = 0; j < need && poolrun[i+j] == 0; j++)
      ;
    if(j != need){
      i += j;
      continue;
    }
    poolrun[i] = need;
    for(j = 1; j < need; j++)
      poolrun[i+j] = 0xffff;
    release(&poollock);
    void *p = (void*)(uintptr)(DRAM_BASE + S5PV210_GUI_OFFSET + i*PGSIZE);
    memset(p, 0, need*PGSIZE);
    return p;
  }
  release(&poollock);
  return 0;
}

static void
win_free(void *ptr)
{
  if(ptr == 0)
    return;
  uintptr base = DRAM_BASE + S5PV210_GUI_OFFSET;
  uintptr p = (uintptr)ptr;
  if(p < base || p >= base + S5PV210_GUI_SIZE || ((p-base) & (PGSIZE-1)))
    panic("win32k free");
  uint i = (p-base)/PGSIZE;
  acquire(&poollock);
  uint n = poolrun[i];
  if(n == 0 || n == 0xffff){
    release(&poollock);
    panic("win32k double free");
  }
  for(uint j = 0; j < n; j++)
    poolrun[i+j] = 0;
  release(&poollock);
}

static uint32
pixel(uint32 c)
{
  uint r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255;
  return (r << 16) | (g << 8) | b;
}

static uint32 hwnd(int i) { return i < 0 ? 0 : (wins[i].gen << 8) | (i+1); }

static int
windex(uint32 h)
{
  int i = (h & 255) - 1;
  return i >= 0 && i < MAXW && wins[i].used && wins[i].gen == (h >> 8) ? i : -1;
}

static struct kqueue *
getqueue(int tgid, int create)
{
  struct kqueue *empty = 0;
  for(int i = 0; i < MAXQ; i++){
    if(queues[i].used && queues[i].tgid == tgid)
      return &queues[i];
    if(!queues[i].used && empty == 0)
      empty = &queues[i];
  }
  if(!create || empty == 0)
    return 0;
  empty->used = 1;
  empty->tgid = tgid;
  empty->head = empty->tail = 0;
  return empty;
}

static void
post(int tgid, uint32 h, uint32 message, uint64 wp, long lp)
{
  struct kqueue *q = getqueue(tgid, 1);
  if(q == 0)
    return;
  acquire(&qlock);
  if(q->tail - q->head < QLEN){
    struct w32_msg *m = &q->msg[q->tail++ % QLEN];
    m->hwnd = h; m->message = message; m->wparam = wp; m->lparam = lp;
  }
  wakeup(q);
  release(&qlock);
}

static void
invalidate(int i)
{
  wins[i].update.left = wins[i].update.top = 0;
  wins[i].update.right = wins[i].width;
  wins[i].update.bottom = wins[i].height;
  post(wins[i].owner, hwnd(i), W32M_PAINT, 0, 0);
}

static void
compose(void)
{
  if(shadow == 0 || fb.pixels == 0)
    return;
  uint32 bg = pixel((58U) | (110U<<8) | (165U<<16));
  for(int y = 0; y < fb.height; y++)
    for(int x = 0; x < fb.width; x++)
      shadow[y*fb.width+x] = bg;
  for(int n = 0; n < nz; n++){
    struct kwnd *w = &wins[z[n]];
    if(!w->used || !(w->style & W32S_VISIBLE) || w->minimized || !w->painted)
      continue;
    int l = w->rc.left < 0 ? 0 : w->rc.left;
    int t = w->rc.top < 0 ? 0 : w->rc.top;
    int r = w->rc.right > fb.width ? fb.width : w->rc.right;
    int b = w->rc.bottom > fb.height ? fb.height : w->rc.bottom;
    for(int y = t; y < b; y++)
      memmove(shadow + y*fb.width + l,
              w->front + (y-w->rc.top)*w->width + (l-w->rc.left),
              (r-l)*4);
  }
  for(int y = 0; y < fb.height; y++)
    memmove(fb.pixels + y*fb.stride, shadow + y*fb.width, fb.width*4);
}

static void
zremove(int i)
{
  int out = 0;
  for(int n = 0; n < nz; n++)
    if(z[n] != i)
      z[out++] = z[n];
  nz = out;
}

static void
raise(int i)
{
  zremove(i);
  z[nz++] = i;
  active = i;
  compose();
}

static void
destroy(int i)
{
  int owner = wins[i].owner;
  uint gen = wins[i].gen;
  zremove(i);
  win_free(wins[i].front);
  win_free(wins[i].back);
  memset(&wins[i], 0, sizeof(wins[i]));
  wins[i].gen = gen;
  if(active == i)
    active = nz ? z[nz-1] : -1;
  compose();
  if(nz == 0 && fbcon_pause)
    fbcon_pause(0);
  (void)owner;
}

static void
fill(struct kwnd *w, const struct w32_gdicmd *c)
{
  int l=c->x0, t=c->y0, r=c->x1, b=c->y1;
  if(l<c->clip_left) l=c->clip_left; if(t<c->clip_top) t=c->clip_top;
  if(r>c->clip_right) r=c->clip_right; if(b>c->clip_bottom) b=c->clip_bottom;
  if(l<0) l=0; if(t<0) t=0; if(r>w->width) r=w->width; if(b>w->height) b=w->height;
  uint32 col = pixel(c->color);
  for(int y=t; y<b; y++) for(int x=l; x<r; x++) w->back[y*w->width+x]=col;
}

static void
text(struct kwnd *w, const struct w32_gdicmd *c, const char *s)
{
  uint32 col = pixel(c->color);
  for(int n=0; n<c->len; n++){
    int ch=(uchar)s[n];
    if(ch < FONT_FIRST || ch > FONT_LAST) continue;
    const uchar *g=font8x16[ch-FONT_FIRST];
    for(int row=0; row<FONT_H; row++) for(int bit=0; bit<8; bit++){
      int x=c->x0+n*8+bit, y=c->y0+row;
      if((g[row]&(0x80>>bit)) && x>=c->clip_left && x<c->clip_right &&
         y>=c->clip_top && y<c->clip_bottom && x>=0 && x<w->width && y>=0 && y<w->height)
        w->back[y*w->width+x]=col;
    }
  }
}

static int
keychar(int code)
{
  if(code >= KEY_A && code <= KEY_L){
    static const char map[]="asdfghjkl";
    return map[code-KEY_A];
  }
  if(code >= KEY_Q && code <= KEY_P){
    static const char map[]="qwertyuiop";
    return map[code-KEY_Q];
  }
  if(code >= KEY_Z && code <= KEY_M){
    static const char map[]="zxcvbnm";
    return map[code-KEY_Z];
  }
  if(code >= KEY_1 && code <= KEY_9) return '1'+code-KEY_1;
  if(code == KEY_0) return '0';
  if(code == KEY_SPACE) return ' ';
  if(code == KEY_ENTER) return '\n';
  return 0;
}

static void
input_workfn(struct work_struct *work)
{
  for(;;){
    acquire(&rawlock);
    if(rawhead == rawtail){ release(&rawlock); return; }
    uint slot=rawhead++%RAWQ;
    uint16 type=raw[slot].type, code=raw[slot].code;
    int value=raw[slot].value;
    release(&rawlock);
    if(type != EV_KEY || !value) continue;
    acquiresleep(&wm);
    if(active >= 0 && wins[active].used){
      uint32 h=hwnd(active); int owner=wins[active].owner; int ch=keychar(code);
      post(owner,h,W32M_KEYDOWN,code,0);
      if(ch) post(owner,h,W32M_CHAR,ch,0);
    }
    releasesleep(&wm);
  }
}

void
win32k_input_event(uint16 type, uint16 code, int value)
{
  acquire(&rawlock);
  if(rawtail-rawhead < RAWQ){
    uint s=rawtail++%RAWQ;
    raw[s].type=type; raw[s].code=code; raw[s].value=value;
  }
  release(&rawlock);
  queue_work(&input_wq, &input_work);
}

int win32k_owns_input(void) { return nz != 0; }

void
win32k_exit(int tgid)
{
  acquiresleep(&wm);
  for(int i=0;i<MAXW;i++) if(wins[i].used && wins[i].owner==tgid) destroy(i);
  struct kqueue *q=getqueue(tgid,0);
  if(q) memset(q,0,sizeof(*q));
  releasesleep(&wm);
}

uint64
sys_win32k(void)
{
  int op; uint64 a1,a2,a3;
  struct proc *p=myproc();
  if(argint(0,&op)<0 || argaddr(1,&a1)<0 || argaddr(2,&a2)<0 || argaddr(3,&a3)<0)
    return -1;
  if(op==W32_CREATE){
    struct w32_create c;
    if(copyin(p->vm->pagetable,(char*)&c,a1,sizeof(c))<0 || c.w<=0 || c.h<=0)
      return -1;
    if(fb_kernel_info(&fb)<0 || c.w>fb.width || c.h>fb.height) return -1;
    acquiresleep(&wm);
    if(!shadow) shadow=win_alloc(fb.width*fb.height*4);
    int i; for(i=0;i<MAXW && wins[i].used;i++); 
    if(i==MAXW || !shadow){ releasesleep(&wm); return -1; }
    uint32 *front=win_alloc(c.w*c.h*4), *back=win_alloc(c.w*c.h*4);
    if(!front || !back){ win_free(front); win_free(back); releasesleep(&wm); return -1; }
    uint gen=wins[i].gen+1; memset(&wins[i],0,sizeof(wins[i]));
    wins[i].used=1; wins[i].gen=gen; wins[i].owner=p->tgid; wins[i].style=c.style;
    wins[i].rc=(struct w32_rect){c.x,c.y,c.x+c.w,c.y+c.h};
    wins[i].width=c.w; wins[i].height=c.h; wins[i].front=front; wins[i].back=back;
    safestrcpy(wins[i].title,c.title,sizeof(wins[i].title));
    if(nz == 0 && fbcon_pause) fbcon_pause(1);
    z[nz++]=i; active=i;
    invalidate(i); compose(); uint32 h=hwnd(i); releasesleep(&wm); return h;
  }
  if(op==W32_GETMSG){
    struct kqueue *q=getqueue(p->tgid,1); struct w32_msg m;
    acquire(&qlock);
    while(q->head==q->tail && !p->killed) sleep(q,&qlock);
    if(p->killed){ release(&qlock); return -1; }
    m=q->msg[q->head++%QLEN]; release(&qlock);
    return copyout(p->vm->pagetable,a1,(char*)&m,sizeof(m))<0 ? -1 : 1;
  }
  if(op==W32_POST){
    struct w32_msg m; if(copyin(p->vm->pagetable,(char*)&m,a1,sizeof(m))<0) return -1;
    acquiresleep(&wm); int i=windex(m.hwnd); if(i>=0) post(wins[i].owner,m.hwnd,m.message,m.wparam,m.lparam);
    releasesleep(&wm); return i<0?-1:0;
  }
  if(op==W32_QUIT){ post(p->tgid,0,W32M_QUIT,a1,0); return 0; }
  if(op==W32_SCREEN){ if(fb_kernel_info(&fb)<0)return -1; return fb.width|(fb.height<<16); }
  if(op==W32_GETACTIVE) return active>=0?hwnd(active):0;
  acquiresleep(&wm);
  int i=windex(a1); if(i<0){ releasesleep(&wm); return -1; }
  struct kwnd *w=&wins[i]; int mine=w->owner==p->tgid;
  if(op==W32_DESTROY && mine){ destroy(i); releasesleep(&wm); return 0; }
  if(op==W32_ACTIVATE){ raise(i); releasesleep(&wm); return 0; }
  if(op==W32_SHOW){ w->minimized=!a2; if(a2)w->style|=W32S_VISIBLE; compose(); releasesleep(&wm); return 0; }
  if(op==W32_INVALIDATE && mine){ invalidate(i); releasesleep(&wm); return 0; }
  if(op==W32_BEGINPAINT && mine){
    struct w32_paint ps={w->rc,w->update,i==active}; w->painting=w->update;
    memset(&w->update,0,sizeof(w->update)); releasesleep(&wm);
    return copyout(p->vm->pagetable,a2,(char*)&ps,sizeof(ps))<0?-1:0;
  }
  if(op==W32_ENDPAINT && mine){
    struct w32_rect r=w->painting; if(r.right<=r.left||r.bottom<=r.top)r=(struct w32_rect){0,0,w->width,w->height};
    for(int y=r.top;y<r.bottom;y++) memmove(w->front+y*w->width+r.left,w->back+y*w->width+r.left,(r.right-r.left)*4);
    w->painted=1; compose(); releasesleep(&wm); return 0;
  }
  if(op==W32_GDI && mine){
    uint len=a3; if(len==0||len>S5PV210_GUI_SIZE/4){releasesleep(&wm);return -1;}
    char *buf=win_alloc(len); releasesleep(&wm); if(!buf)return -1;
    if(copyin(p->vm->pagetable,buf,a2,len)<0){win_free(buf);return -1;}
    acquiresleep(&wm); i=windex(a1); if(i<0){releasesleep(&wm);win_free(buf);return -1;} w=&wins[i];
    uint off=0; while(off+sizeof(struct w32_gdicmd)<=len){struct w32_gdicmd *c=(void*)(buf+off);off+=sizeof(*c);if(off+c->len>len)break;if(c->op==GDI_FILL)fill(w,c);else if(c->op==GDI_TEXT)text(w,c,buf+off);off+=(c->len+3)&~3;}
    releasesleep(&wm); win_free(buf); return 0;
  }
  if(op==W32_GETINFO){ struct w32_info in={w->rc,w->style,w->owner,w->minimized,i==active}; releasesleep(&wm); return copyout(p->vm->pagetable,a2,(char*)&in,sizeof(in))<0?-1:0; }
  if(op==W32_SETTEXT && mine){ char title[64]; releasesleep(&wm); if(copyinstr(p->vm->pagetable,title,a2,sizeof(title))<0)return -1; acquiresleep(&wm); i=windex(a1); if(i>=0)safestrcpy(wins[i].title,title,64); releasesleep(&wm); return i<0?-1:0; }
  releasesleep(&wm); return -1;
}

static void
win32k_init(void)
{
  initsleeplock(&wm,"win32k"); initlock(&qlock,"win32k-q");
  initlock(&rawlock,"win32k-input"); initlock(&poollock,"win32k-pool");
  init_workqueue(&input_wq,"win32k",1); init_work(&input_work,input_workfn);
  printf("win32k: Tiny210 compositor, 16 MiB cached surface arena\n");
}
subsys_initcall("win32k",win32k_init);
