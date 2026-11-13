// win: a small classic-Windows-style desktop for xv6, modelled on the
// look of ReactOS (an open-source Windows clone): a desktop with icons, a
// taskbar with a Start menu and clock, and a few accessories written as
// ordinary Win32-style programs against win32.h:
//
//   Explorer        browse the file system; open files in Notepad
//   Notepad         edit and save text files
//   Calculator      integer calculator
//   Command Prompt  /bin/sh on a pseudo-terminal
//   Task Manager    processes (ps) and memory; End Process
//
// Everything runs in this one process: one message loop dispatches to every
// window procedure, as in 16-bit Windows.  Shut Down (Start menu) or
// Ctrl+Alt is not needed -- Start > Shut Down returns to the text console.

#include <uapi/types.h>
#include <uapi/stat.h>
#include "user/user.h"
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <uapi/xv6fs.h>
#include "user/win32.h"

#define TASKBAR_H 28

static HWND desktop, taskbar;

static void open_explorer(const char *path);
static void open_notepad(const char *path);
static void open_calc(void);
static void open_console(void);
static void open_taskmgr(void);

// strncpy that always terminates (the kernel's safestrcpy).
static void
scopy(char *d, const char *s, int n)
{
  if(n <= 0)
    return;
  while(--n > 0 && *s)
    *d++ = *s++;
  *d = 0;
}

static void
itoa_(char *d, long v)
{
  char t[24];
  int n = 0, neg = v < 0;
  unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
  do t[n++] = '0' + u % 10; while((u /= 10) && n < 22);
  if(neg)
    *d++ = '-';
  while(n)
    *d++ = t[--n];
  *d = 0;
}

static void
two(char *d, int v)
{
  d[0] = '0' + (v / 10) % 10;
  d[1] = '0' + v % 10;
}

// ===========================================================================
// Icons: 32x32 pictures drawn with a few rectangles, so no image files.

enum { IC_COMPUTER, IC_NOTEPAD, IC_CALC, IC_CONSOLE, IC_TASKMGR, IC_FOLDER,
       IC_FILE };

static void
box(HDC dc, int x, int y, int w, int h, COLORREF c)
{
  RECT r = { x, y, x + w, y + h };
  FillRect(dc, &r, c);
}

static void
draw_icon(HDC dc, int x, int y, int kind)
{
  switch(kind){
  case IC_COMPUTER:
    box(dc, x + 2, y + 2, 28, 20, CLR_LIGHT);
    box(dc, x + 5, y + 5, 22, 14, RGB(0, 128, 128));
    box(dc, x + 12, y + 22, 8, 3, CLR_SHADOW);
    box(dc, x + 4, y + 25, 24, 5, CLR_LIGHT);
    box(dc, x + 4, y + 29, 24, 1, CLR_SHADOW);
    break;
  case IC_NOTEPAD:
    box(dc, x + 6, y + 2, 20, 28, CLR_HILIGHT);
    box(dc, x + 6, y + 2, 20, 4, RGB(0, 0, 160));
    for(int i = 0; i < 6; i++)
      box(dc, x + 9, y + 9 + i * 3, 14, 1, CLR_SHADOW);
    box(dc, x + 25, y + 2, 1, 28, CLR_DKSHADOW);
    break;
  case IC_CALC:
    box(dc, x + 6, y + 1, 20, 30, CLR_SHADOW);
    box(dc, x + 8, y + 3, 16, 6, RGB(160, 200, 160));
    for(int r = 0; r < 4; r++)
      for(int c = 0; c < 3; c++)
        box(dc, x + 8 + c * 6, y + 12 + r * 5, 4, 3, CLR_HILIGHT);
    break;
  case IC_CONSOLE:
    box(dc, x + 1, y + 4, 30, 24, CLR_LIGHT);
    box(dc, x + 1, y + 4, 30, 4, RGB(0, 0, 160));
    box(dc, x + 3, y + 9, 26, 17, CLR_DKSHADOW);
    SetTextColor(dc, CLR_HILIGHT);
    TextOut(dc, x + 5, y + 9, ">_", 2);
    break;
  case IC_TASKMGR:
    box(dc, x + 2, y + 2, 28, 28, CLR_DKSHADOW);
    for(int i = 0; i < 5; i++){
      int h = (i * 7 + 5) % 20 + 4;
      box(dc, x + 5 + i * 5, y + 28 - h, 3, h, RGB(0, 220, 0));
    }
    break;
  case IC_FOLDER:
    box(dc, x + 2, y + 6, 12, 4, RGB(220, 180, 40));
    box(dc, x + 2, y + 9, 28, 19, RGB(240, 210, 80));
    box(dc, x + 2, y + 27, 28, 1, RGB(160, 120, 0));
    break;
  default:
    box(dc, x + 8, y + 2, 16, 28, CLR_HILIGHT);
    box(dc, x + 23, y + 2, 1, 28, CLR_SHADOW);
    break;
  }
}

// ===========================================================================
// Desktop.

static const struct {
  const char *label;
  int icon;
} desk_items[] = {
  { "My Computer", IC_COMPUTER },
  { "Notepad", IC_NOTEPAD },
  { "Calculator", IC_CALC },
  { "Command", IC_CONSOLE },
  { "Task Manager", IC_TASKMGR },
};
#define NDESK (int)(sizeof(desk_items) / sizeof(desk_items[0]))
#define DESK_CELL 70

