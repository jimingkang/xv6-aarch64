// taskmgr: processes (ps) and memory; End Process kills the selected one.
// Ending a windowed program shows the kernel reclaiming its windows.
#include <uapi/types.h>
#include <uapi/stat.h>
#include "user/user.h"
#include <fcntl.h>
#include "user/winapp.h"

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
  case WM_DESTROY:
    PostQuitMessage(0);
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


int
main(void)
{
  if(WinInit() < 0)
    exit(1);
  WNDCLASS wc = { "TaskManager", TaskMgrProc, CLR_FACE, 0 };
  RegisterClass(&wc);
  open_taskmgr();
  run_message_loop();
  exit(0);
}
