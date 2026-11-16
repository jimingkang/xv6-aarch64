/* Small user32/gdi32 facade.  Kernel handles and surfaces stay in win32k;
 * this file owns only process-local HWND/WNDPROC/DC objects. */
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "win32k.h"
#include "user/win32.h"

struct window {
  uint32 handle;
  WNDPROC proc;
  COLORREF bg;
  uint style;
  int width, height;
  char title[64];
  struct window *next;
};
struct dc { struct window *w; RECT clip; COLORREF text; };
struct klass { int used; char name[32]; WNDPROC proc; COLORREF bg; };
static struct klass classes[8];
static struct window *windows;
static int screenw, screenh;

static struct window *byhandle(uint32 h){ for(struct window *w=windows;w;w=w->next)if(w->handle==h)return w;return 0; }
static struct klass *byclass(const char *s){for(int i=0;i<8;i++)if(classes[i].used&&!strcmp(classes[i].name,s))return &classes[i];return 0;}

int WinInit(void){ long s=win32k(W32_SCREEN,0,0,0); if(s<0)return -1;screenw=s&0xffff;screenh=(s>>16)&0xffff;return 0; }
void WinExit(void){ while(windows)DestroyWindow(windows); }

int RegisterClass(const WNDCLASS *wc){ if(!wc||!wc->lpszClassName||!wc->lpfnWndProc)return 0;for(int i=0;i<8;i++)if(!classes[i].used){classes[i].used=1;strncpy(classes[i].name,wc->lpszClassName,31);classes[i].name[31]=0;classes[i].proc=wc->lpfnWndProc;classes[i].bg=wc->hbrBackground;return i+1;}return 0; }

HWND CreateWindow(const char *cls,const char *title,uint style,int x,int y,int width,int height,HWND parent,int id,void *param){
  (void)parent;(void)id;(void)param; struct klass *k=byclass(cls);if(!k)return 0;
  struct w32_create c;memset(&c,0,sizeof(c));c.x=x;c.y=y;c.w=width;c.h=height;c.style=W32S_VISIBLE|((style&WS_CAPTION)?W32S_CAPTION:0);strncpy(c.title,title?title:"",63);
  long h=win32k(W32_CREATE,(long)&c,0,0);if(h<=0)return 0;
  struct window *w=malloc(sizeof(*w));if(!w){win32k(W32_DESTROY,h,0,0);return 0;}
  memset(w,0,sizeof(*w));w->handle=h;w->proc=k->proc;w->bg=k->bg;
  w->style=style;w->width=width;w->height=height;
  strncpy(w->title,title?title:"",sizeof(w->title)-1);
  w->next=windows;windows=w;w->proc(w,WM_CREATE,0,0);return w;
}

int DestroyWindow(HWND w){if(!w)return 0;win32k(W32_DESTROY,w->handle,0,0);struct window **p=&windows;while(*p&&*p!=w)p=&(*p)->next;if(*p)*p=w->next;w->proc(w,WM_DESTROY,0,0);free(w);return 1;}
void PostQuitMessage(int code){win32k(W32_QUIT,code,0,0);}
int InvalidateRect(HWND w,const RECT *r,int erase){(void)r;(void)erase;return w&&win32k(W32_INVALIDATE,w->handle,0,0)==0;}

int GetMessage(MSG *m,HWND only,uint min,uint max){(void)only;(void)min;(void)max;struct w32_msg km;long r=win32k(W32_GETMSG,(long)&km,-1,0);if(r<=0)return r;m->hwnd=byhandle(km.hwnd);m->message=km.message;m->wParam=km.wparam;m->lParam=km.lparam;return km.message!=WM_QUIT;}
LRESULT DispatchMessage(const MSG *m){return m&&m->hwnd&&m->hwnd->proc?m->hwnd->proc(m->hwnd,m->message,m->wParam,m->lParam):0;}
LRESULT DefWindowProc(HWND w,uint msg,WPARAM wp,LPARAM lp){(void)wp;(void)lp;if(msg==WM_CLOSE){DestroyWindow(w);PostQuitMessage(0);return 0;}return 0;}