static int desk_sel = -1;

static void
desk_launch(int i)
{
  switch(i){
  case 0: open_explorer("/"); break;
  case 1: open_notepad(0); break;
  case 2: open_calc(); break;
  case 3: open_console(); break;
  case 4: open_taskmgr(); break;
  }
}

static int
desk_hit(int x, int y)
{
  int i = (y - 8) / DESK_CELL;
  if(x < 4 || x > 4 + 80 || i < 0 || i >= NDESK)
    return -1;
  return i;
}

static const char *about_text =
  "xv6 on Raspberry Pi 3B (AArch64)\n"
  "\n"
  "A classic-style desktop in the spirit of ReactOS:\n"
  "win32.c implements windows, messages, GDI and\n"
  "controls on /dev/fb0 + /dev/input + pty.";

static LRESULT
DesktopProc(HWND w, uint msg, WPARAM wp, LPARAM lp)
{
  switch(msg){
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    RECT r;
    GetClientRect(w, &r);
    // Wordmark in the lower right corner.
    SetTextColor(dc, RGB(90, 140, 190));
    TextOut(dc, r.right - 24 * FONT_CX - 12, r.bottom - 28,
            "xv6 Classic Desktop", -1);
    for(int i = 0; i < NDESK; i++){
      int cy = 8 + i * DESK_CELL;
      draw_icon(dc, 4 + 24, cy + 4, desk_items[i].icon);
      const char *s = desk_items[i].label;
      int tw = strlen(s) * FONT_CX;
      int tx = 4 + 40 - tw / 2;
      if(i == desk_sel){
        RECT hl = { tx - 1, cy + 40, tx + tw + 1, cy + 40 + FONT_CY };
        FillRect(dc, &hl, CLR_SELECT);
      }
      SetTextColor(dc, CLR_HILIGHT);
      TextOut(dc, tx, cy + 40, s, -1);
    }
    EndPaint(w, &ps);
    return 0;
  }
  case WM_LBUTTONDOWN:
    desk_sel = desk_hit(LOWORD(lp), HIWORD(lp));
    InvalidateRect(w, 0, 1);
    return 0;
  case WM_LBUTTONDBLCLK: {
    int i = desk_hit(LOWORD(lp), HIWORD(lp));
    desk_sel = i;
    InvalidateRect(w, 0, 1);
    if(i >= 0)
      desk_launch(i);
    return 0;
  }
  case WM_KEYDOWN:
    if(wp == VK_DOWN && desk_sel + 1 < NDESK) desk_sel++;
    else if(wp == VK_UP && desk_sel > 0) desk_sel--;
    else if(wp == VK_RETURN && desk_sel >= 0) desk_launch(desk_sel);
    InvalidateRect(w, 0, 1);
    return 0;
  case WM_RBUTTONDOWN: {
    static const char *items[] = { "Arrange Icons", 0, "About xv6..." };
    RECT r;
    GetWindowRect(w, &r);
    TrackPopupMenu(items, 3, r.left + LOWORD(lp), r.top + HIWORD(lp), w, 500);
    return 0;
  }
  case WM_COMMAND:
    if(LOWORD(wp) == 502)
      MessageBox(w, about_text, "About xv6", MB_OK);
    else if(LOWORD(wp) == 500){
      desk_sel = -1;
      InvalidateRect(w, 0, 1);
    }
    return 0;
  }
  return DefWindowProc(w, msg, wp, lp);
}

// ===========================================================================
// Taskbar.

#define START_W 64
#define TRAY_W  76
#define TBTN_MAX 140

static HWND task_wins[16];
static int ntask;
static HWND start_menu;

static void
task_layout(int i, RECT *r)
{
  int avail = GetSystemMetrics(SM_CXSCREEN) - START_W - TRAY_W - 12;
  int bw = ntask ? avail / ntask : TBTN_MAX;
  if(bw > TBTN_MAX)
    bw = TBTN_MAX;
  r->left = START_W + 6 + i * bw;
  r->right = r->left + bw - 3;
  r->top = 4;
  r->bottom = TASKBAR_H - 2;
}

static const char *start_items[] = {
  "Explorer", "Notepad", "Calculator", "Command Prompt", "Task Manager",
  0, "About xv6...", "Shut Down...",
};
#define START_BASE 100

