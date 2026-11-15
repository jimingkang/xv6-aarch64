#include "kernel/types.h"
#include "user/user.h"
#include "user/win32.h"

static LRESULT
WndProc(HWND w,uint msg,WPARAM wp,LPARAM lp)
{
  (void)lp;
  if(msg==WM_PAINT){
    PAINTSTRUCT ps; HDC dc=BeginPaint(w,&ps);
    RECT box={20,35,360,170}; FillRect(dc,&box,RGB(255,255,255));
    TextOut(dc,32,55,"Tiny210 win32k",15);
    TextOut(dc,32,85,"LCD front/back surfaces",23);
    TextOut(dc,32,115,"Press a board key",17);
    EndPaint(w,&ps); return 0;
  }
  if(msg==WM_CHAR){ printf("windemo: key %c\n",(int)wp); return 0; }
  if(msg==WM_DESTROY){PostQuitMessage(0);return 0;}
  return DefWindowProc(w,msg,wp,lp);
}

int
main(void)
{
  if(WinInit()<0){printf("windemo: framebuffer unavailable\n");exit(1);}
  WNDCLASS wc={"tiny210",WndProc,RGB(192,192,192),0}; RegisterClass(&wc);
  HWND w=CreateWindow("tiny210","Tiny210",WS_VISIBLE|WS_OVERLAPPEDWINDOW,80,60,400,220,0,0,0);
  if(!w){printf("windemo: CreateWindow failed\n");exit(1);}
  MSG m; while(GetMessage(&m,0,0,0)>0)DispatchMessage(&m);
  WinExit(); exit(0);
}