static void
draw_window_frame(HDC d, int active)
{
  struct window *w=d->w;
  RECT r;

  if(!(w->style&WS_CAPTION))
    return;
  r=(RECT){0,0,w->width,2};FillRect(d,&r,RGB(28,28,28));
  r=(RECT){0,w->height-2,w->width,w->height};FillRect(d,&r,RGB(28,28,28));
  r=(RECT){0,0,2,w->height};FillRect(d,&r,RGB(28,28,28));
  r=(RECT){w->width-2,0,w->width,w->height};FillRect(d,&r,RGB(28,28,28));
  r=(RECT){2,2,w->width-2,25};
  FillRect(d,&r,active?RGB(10,82,160):RGB(102,112,128));
  d->text=RGB(255,255,255);
  TextOut(d,9,5,w->title,strlen(w->title));
  r=(RECT){w->width-25,5,w->width-7,21};FillRect(d,&r,RGB(205,53,53));
  TextOut(d,w->width-20,5,"x",1);
}

HDC BeginPaint(HWND w,PAINTSTRUCT *ps){if(!w||!ps)return 0;struct w32_paint kp;if(win32k(W32_BEGINPAINT,w->handle,(long)&kp,0)<0)return 0;struct dc *d=malloc(sizeof(*d));if(!d)return 0;d->w=w;d->clip=(RECT){kp.update.left,kp.update.top,kp.update.right,kp.update.bottom};d->text=RGB(0,0,0);ps->hdc=d;ps->rcPaint=d->clip;if(w->bg){RECT all={0,0,kp.rc.right-kp.rc.left,kp.rc.bottom-kp.rc.top};FillRect(d,&all,w->bg);}draw_window_frame(d,kp.active);return d;}
int EndPaint(HWND w,const PAINTSTRUCT *ps){if(!w||!ps||!ps->hdc)return 0;free(ps->hdc);return win32k(W32_ENDPAINT,w->handle,0,0)==0;}

static int gdi(HDC d,struct w32_gdicmd *c,const char *s){if(!d||!d->w)return 0;c->clip_left=d->clip.left;c->clip_top=d->clip.top;c->clip_right=d->clip.right;c->clip_bottom=d->clip.bottom;uint bytes=sizeof(*c)+((c->len+3)&~3);char *b=malloc(bytes);if(!b)return 0;memset(b,0,bytes);memmove(b,c,sizeof(*c));if(c->len)memmove(b+sizeof(*c),s,c->len);int ok=win32k(W32_GDI,d->w->handle,(long)b,bytes)==0;free(b);return ok;}
int FillRect(HDC d,const RECT *r,COLORREF color){struct w32_gdicmd c;memset(&c,0,sizeof(c));c.op=GDI_FILL;c.x0=r->left;c.y0=r->top;c.x1=r->right;c.y1=r->bottom;c.color=color;return gdi(d,&c,0);}
COLORREF SetTextColor(HDC d,COLORREF color){if(!d)return 0;COLORREF old=d->text;d->text=color;return old;}
int TextOut(HDC d,int x,int y,const char *s,int len){struct w32_gdicmd c;memset(&c,0,sizeof(c));c.op=GDI_TEXT;c.len=len;c.x0=x;c.y0=y;c.color=d?d->text:RGB(0,0,0);return gdi(d,&c,s);}
int DrawText(HDC d,const char *s,int len,const RECT *r,uint fmt){(void)fmt;if(len<0)len=strlen(s);return TextOut(d,r->left,r->top,s,len);}
int GetClientRect(HWND w,RECT *r){struct w32_info i;if(!w||win32k(W32_GETINFO,w->handle,(long)&i,0)<0)return 0;*r=(RECT){0,0,i.rc.right-i.rc.left,i.rc.bottom-i.rc.top};return 1;}
int GetSystemMetrics(int n){return n==SM_CXSCREEN?screenw:n==SM_CYSCREEN?screenh:0;}
HWND SetActiveWindow(HWND w){if(!w)return 0;long oldh=win32k(W32_GETACTIVE,0,0,0);if(win32k(W32_ACTIVATE,w->handle,0,0)<0)return 0;struct window *old=byhandle(oldh);if(old&&old!=w)InvalidateRect(old,0,0);InvalidateRect(w,0,0);return w;}