static LRESULT
TaskbarProc(HWND w, uint msg, WPARAM wp, LPARAM lp)
{
  switch(msg){
  case WM_CREATE:
    SetTimer(w, 1, 1000);
    return 0;
  case WM_TIMER:
  case WM_SHELLHOOK:
    InvalidateRect(w, 0, 1);
    return 0;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    RECT r;
    GetClientRect(w, &r);
    RECT line = { 0, 0, r.right, 1 };
    FillRect(dc, &line, CLR_LIGHT);
    RECT line2 = { 0, 1, r.right, 2 };
    FillRect(dc, &line2, CLR_HILIGHT);

    // Start button.
    RECT sb = { 2, 4, START_W, TASKBAR_H - 2 };
    int start_down = IsWindow(start_menu);
    if(start_down){
      FrameRect(dc, &sb, CLR_DKSHADOW);
    } else {
      DrawEdge(dc, &sb, EDGE_RAISED);
    }
    int o = start_down ? 1 : 0;
    box(dc, 7 + o, 9 + o, 6, 6, RGB(0, 160, 0));
    box(dc, 14 + o, 9 + o, 6, 6, RGB(0, 90, 200));
    box(dc, 7 + o, 16 + o, 13, 1, CLR_SHADOW);
    SetTextColor(dc, CLR_TEXT);
    TextOut(dc, 24 + o, 6 + o, "Start", 5);
    TextOut(dc, 25 + o, 6 + o, "Start", 5);       // poor man's bold

    // One button per top-level window.
    ntask = WinEnumTopLevel(task_wins, 16);
    HWND act = GetActiveWindow();
    for(int i = 0; i < ntask; i++){
      RECT b;
      task_layout(i, &b);
      int pressed = task_wins[i] == act && !IsIconic(task_wins[i]);
      if(pressed){
        FillRect(dc, &b, CLR_LIGHT);
        DrawEdge(dc, &b, EDGE_SUNKEN);
      } else {
        DrawEdge(dc, &b, EDGE_RAISED);
      }
      char title[64];
      GetWindowText(task_wins[i], title, sizeof(title));
      int maxc = (b.right - b.left - 10) / FONT_CX;
      if(maxc < 0) maxc = 0;
      int n = strlen(title);
      if(n > maxc) n = maxc;
      RECT t = { b.left + 5 + pressed, b.top + pressed, b.right - 3,
                 b.bottom + pressed };
      SetTextColor(dc, CLR_TEXT);
      DrawText(dc, title, n, &t, DT_LEFT);
    }

    // Tray clock.
    RECT tray = { r.right - TRAY_W, 4, r.right - 3, TASKBAR_H - 2 };
    DrawEdge(dc, &tray, EDGE_SUNKEN);
    char clk[16];
    struct timespec ts;
    if(clock_gettime(CLOCK_REALTIME, &ts) == 0 && ts.tv_sec > 1600000000){
      long t = ts.tv_sec % 86400;
      two(clk, t / 3600); clk[2] = ':'; two(clk + 3, (t / 60) % 60);
      clk[5] = 0;
    } else {
      long t = clock_us() / 1000000;
      two(clk, t / 3600); clk[2] = ':'; two(clk + 3, (t / 60) % 60);
      clk[5] = ':'; two(clk + 6, t % 60); clk[8] = 0;
    }
    DrawText(dc, clk, -1, &tray, DT_CENTER);
    EndPaint(w, &ps);
    return 0;
  }
  case WM_LBUTTONDOWN:
  case WM_LBUTTONDBLCLK: {
    int x = LOWORD(lp), y = HIWORD(lp);
    if(x >= 2 && x < START_W){
      RECT r;
      GetWindowRect(w, &r);
      start_menu = TrackPopupMenu(start_items, 8, r.left + 2, r.top, w,
                                  START_BASE);
      InvalidateRect(w, 0, 1);
      return 0;
    }
    for(int i = 0; i < ntask; i++){
      RECT b;
      task_layout(i, &b);
      if(x >= b.left && x < b.right && y >= b.top && y < b.bottom){
        HWND t = task_wins[i];
        if(IsWindow(t)){
          if(t == GetActiveWindow() && !IsIconic(t))
            ShowWindow(t, 0);
          else
            ShowWindow(t, 1);
        }
        InvalidateRect(w, 0, 1);
        return 0;
      }
    }
    return 0;
  }
  case WM_COMMAND: {
    int id = LOWORD(wp) - START_BASE;
    InvalidateRect(w, 0, 1);
    switch(id){
    case 0: open_explorer("/"); break;
    case 1: open_notepad(0); break;
    case 2: open_calc(); break;
    case 3: open_console(); break;
    case 4: open_taskmgr(); break;
    case 6: MessageBox(w, about_text, "About xv6", MB_OK); break;
    case 7: PostQuitMessage(0); break;
    }
    return 0;
  }
  }
  return DefWindowProc(w, msg, wp, lp);
}

// ===========================================================================
// Explorer.

#define EX_PATH 1
#define EX_UP   2
#define EX_LIST 3
#define EX_STAT 4

struct explorer {
  char path[256];
};

static void
path_join(char *dst, const char *dir, const char *name)
{
  strcpy(dst, dir);
  int n = strlen(dst);
  if(n == 0 || dst[n - 1] != '/')
    dst[n++] = '/';
  strcpy(dst + n, name);
}

static void
path_up(char *p)
{
  int n = strlen(p);
  while(n > 1 && p[n - 1] == '/')
    p[--n] = 0;
  while(n > 1 && p[n - 1] != '/')
    p[--n] = 0;
  while(n > 1 && p[n - 1] == '/')
    p[--n] = 0;
}

