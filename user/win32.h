// A tiny Win32-flavoured windowing API for xv6 (user space, one process).
//
// The shapes follow Win32 (user32 + gdi32) closely enough that a program
// reads like a classic Windows program -- RegisterClass, CreateWindow,
// a GetMessage/DispatchMessage loop, WM_PAINT with BeginPaint/EndPaint,
// WM_COMMAND from controls -- but everything runs inside one xv6 process on
// top of /dev/fb0 (screen), /dev/input/eventN (keyboard, mouse) and
// pty_open() (the console window).  It is a teaching model in the spirit of
// ReactOS's win32k, not a binary-compatible implementation.
//
// Simplifications:
//   - colours are COLORREF (0x00BBGGRR, as RGB() builds them); there are no
//     brush/pen objects, the GDI calls take colours directly
//   - one thread, one message queue; MessageBox() does not block
//   - no resizing; windows can be moved, minimized, closed

#ifndef XV6_WIN32_H
#define XV6_WIN32_H

#include <uapi/types.h>

typedef struct window *HWND;
typedef struct dc *HDC;
typedef unsigned long WPARAM;
typedef long LPARAM;
typedef long LRESULT;
typedef uint32 COLORREF;
typedef LRESULT (*WNDPROC)(HWND, uint, WPARAM, LPARAM);

typedef struct { int left, top, right, bottom; } RECT;
typedef struct { int x, y; } POINT;

typedef struct {
  HWND hwnd;
  uint message;
  WPARAM wParam;
  LPARAM lParam;
} MSG;

typedef struct {
  const char *lpszClassName;
  WNDPROC lpfnWndProc;
  COLORREF hbrBackground;       // client background, painted before WM_PAINT
  uint style;
} WNDCLASS;

typedef struct {
  HDC hdc;
  RECT rcPaint;
} PAINTSTRUCT;

#define RGB(r, g, b) ((COLORREF)(((r) & 0xff) | (((g) & 0xff) << 8) | \
                                 (((b) & 0xff) << 16)))
#define GetRValue(c) ((c) & 0xff)
#define GetGValue(c) (((c) >> 8) & 0xff)
#define GetBValue(c) (((c) >> 16) & 0xff)
#define LOWORD(l) ((int)(short)((l) & 0xffff))
#define HIWORD(l) ((int)(short)(((l) >> 16) & 0xffff))
#define MAKELPARAM(lo, hi) ((LPARAM)(((uint32)(lo) & 0xffff) | \
                                     (((uint32)(hi) & 0xffff) << 16)))

// The classic 3D palette.
#define CLR_FACE      RGB(192, 192, 192)
#define CLR_HILIGHT   RGB(255, 255, 255)
#define CLR_LIGHT     RGB(223, 223, 223)
#define CLR_SHADOW    RGB(128, 128, 128)
#define CLR_DKSHADOW  RGB(0, 0, 0)
#define CLR_WINDOW    RGB(255, 255, 255)
#define CLR_TEXT      RGB(0, 0, 0)
#define CLR_SELECT    RGB(0, 0, 128)
#define CLR_DESKTOP   RGB(58, 110, 165)

// Window styles.
#define WS_OVERLAPPED  0x00000000u
#define WS_CHILD       0x40000000u
#define WS_VISIBLE     0x10000000u
#define WS_CAPTION     0x00C00000u   // title bar + 3D frame
#define WS_BORDER      0x00800000u   // thin sunken border (controls)
#define WS_POPUP       0x80000000u   // no frame (menus, taskbar)
#define WS_TOPMOST     0x00000008u   // stays above normal windows
#define WS_NOTASKBAR   0x00000004u   // not listed on the taskbar
#define WS_BOTTOM      0x00000010u   // the desktop: never raised
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION)
#define ES_MULTILINE   0x0004u
#define ES_READONLY    0x0800u
#define ES_RIGHT       0x0002u

#define CW_USEDEFAULT  ((int)0x80000000)

// Messages.
#define WM_CREATE        0x0001
#define WM_DESTROY       0x0002
#define WM_SETFOCUS      0x0007
#define WM_KILLFOCUS     0x0008
#define WM_SETTEXT       0x000C
#define WM_PAINT         0x000F
#define WM_CLOSE         0x0010
#define WM_QUIT          0x0012
#define WM_KEYDOWN       0x0100
#define WM_CHAR          0x0102
#define WM_COMMAND       0x0111
#define WM_TIMER         0x0113
#define WM_MOUSEMOVE     0x0200
#define WM_LBUTTONDOWN   0x0201
#define WM_LBUTTONUP     0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN   0x0204
#define WM_MOUSEWHEEL    0x020A
#define WM_USER          0x0400
#define WM_FDREADY       (WM_USER + 0x100)  // wParam = fd (WinWatchFd)
#define WM_SHELLHOOK     (WM_USER + 0x101)  // to the taskbar: windows changed

// Virtual keys (WM_KEYDOWN wParam).
#define VK_BACK    0x08
#define VK_TAB     0x09
#define VK_RETURN  0x0D
#define VK_ESCAPE  0x1B
#define VK_END     0x23
#define VK_HOME    0x24
#define VK_LEFT    0x25
#define VK_UP      0x26
#define VK_RIGHT   0x27
#define VK_DOWN    0x28
#define VK_DELETE  0x2E

