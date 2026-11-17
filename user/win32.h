#ifndef XV6_USER_WIN32_H
#define XV6_USER_WIN32_H

#include "kernel/types.h"

typedef struct window *HWND;
typedef struct dc *HDC;
typedef uint32 COLORREF;
typedef unsigned long WPARAM;
typedef long LPARAM, LRESULT;
typedef LRESULT (*WNDPROC)(HWND, uint, WPARAM, LPARAM);
typedef struct { int left, top, right, bottom; } RECT;
typedef struct { HWND hwnd; uint message; WPARAM wParam; LPARAM lParam; } MSG;
typedef struct { const char *lpszClassName; WNDPROC lpfnWndProc; COLORREF hbrBackground; uint style; } WNDCLASS;
typedef struct { HDC hdc; RECT rcPaint; } PAINTSTRUCT;

#define RGB(r,g,b) ((COLORREF)(((r)&255)|(((g)&255)<<8)|(((b)&255)<<16)))
#define WS_VISIBLE 0x10000000u
#define WS_CAPTION 0x00c00000u
#define WS_OVERLAPPEDWINDOW WS_CAPTION
#define WM_CREATE 1
#define WM_DESTROY 2
#define WM_PAINT 0x000f
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_KEYDOWN 0x0100
#define WM_CHAR 0x0102
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_MOUSEWHEEL 0x020a
#define MK_LBUTTON 0x01
#define MK_RBUTTON 0x02
#define MK_MBUTTON 0x10
#define GET_X_LPARAM(lp) ((int)(short)((lp) & 0xffff))
#define GET_Y_LPARAM(lp) ((int)(short)(((lp) >> 16) & 0xffff))
#define GET_WHEEL_DELTA_WPARAM(wp) ((short)((wp) >> 16))
#define DT_LEFT 0

int RegisterClass(const WNDCLASS *wc);
HWND CreateWindow(const char *cls,const char *title,uint style,int x,int y,int w,int h,HWND parent,int id,void *param);
int DestroyWindow(HWND hwnd);
LRESULT DefWindowProc(HWND hwnd,uint msg,WPARAM wp,LPARAM lp);
int GetMessage(MSG *msg,HWND hwnd,uint min,uint max);
LRESULT DispatchMessage(const MSG *msg);
void PostQuitMessage(int code);
int InvalidateRect(HWND hwnd,const RECT *rc,int erase);
HDC BeginPaint(HWND hwnd,PAINTSTRUCT *ps);
int EndPaint(HWND hwnd,const PAINTSTRUCT *ps);
int FillRect(HDC hdc,const RECT *rc,COLORREF color);
COLORREF SetTextColor(HDC hdc,COLORREF color);
int TextOut(HDC hdc,int x,int y,const char *s,int len);
int DrawText(HDC hdc,const char *s,int len,const RECT *rc,uint fmt);
int GetClientRect(HWND hwnd,RECT *rc);
int GetSystemMetrics(int index);
HWND SetActiveWindow(HWND hwnd);
int WinInit(void);
void WinExit(void);

#define SM_CXSCREEN 0
#define SM_CYSCREEN 1

#endif