static void
explorer_fill(HWND w)
{
  struct explorer *ex = (struct explorer *)GetWindowLong(w, GWLP_USERDATA);
  HWND list = GetDlgItem(w, EX_LIST);
  char title[300], line[80], full[300];
  struct dirent de;
  struct stat st;
  static char names[256][DIRSIZ + 1];
  static int isdir[256], sizes[256];
  int n = 0;

  SendMessage(list, LB_RESETCONTENT, 0, 0);
  SetWindowText(GetDlgItem(w, EX_PATH), ex->path);
  strcpy(title, "Exploring - ");
  strcpy(title + strlen(title), ex->path);
  SetWindowText(w, title);

  int fd = open(ex->path, O_RDONLY);
  if(fd < 0){
    SetWindowText(GetDlgItem(w, EX_STAT), "cannot open directory");
    return;
  }
  while(n < 256 && read(fd, &de, sizeof(de)) == sizeof(de)){
    if(de.inum == 0)
      continue;
    memmove(names[n], de.name, DIRSIZ);
    names[n][DIRSIZ] = 0;
    if(strcmp(names[n], ".") == 0)
      continue;
    path_join(full, ex->path, names[n]);
    isdir[n] = stat(full, &st) == 0 && st.type == T_DIR;
    sizes[n] = isdir[n] ? 0 : (int)st.size;
    n++;
  }
  close(fd);
  // Folders first, then files; each group by name (insertion sort).
  for(int i = 1; i < n; i++)
    for(int j = i; j > 0; j--){
      int a = j - 1, b = j;
      int swap = isdir[b] > isdir[a] ||
                 (isdir[b] == isdir[a] && strcmp(names[b], names[a]) < 0);
      if(!swap)
        break;
      char t[DIRSIZ + 1];
      memmove(t, names[a], sizeof(t));
      memmove(names[a], names[b], sizeof(t));
      memmove(names[b], t, sizeof(t));
      int ti = isdir[a]; isdir[a] = isdir[b]; isdir[b] = ti;
      ti = sizes[a]; sizes[a] = sizes[b]; sizes[b] = ti;
    }
  int files = 0, dirs = 0;
  for(int i = 0; i < n; i++){
    if(isdir[i]){
      strcpy(line, "[");
      strcpy(line + 1, names[i]);
      strcpy(line + strlen(line), "]");
      dirs++;
    } else {
      strcpy(line, names[i]);
      int l = strlen(line);
      while(l < 32)
        line[l++] = ' ';
      itoa_(line + l, sizes[i]);
      strcpy(line + strlen(line), " B");
      files++;
    }
    SendMessage(list, LB_ADDSTRING, 0, (LPARAM)line);
  }
  char s[64];
  itoa_(s, dirs);
  strcpy(s + strlen(s), " folders, ");
  itoa_(s + strlen(s), files);
  strcpy(s + strlen(s), " files");
  SetWindowText(GetDlgItem(w, EX_STAT), s);
}

static void
explorer_open_selected(HWND w)
{
  struct explorer *ex = (struct explorer *)GetWindowLong(w, GWLP_USERDATA);
  HWND list = GetDlgItem(w, EX_LIST);
  int i = SendMessage(list, LB_GETCURSEL, 0, 0);
  char item[128], name[64], full[300];
  if(i < 0)
    return;
  SendMessage(list, LB_GETTEXT, i, (LPARAM)item);
  if(item[0] == '['){
    int n = strlen(item) - 2;
    memmove(name, item + 1, n);
    name[n] = 0;
    if(strcmp(name, "..") == 0)
      path_up(ex->path);
    else{
      path_join(full, ex->path, name);
      scopy(ex->path, full, sizeof(ex->path));
    }
    explorer_fill(w);
  } else {
    int n = 0;
    while(item[n] && item[n] != ' ' && n < 63){
      name[n] = item[n];
      n++;
    }
    name[n] = 0;
    path_join(full, ex->path, name);
    open_notepad(full);
  }
}

static LRESULT
ExplorerProc(HWND w, uint msg, WPARAM wp, LPARAM lp)
{
  switch(msg){
  case WM_CREATE: {
    struct explorer *ex = malloc(sizeof(*ex));
    if(!ex)
      return -1;
    scopy(ex->path, lp ? (const char *)lp : "/", sizeof(ex->path));
    SetWindowLong(w, GWLP_USERDATA, (long)ex);
    RECT r;
    GetClientRect(w, &r);
    CreateWindow("STATIC", "Address", WS_CHILD | WS_VISIBLE, 4, 6, 64, 18,
                 w, 0, 0);
    CreateWindow("EDIT", ex->path, WS_CHILD | WS_VISIBLE | WS_BORDER,
                 70, 3, r.right - 70 - 50, 22, w, EX_PATH, 0);
    CreateWindow("BUTTON", "Up", WS_CHILD | WS_VISIBLE, r.right - 46, 3, 42,
                 22, w, EX_UP, 0);
    CreateWindow("LISTBOX", "", WS_CHILD | WS_VISIBLE | WS_BORDER, 4, 30,
                 r.right - 8, r.bottom - 30 - 24, w, EX_LIST, 0);
    CreateWindow("STATIC", "", WS_CHILD | WS_VISIBLE, 4, r.bottom - 20,
                 r.right - 8, 18, w, EX_STAT, 0);
    explorer_fill(w);
    SetFocus(GetDlgItem(w, EX_LIST));
    return 0;
  }
  case WM_COMMAND: {
    struct explorer *ex = (struct explorer *)GetWindowLong(w, GWLP_USERDATA);
    int id = LOWORD(wp), code = HIWORD(wp);
    if(id == EX_UP){
      path_up(ex->path);
      explorer_fill(w);
    } else if(id == EX_LIST && code == LBN_DBLCLK){
      explorer_open_selected(w);
    } else if(id == EX_PATH && code == VK_RETURN){
      GetWindowText(GetDlgItem(w, EX_PATH), ex->path, sizeof(ex->path));
      explorer_fill(w);
    }
    return 0;
  }
  case WM_DESTROY:
    free((void *)GetWindowLong(w, GWLP_USERDATA));
    return 0;
  }
  return DefWindowProc(w, msg, wp, lp);
}

