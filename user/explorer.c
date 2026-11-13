// explorer: the shell -- desktop, taskbar with Start menu and clock, and
// folder windows -- in the style of ReactOS's explorer.exe.
//
// It is an ordinary process: the windows it shows on the taskbar belong to
// other processes (notepad, calc, cmd, taskmgr), which it lists, activates
// and minimizes through the kernel window manager (win32k).  Start one
// session from the serial console with
//
//   explorer &
//
// Start > Shut Down asks every window to close, ends what is left, and
// returns the screen and keyboard to the console.

#include <uapi/types.h>
#include <uapi/stat.h>
#include "user/user.h"
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <uapi/xv6fs.h>
#include <uapi/win32k.h>
#include "user/winapp.h"

#define TASKBAR_H 28

static HWND desktop, taskbar;
static void open_explorer(const char *path);
static void shut_down(HWND taskbar_w);
static void shut_down_finish(void);

// ===========================================================================
// Icons: 32x32 pictures drawn with a few rectangles, so no image files.

enum { IC_COMPUTER, IC_NOTEPAD, IC_CALC, IC_CONSOLE, IC_TASKMGR, IC_FOLDER,
       IC_FILE };


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
  case 1: spawn("/bin/notepad", 0); break;
  case 2: spawn("/bin/calc", 0); break;
  case 3: spawn("/bin/cmd", 0); break;
  case 4: spawn("/bin/taskmgr", 0); break;
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
  "user32 (win32.c) implements GDI and the\n"
  "controls; the kernel's win32k owns the windows,\n"
  "message queues and input of every process.";

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

static KWND task_wins[16];
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
    if(wp == 2){                // shutdown grace period over
      shut_down_finish();
      return 0;
    }
    while(waitpid(-1, 0, WNOHANG) > 0)
      ;                         // reap programs we started
    InvalidateRect(w, 0, 1);
    return 0;
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
    // Every process's windows, straight from the kernel.
    ntask = WinEnumWindows(task_wins, 16);
    KWND act = KwndGetActive();
    for(int i = 0; i < ntask; i++){
      RECT b;
      struct w32_info info;
      task_layout(i, &b);
      int iconic = KwndGetInfo(task_wins[i], &info) && info.minimized;
      int pressed = task_wins[i] == act && !iconic;
      if(pressed){
        FillRect(dc, &b, CLR_LIGHT);
        DrawEdge(dc, &b, EDGE_SUNKEN);
      } else {
        DrawEdge(dc, &b, EDGE_RAISED);
      }
      char title[64];
      KwndGetText(task_wins[i], title, sizeof(title));
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
        KWND t = task_wins[i];
        struct w32_info info;
        int iconic = KwndGetInfo(t, &info) && info.minimized;
        if(t == KwndGetActive() && !iconic)
          KwndShow(t, 0);
        else
          KwndActivate(t);
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
    case 1: spawn("/bin/notepad", 0); break;
    case 2: spawn("/bin/calc", 0); break;
    case 3: spawn("/bin/cmd", 0); break;
    case 4: spawn("/bin/taskmgr", 0); break;
    case 6: MessageBox(w, about_text, "About xv6", MB_OK); break;
    case 7: shut_down(w); break;
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
    spawn("/bin/notepad", full);
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
// Shut Down: ask everybody to close, then end the stragglers.

static void
shut_down(HWND tb)
{
  KWND all[64];
  int n = WinEnumAllWindows(all, 64);
  int me = getpid();
  for(int i = 0; i < n; i++){
    struct w32_info info;
    if(KwndGetInfo(all[i], &info) && info.owner != me)
      KwndPost(all[i], WM_CLOSE, 0, 0);
  }
  SetTimer(tb, 2, 1500);
}

static void
shut_down_finish(void)
{
  KWND all[64];
  int n = WinEnumAllWindows(all, 64);
  int me = getpid();
  for(int i = 0; i < n; i++){
    struct w32_info info;
    if(KwndGetInfo(all[i], &info) && info.owner != me)
      kill(info.owner);         // did not close in time
  }
  while(waitpid(-1, 0, WNOHANG) > 0)
    ;
  PostQuitMessage(0);
}

int
main(int argc, char **argv)
{
  if(WinInit() < 0)
    exit(1);
  static const WNDCLASS classes[] = {
    { "Desktop", DesktopProc, CLR_DESKTOP, 0 },
    { "Taskbar", TaskbarProc, CLR_FACE, 0 },
    { "Explorer", ExplorerProc, CLR_FACE, 0 },
  };
  for(uint i = 0; i < sizeof(classes) / sizeof(classes[0]); i++)
    RegisterClass(&classes[i]);

  int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
  desktop = CreateWindow("Desktop", "Desktop", WS_POPUP | WS_BOTTOM |
                         WS_NOTASKBAR | WS_VISIBLE, 0, 0, sw, sh - TASKBAR_H,
                         0, 0, 0);
  taskbar = CreateWindow("Taskbar", "Taskbar", WS_POPUP | WS_TOPMOST |
                         WS_NOTASKBAR | WS_VISIBLE, 0, sh - TASKBAR_H, sw,
                         TASKBAR_H, 0, 0, 0);
  if(!desktop || !taskbar){
    printf("explorer: cannot create the desktop\n");
    exit(1);
  }
  WinSetShell(taskbar);
  SetActiveWindow(desktop);
  for(int i = 1; i < argc; i++){        // explorer notepad calc ...
    char path[64] = "/bin/";
    scopy(path + 5, argv[i], sizeof(path) - 5);
    spawn(path, 0);
  }
  printf("explorer: desktop up; Start > Shut Down returns here\n");
  run_message_loop();
  WinExit();
  printf("explorer: shut down\n");
  exit(0);
}