// Control notifications (HIWORD of WM_COMMAND wParam).
#define BN_CLICKED     0
#define LBN_SELCHANGE  1
#define LBN_DBLCLK     2
#define EN_CHANGE      0x300

// List box messages.
#define LB_ADDSTRING     0x0180
#define LB_RESETCONTENT  0x0184
#define LB_SETCURSEL     0x0186
#define LB_GETCURSEL     0x0188
#define LB_GETTEXT       0x0189
#define LB_GETCOUNT      0x018B
#define LB_ERR           (-1)

// DrawEdge kinds.
#define EDGE_RAISED   1
#define EDGE_SUNKEN   2
#define EDGE_ETCHED   3

#define DT_LEFT    0
#define DT_CENTER  1
#define DT_RIGHT   2

#define MB_OK      0

// GetWindowLong indices.
#define GWLP_USERDATA (-21)
#define GWLP_ID       (-12)

// System metrics used by the shell.
#define SM_CXSCREEN   0
#define SM_CYSCREEN   1
#define SM_CYCAPTION  4
#define SM_CYTASKBAR  100

// user32
int     RegisterClass(const WNDCLASS *wc);
HWND    CreateWindow(const char *cls, const char *title, uint style,
                     int x, int y, int w, int h, HWND parent, int id,
                     void *param);
int     DestroyWindow(HWND hwnd);
LRESULT DefWindowProc(HWND hwnd, uint msg, WPARAM wp, LPARAM lp);
int     GetMessage(MSG *msg, HWND hwnd, uint min, uint max);
LRESULT DispatchMessage(const MSG *msg);
int     PostMessage(HWND hwnd, uint msg, WPARAM wp, LPARAM lp);
LRESULT SendMessage(HWND hwnd, uint msg, WPARAM wp, LPARAM lp);
void    PostQuitMessage(int code);
int     InvalidateRect(HWND hwnd, const RECT *rc, int erase);
int     ShowWindow(HWND hwnd, int show);   // 0 hide/minimize, 1 show/restore
int     SetWindowText(HWND hwnd, const char *text);
int     GetWindowText(HWND hwnd, char *buf, int max);
int     GetWindowTextLength(HWND hwnd);
int     GetClientRect(HWND hwnd, RECT *rc);
int     GetWindowRect(HWND hwnd, RECT *rc);
int     MoveWindow(HWND hwnd, int x, int y, int w, int h, int repaint);
HWND    SetFocus(HWND hwnd);
HWND    GetFocus(void);
HWND    SetActiveWindow(HWND hwnd);
HWND    GetActiveWindow(void);
HWND    GetParent(HWND hwnd);
HWND    GetDlgItem(HWND parent, int id);
long    GetWindowLong(HWND hwnd, int index);
long    SetWindowLong(HWND hwnd, int index, long value);
int     SetTimer(HWND hwnd, int id, int ms);
int     KillTimer(HWND hwnd, int id);
int     MessageBox(HWND owner, const char *text, const char *caption, uint type);
int     GetSystemMetrics(int index);
int     IsWindow(HWND hwnd);
int     IsWindowVisible(HWND hwnd);
int     IsIconic(HWND hwnd);

// gdi32
HDC     BeginPaint(HWND hwnd, PAINTSTRUCT *ps);
int     EndPaint(HWND hwnd, const PAINTSTRUCT *ps);
HDC     GetDC(HWND hwnd);
int     ReleaseDC(HWND hwnd, HDC hdc);
int     FillRect(HDC hdc, const RECT *rc, COLORREF color);
int     FrameRect(HDC hdc, const RECT *rc, COLORREF color);
int     SetPixel(HDC hdc, int x, int y, COLORREF color);
int     TextOut(HDC hdc, int x, int y, const char *s, int len);
int     DrawText(HDC hdc, const char *s, int len, const RECT *rc, uint fmt);
COLORREF SetTextColor(HDC hdc, COLORREF c);
COLORREF SetBkColor(HDC hdc, COLORREF c);
int     SetBkMode(HDC hdc, int opaque);     // 0 transparent, 1 opaque
int     DrawEdge(HDC hdc, const RECT *rc, int edge);
int     GetTextWidth(const char *s);         // pixels (fixed 8x16 font)
#define FONT_CX 8
#define FONT_CY 16

// xv6 extensions
int     WinInit(void);                       // open fb0 + input; 0 on success
void    WinExit(void);
int     WinWatchFd(int fd, HWND hwnd);       // post WM_FDREADY when readable
void    WinUnwatchFd(int fd);
// Enumerate taskbar-visible top-level windows, in creation order.
int     WinEnumTopLevel(HWND *out, int max);
// Pop up a menu of items (NULL = separator) at screen x,y.  The chosen
// index arrives at owner as WM_COMMAND(base_id + index).
HWND    TrackPopupMenu(const char **items, int n, int x, int y, HWND owner,
                       int base_id);
void    WinSetTaskbar(HWND hwnd);            // the work area ends above it

#endif