static void
open_explorer(const char *path)
{
  CreateWindow("Explorer", "Exploring", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
               CW_USEDEFAULT, CW_USEDEFAULT, 380, 320, 0, 0, (void *)path);
}

// ===========================================================================
// Notepad.

#define NP_NAME 1
#define NP_OPEN 2
#define NP_SAVE 3
#define NP_TEXT 4
#define NP_MAX  (64 * 1024)

static void
notepad_title(HWND w, const char *path)
{
  char t[128];
  const char *base = path;
  for(const char *p = path; *p; p++)
    if(*p == '/')
      base = p + 1;
  strcpy(t, *base ? base : "Untitled");
  strcpy(t + strlen(t), " - Notepad");
  SetWindowText(w, t);
}

static void
notepad_load(HWND w)
{
  char path[256];
  GetWindowText(GetDlgItem(w, NP_NAME), path, sizeof(path));
  int fd = open(path, O_RDONLY);
  if(fd < 0){
    MessageBox(w, "Cannot open the file.", "Notepad", MB_OK);
    return;
  }
  char *buf = malloc(NP_MAX + 1);
  int n = 0, r;
  while(buf && n < NP_MAX && (r = read(fd, buf + n, NP_MAX - n)) > 0)
    n += r;
  close(fd);
  if(!buf)
    return;
  // Show only text; replace other bytes so binaries do not garble the view.
  for(int i = 0; i < n; i++)
    if((buf[i] < ' ' || buf[i] > '~') && buf[i] != '\n')
      buf[i] = buf[i] == '\t' ? ' ' : '.';
  buf[n] = 0;
  SetWindowText(GetDlgItem(w, NP_TEXT), buf);
  free(buf);
  notepad_title(w, path);
}

static void
notepad_save(HWND w)
{
  char path[256];
  GetWindowText(GetDlgItem(w, NP_NAME), path, sizeof(path));
  if(path[0] == 0){
    MessageBox(w, "Type a file name first.", "Notepad", MB_OK);
    return;
  }
  HWND ed = GetDlgItem(w, NP_TEXT);
  int len = GetWindowTextLength(ed);
  char *buf = malloc(len + 1);
  if(!buf)
    return;
  GetWindowText(ed, buf, len + 1);
  int fd = open(path, O_CREATE | O_WRONLY | O_TRUNC);
  int ok = fd >= 0 && write(fd, buf, len) == len;
  if(fd >= 0)
    close(fd);
  free(buf);
  if(ok){
    notepad_title(w, path);
    MessageBox(w, "Saved.", "Notepad", MB_OK);
  } else {
    MessageBox(w, "Cannot write the file.", "Notepad", MB_OK);
  }
}

static LRESULT
NotepadProc(HWND w, uint msg, WPARAM wp, LPARAM lp)
{
  switch(msg){
  case WM_CREATE: {
    RECT r;
    GetClientRect(w, &r);
    const char *path = lp ? (const char *)lp : "";
    CreateWindow("STATIC", "File", WS_CHILD | WS_VISIBLE, 4, 6, 36, 18, w, 0, 0);
    CreateWindow("EDIT", path, WS_CHILD | WS_VISIBLE | WS_BORDER, 42, 3,
                 r.right - 42 - 108, 22, w, NP_NAME, 0);
    CreateWindow("BUTTON", "Open", WS_CHILD | WS_VISIBLE, r.right - 104, 3,
                 50, 22, w, NP_OPEN, 0);
    CreateWindow("BUTTON", "Save", WS_CHILD | WS_VISIBLE, r.right - 52, 3,
                 48, 22, w, NP_SAVE, 0);
    HWND ed = CreateWindow("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER |
                           ES_MULTILINE, 2, 29, r.right - 4, r.bottom - 31,
                           w, NP_TEXT, 0);
    if(path[0])
      notepad_load(w);
    else
      notepad_title(w, "");
    SetFocus(ed);
    return 0;
  }
  case WM_COMMAND:
    if(LOWORD(wp) == NP_OPEN || (LOWORD(wp) == NP_NAME && HIWORD(wp) == VK_RETURN))
      notepad_load(w);
    else if(LOWORD(wp) == NP_SAVE)
      notepad_save(w);
    return 0;
  }
  return DefWindowProc(w, msg, wp, lp);
}

static void
open_notepad(const char *path)
{
  CreateWindow("Notepad", "Notepad", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
               CW_USEDEFAULT, CW_USEDEFAULT, 440, 320, 0, 0, (void *)path);
}

// ===========================================================================
// Calculator: integer arithmetic, evaluated left to right like the real one.

struct calc {
  long acc, cur;
  char op;
  int fresh;                    // the next digit starts a new number
  int error;
};

static const char *calc_keys[] = {
  "7", "8", "9", "/",
  "4", "5", "6", "*",
  "1", "2", "3", "-",
  "0", "C", "=", "+",
};

static void
calc_show(HWND w, struct calc *c)
{
  char s[32];
  if(c->error)
    strcpy(s, "Error ");
  else{
    itoa_(s, c->cur);
    strcpy(s + strlen(s), " ");
  }
  SetWindowText(GetDlgItem(w, 100), s);
}

static void
calc_apply(struct calc *c)
{
  switch(c->op){
  case '+': c->acc += c->cur; break;
  case '-': c->acc -= c->cur; break;
  case '*': c->acc *= c->cur; break;
  case '/':
    if(c->cur == 0){ c->error = 1; return; }
    c->acc /= c->cur;
    break;
  default: c->acc = c->cur;
  }
  c->cur = c->acc;
}

static void
calc_key(HWND w, char k)
{
  struct calc *c = (struct calc *)GetWindowLong(w, GWLP_USERDATA);
  if(k == 'C' || k == 'c' || k == 0x1b){
    memset(c, 0, sizeof(*c));
  } else if(c->error){
    return;
  } else if(k >= '0' && k <= '9'){
    if(c->fresh){ c->cur = 0; c->fresh = 0; }
    if(c->cur < 100000000000000L)
      c->cur = c->cur * 10 + (k - '0');
  } else if(k == '+' || k == '-' || k == '*' || k == '/'){
    calc_apply(c);
    c->op = k;
    c->fresh = 1;
  } else if(k == '=' || k == '\r'){
    calc_apply(c);
    c->op = 0;
    c->fresh = 1;
  } else if(k == '\b'){
    c->cur /= 10;
  }
  calc_show(w, c);
}

static LRESULT
CalcProc(HWND w, uint msg, WPARAM wp, LPARAM lp)
{
  switch(msg){
  case WM_CREATE: {
    struct calc *c = malloc(sizeof(*c));
    if(!c)
      return -1;
    memset(c, 0, sizeof(*c));
    SetWindowLong(w, GWLP_USERDATA, (long)c);
    CreateWindow("STATIC", "0 ", WS_CHILD | WS_VISIBLE | WS_BORDER |
                 ES_READONLY | ES_RIGHT, 8, 8, 4 * 44 - 4, 26, w, 100, 0);
    for(int i = 0; i < 16; i++)
      CreateWindow("BUTTON", calc_keys[i], WS_CHILD | WS_VISIBLE,
                   8 + (i % 4) * 44, 44 + (i / 4) * 34, 40, 30, w,
                   200 + i, 0);
    return 0;
  }
  case WM_COMMAND:
    if(LOWORD(wp) >= 200 && LOWORD(wp) < 216)
      calc_key(w, calc_keys[LOWORD(wp) - 200][0]);
    SetFocus(w);              // keep typing on the calculator itself
    return 0;
  case WM_CHAR:
    calc_key(w, (char)wp);
    return 0;
  case WM_DESTROY:
    free((void *)GetWindowLong(w, GWLP_USERDATA));
    return 0;
  }
  return DefWindowProc(w, msg, wp, lp);
}

static void
open_calc(void)
{
  CreateWindow("Calculator", "Calculator", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
               CW_USEDEFAULT, CW_USEDEFAULT, 4 * 44 + 12 + 6,
               44 + 4 * 34 + 8 + 18 + 6, 0, 0, 0);
}

// ===========================================================================
// Command Prompt: a terminal emulator on a pty running /bin/sh.

#define CON_COLS 64
#define CON_ROWS 20

struct console {
  char ch[CON_ROWS][CON_COLS];
  int cx, cy, esc;
  int master, pid;
};

static void
con_scroll(struct console *c)
{
  memmove(c->ch[0], c->ch[1], (CON_ROWS - 1) * CON_COLS);
  memset(c->ch[CON_ROWS - 1], ' ', CON_COLS);
}

static void
con_put(struct console *c, int ch)
{
  if(c->esc == 1){
    c->esc = ch == '[' ? 2 : 0;
    return;
  }
  if(c->esc == 2){
    if(ch >= '@' && ch <= '~'){
      c->esc = 0;
      if(ch == 'K')
        for(int x = c->cx; x < CON_COLS; x++)
          c->ch[c->cy][x] = ' ';
      else if(ch == 'J'){
        memset(c->ch, ' ', sizeof(c->ch));
        c->cx = c->cy = 0;
      }
    }
    return;
  }
  if(ch == 0x1b){ c->esc = 1; return; }
  if(ch == '\r'){ c->cx = 0; return; }
  if(ch == '\b'){ if(c->cx) c->cx--; return; }
  if(ch == '\t'){ do con_put(c, ' '); while(c->cx % 8); return; }
  if(ch == '\n' || c->cx >= CON_COLS){
    c->cx = 0;
    if(++c->cy >= CON_ROWS){
      c->cy = CON_ROWS - 1;
      con_scroll(c);
    }
    if(ch == '\n')
      return;
  }
  if(ch < ' ' || ch > '~')
    return;
  c->ch[c->cy][c->cx++] = ch;
}

static LRESULT
ConsoleProc(HWND w, uint msg, WPARAM wp, LPARAM lp)
{
  struct console *c = (struct console *)GetWindowLong(w, GWLP_USERDATA);
  switch(msg){
  case WM_CREATE: {
    int fd[2];
    c = malloc(sizeof(*c));
    if(!c)
      return -1;
    memset(c, 0, sizeof(*c));
    memset(c->ch, ' ', sizeof(c->ch));
    c->master = -1;
    SetWindowLong(w, GWLP_USERDATA, (long)c);
    if(pty_open(fd) < 0)
      return 0;
    int pid = fork();
    if(pid == 0){
      close(fd[0]);
      for(int i = 0; i < 3; i++){
        close(i);
        dup(fd[1]);
      }
      for(int i = 3; i < 32; i++)
        close(i);
      char *argv[] = { "sh", 0 };
      exec("/bin/sh", argv);
      exit(127);
    }
    close(fd[1]);
    if(pid < 0){
      close(fd[0]);
      return 0;
    }
    c->master = fd[0];
    c->pid = pid;
    WinWatchFd(c->master, w);
    const char *banner = "xv6 Command Prompt (/bin/sh on a pty)\r\n\r\n";
    while(*banner)
      con_put(c, *banner++);
    return 0;
  }
  case WM_FDREADY: {
    char buf[512];
    int n = read(c->master, buf, sizeof(buf));
    if(n <= 0){
      WinUnwatchFd(c->master);
      close(c->master);
      c->master = -1;
      if(c->pid > 0)
        waitpid(c->pid, 0, 0);
      c->pid = 0;
      DestroyWindow(w);         // "exit" closes the window, as in Windows
      return 0;
    }
    for(int i = 0; i < n; i++)
      con_put(c, buf[i]);
    InvalidateRect(w, 0, 1);
    return 0;
  }
  case WM_CHAR:
    if(c->master >= 0){
      char ch = wp;
      if(ch != 0x1b)
        write(c->master, &ch, 1);
    }
    return 0;
  case WM_KEYDOWN: {
    const char *seq = wp == VK_UP ? "\033[A" : wp == VK_DOWN ? "\033[B" :
                      wp == VK_RIGHT ? "\033[C" : wp == VK_LEFT ? "\033[D" : 0;
    if(seq && c->master >= 0)
      write(c->master, seq, 3);
    return 0;
  }
  case WM_SETFOCUS:
  case WM_KILLFOCUS:
    InvalidateRect(w, 0, 1);
    return 0;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    SetTextColor(dc, RGB(192, 192, 192));
    for(int r = 0; r < CON_ROWS; r++)
      TextOut(dc, 2, 2 + r * FONT_CY, c->ch[r], CON_COLS);
    if(GetFocus() == w && c->master >= 0){
      int cx = c->cx >= CON_COLS ? CON_COLS - 1 : c->cx;
      RECT cur = { 2 + cx * FONT_CX, 2 + c->cy * FONT_CY + 13,
                   2 + (cx + 1) * FONT_CX, 2 + c->cy * FONT_CY + 16 };
      FillRect(dc, &cur, RGB(192, 192, 192));
    }
    EndPaint(w, &ps);
    return 0;
  }
  case WM_DESTROY:
    if(c){
      if(c->master >= 0){
        WinUnwatchFd(c->master);
        close(c->master);
      }
      if(c->pid > 0){
        kill(c->pid);
        waitpid(c->pid, 0, 0);
      }
      free(c);
    }
    return 0;
  }
  return DefWindowProc(w, msg, wp, lp);
}

static void
open_console(void)
{
  CreateWindow("Console", "Command Prompt", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
               CW_USEDEFAULT, CW_USEDEFAULT, CON_COLS * FONT_CX + 4 + 6,
               CON_ROWS * FONT_CY + 4 + 18 + 6, 0, 0, 0);
}

// ===========================================================================
// Task Manager.

#define TM_LIST 1
#define TM_MEM  2
#define TM_KILL 3

static int
meminfo_kb(const char *buf, const char *key)
{
  int n = strlen(key);
  for(const char *p = buf; *p; ){
    if(strncmp(p, key, n) == 0 && p[n] == ':'){
      p += n + 1;
      while(*p == ' ')
        p++;
      return atoi(p);
    }
    while(*p && *p != '\n')
      p++;
    if(*p)
      p++;
  }
  return -1;
}

static void
taskmgr_refresh(HWND w)
{
  static char buf[4096];
  HWND list = GetDlgItem(w, TM_LIST);
  int sel = SendMessage(list, LB_GETCURSEL, 0, 0);
  int n = ps(buf, sizeof(buf) - 1);
  SendMessage(list, LB_RESETCONTENT, 0, 0);
  if(n > 0){
    buf[n] = 0;
    char *s = buf;
    while(*s){
      char *e = s;
      while(*e && *e != '\n')
        e++;
      char save = *e;
      *e = 0;
      if(e > s)
        SendMessage(list, LB_ADDSTRING, 0, (LPARAM)s);
      *e = save;
      s = *e ? e + 1 : e;
    }
  }
  if(sel >= 0)
    SendMessage(list, LB_SETCURSEL, sel, 0);

  char mem[1024], line[96];
  int fd = open("/proc/meminfo", O_RDONLY), m = 0;
  if(fd >= 0){
    m = read(fd, mem, sizeof(mem) - 1);
    close(fd);
  }
  mem[m > 0 ? m : 0] = 0;
  int total = meminfo_kb(mem, "MemTotal"), freek = meminfo_kb(mem, "MemFree");
  strcpy(line, "Memory: ");
  itoa_(line + strlen(line), (total - freek) / 1024);
  strcpy(line + strlen(line), " MB used of ");
  itoa_(line + strlen(line), total / 1024);
  strcpy(line + strlen(line), " MB    Up ");
  long t = clock_us() / 1000000;
  int l = strlen(line);
  two(line + l, t / 3600); line[l + 2] = ':';
  two(line + l + 3, (t / 60) % 60); line[l + 5] = ':';
  two(line + l + 6, t % 60); line[l + 8] = 0;
  SetWindowText(GetDlgItem(w, TM_MEM), line);
}

static LRESULT
TaskMgrProc(HWND w, uint msg, WPARAM wp, LPARAM lp)
{
  switch(msg){
  case WM_CREATE: {
    RECT r;
    GetClientRect(w, &r);
    CreateWindow("LISTBOX", "", WS_CHILD | WS_VISIBLE | WS_BORDER, 4, 4,
                 r.right - 8, r.bottom - 60, w, TM_LIST, 0);
    CreateWindow("STATIC", "", WS_CHILD | WS_VISIBLE, 4, r.bottom - 52,
                 r.right - 8, 18, w, TM_MEM, 0);
    CreateWindow("BUTTON", "End Process", WS_CHILD | WS_VISIBLE,
                 r.right - 110, r.bottom - 30, 104, 26, w, TM_KILL, 0);
    taskmgr_refresh(w);
    SetTimer(w, 1, 2000);
    return 0;
  }
  case WM_TIMER:
    taskmgr_refresh(w);
    return 0;
  case WM_COMMAND:
    if(LOWORD(wp) == TM_KILL){
      HWND list = GetDlgItem(w, TM_LIST);
      int i = SendMessage(list, LB_GETCURSEL, 0, 0);
      char line[160];
      if(i < 0 || SendMessage(list, LB_GETTEXT, i, (LPARAM)line) < 0){
        MessageBox(w, "Select a process first.", "Task Manager", MB_OK);
        return 0;
      }
      char *p = line;
      while(*p && (*p < '0' || *p > '9'))
        p++;
      int pid = atoi(p);
      if(pid <= 2 || pid == getpid()){
        MessageBox(w, "That process cannot be ended.", "Task Manager", MB_OK);
        return 0;
      }
      kill(pid);
      taskmgr_refresh(w);
    }
    return 0;
  }
  return DefWindowProc(w, msg, wp, lp);
}

static void
open_taskmgr(void)
{
  CreateWindow("TaskManager", "Task Manager", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
               CW_USEDEFAULT, CW_USEDEFAULT, 500, 300, 0, 0, 0);
}

// ===========================================================================

int
main(int argc, char **argv)
{
  if(WinInit() < 0)
    exit(1);
  static const WNDCLASS classes[] = {
    { "Desktop", DesktopProc, CLR_DESKTOP, 0 },
    { "Taskbar", TaskbarProc, CLR_FACE, 0 },
    { "Explorer", ExplorerProc, CLR_FACE, 0 },
    { "Notepad", NotepadProc, CLR_FACE, 0 },
    { "Calculator", CalcProc, CLR_FACE, 0 },
    { "Console", ConsoleProc, RGB(0, 0, 0), 0 },
    { "TaskManager", TaskMgrProc, CLR_FACE, 0 },
  };
  for(uint i = 0; i < sizeof(classes) / sizeof(classes[0]); i++)
    RegisterClass(&classes[i]);

  int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
  taskbar = CreateWindow("Taskbar", "", WS_POPUP | WS_TOPMOST | WS_NOTASKBAR |
                         WS_VISIBLE, 0, sh - TASKBAR_H, sw, TASKBAR_H, 0, 0, 0);
  WinSetTaskbar(taskbar);
  desktop = CreateWindow("Desktop", "", WS_POPUP | WS_BOTTOM | WS_NOTASKBAR |
                         WS_VISIBLE, 0, 0, sw, sh - TASKBAR_H, 0, 0, 0);
  SetActiveWindow(desktop);
  for(int i = 1; i < argc; i++){         // win notepad /README, etc.
    if(strcmp(argv[i], "explorer") == 0) open_explorer("/");
    else if(strcmp(argv[i], "notepad") == 0) open_notepad(i + 1 < argc ? argv[++i] : 0);
    else if(strcmp(argv[i], "calc") == 0) open_calc();
    else if(strcmp(argv[i], "cmd") == 0) open_console();
    else if(strcmp(argv[i], "taskmgr") == 0) open_taskmgr();
  }
  printf("win: desktop up; Start > Shut Down returns here\n");

  MSG msg;
  while(GetMessage(&msg, 0, 0, 0))
    DispatchMessage(&msg);
  WinExit();
  printf("win: shut down\n");
  exit(0);
}
