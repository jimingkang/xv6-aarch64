#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/fs.h"
#include "user/user.h"
#include "user/win32.h"

#define FILE_ROWS 15
#define TERM_ROWS 14
#define TERM_COLS 56

struct file_item {
  char name[DIRSIZ + 1];
  short type;
};

static HWND desktop_window;
static HWND explorer_window;
static HWND terminal_window;
static HWND system_window;
static HWND taskbar_window;
static struct file_item files[FILE_ROWS];
static int file_count;
static char term_lines[TERM_ROWS][TERM_COLS + 1];
static int term_row;
static volatile uint term_seq;
static volatile int term_running;
static int term_master = -1;
static int shell_pid = -1;
static thread_t term_thread;

static void
barrier(void)
{
  __asm__ volatile("dmb ish" ::: "memory");
}

static int
clamp(int value, int low, int high)
{
  if(value < low)
    return low;
  if(value > high)
    return high;
  return value;
}

static void
paint_text(HDC dc, int x, int y, COLORREF color, const char *text)
{
  SetTextColor(dc, color);
  TextOut(dc, x, y, text, strlen(text));
}

static void
load_root_folder(void)
{
  struct dirent de;
  struct stat st;
  char path[DIRSIZ + 2];
  int fd;

  file_count = 0;
  fd = open("/", O_RDONLY);
  if(fd < 0)
    return;
  while(file_count < FILE_ROWS && read(fd, &de, sizeof(de)) == sizeof(de)){
    if(de.inum == 0)
      continue;
    strncpy(files[file_count].name, de.name, DIRSIZ);
    files[file_count].name[DIRSIZ] = 0;
    if(strcmp(files[file_count].name, ".") == 0 ||
       strcmp(files[file_count].name, "..") == 0)
      continue;
    path[0] = '/';
    path[1] = 0;
    strcat(path, files[file_count].name);
    files[file_count].type = stat(path, &st) < 0 ? 0 : st.type;
    file_count++;
  }
  close(fd);
}

static void
term_newline(void)
{
  if(term_row + 1 < TERM_ROWS){
    term_row++;
  } else {
    memmove(term_lines[0], term_lines[1],
            (TERM_ROWS - 1) * (TERM_COLS + 1));
    memset(term_lines[TERM_ROWS - 1], 0, TERM_COLS + 1);
  }
}

static void
term_put(char ch)
{
  int col = strlen(term_lines[term_row]);

  if(ch == '\r')
    return;
  if(ch == '\n'){
    term_newline();
    return;
  }
  if(ch == '\b' || (uchar)ch == 0x7f){
    if(col > 0)
      term_lines[term_row][col - 1] = 0;
    return;
  }
  if(ch == '\t'){
    do {
      term_put(' ');
      col = strlen(term_lines[term_row]);
    } while((col & 3) != 0);
    return;
  }
  if((uchar)ch < ' ' || (uchar)ch > '~')
    return;
  if(col >= TERM_COLS){
    term_newline();
    col = 0;
  }
  term_lines[term_row][col] = ch;
  term_lines[term_row][col + 1] = 0;
}

static void
terminal_reader(void *unused)
{
  char buffer[96];
  int n;

  (void)unused;
  while(term_running && (n = read(term_master, buffer, sizeof(buffer))) > 0){
    term_seq++;
    barrier();
    for(int i = 0; i < n; i++)
      term_put(buffer[i]);
    barrier();
    term_seq++;
    if(terminal_window)
      InvalidateRect(terminal_window, 0, 0);
  }
  term_running = 0;
  if(terminal_window)
    InvalidateRect(terminal_window, 0, 0);
}

static int
start_terminal(void)
{
  int pty[2];

  if(pty_open(pty) < 0)
    return -1;
  shell_pid = fork();
  if(shell_pid < 0){
    close(pty[0]);
    close(pty[1]);
    return -1;
  }
  if(shell_pid == 0){
    close(pty[0]);
    close(0); dup(pty[1]);
    close(1); dup(pty[1]);
    close(2); dup(pty[1]);
    if(pty[1] > 2)
      close(pty[1]);
    chdir("/");
    char *argv[] = { "-sh", "/", 0 };
    exec("/bin/sh", argv);
    exec("/sh", argv);
    exit(127);
  }
  close(pty[1]);
  term_master = pty[0];
  term_running = 1;
  if(thread_create(&term_thread, terminal_reader, 0) < 0){
    term_running = 0;
    kill(shell_pid);
    close(term_master);
    term_master = -1;
    return -1;
  }
  const char *hello = "echo Tiny210 graphical terminal; pwd; ls /\n";
  write(term_master, hello, strlen(hello));
  return 0;
}

static void
stop_terminal(void)
{
  if(shell_pid > 0)
    kill(shell_pid);
  if(term_thread.tid >= 0)
    thread_join(&term_thread, 0);
  if(term_master >= 0)
    close(term_master);
  if(shell_pid > 0)
    wait(0);
  term_master = shell_pid = -1;
}

static void
paint_desktop(HWND w)
{
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(w, &ps);
  RECT rc;

  if(!dc)
    return;
  GetClientRect(w, &rc);
  for(int y = 0; y < rc.bottom; y += 40){
    RECT band = {0, y, rc.right, y + 40};
    FillRect(dc, &band, RGB(25, 92 + y / 16, 145 + y / 24));
  }
  RECT computer = {28, 30, 74, 72};
  FillRect(dc, &computer, RGB(224, 232, 240));
  RECT screen = {34, 36, 68, 59};
  FillRect(dc, &screen, RGB(22, 46, 70));
  paint_text(dc, 19, 78, RGB(255,255,255), "Computer");
  RECT folder = {29, 118, 75, 151};
  FillRect(dc, &folder, RGB(247, 194, 55));
  paint_text(dc, 23, 158, RGB(255,255,255), "Documents");
  paint_text(dc, rc.right - 210, rc.bottom - 58, RGB(225,240,255),
             "xv6 Tiny210 desktop");
  EndPaint(w, &ps);
}

static void
paint_explorer(HWND w)
{
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(w, &ps);
  RECT r;

  if(!dc)
    return;
  r=(RECT){3,27,367,48};FillRect(dc,&r,RGB(238,241,245));
  paint_text(dc,10,29,RGB(30,30,30),"File  View  Help");
  r=(RECT){8,54,360,78};FillRect(dc,&r,RGB(255,255,255));
  paint_text(dc,16,58,RGB(35,62,92),"Address:  /");
  paint_text(dc,12,86,RGB(85,85,85),"Name");
  paint_text(dc,278,86,RGB(85,85,85),"Type");
  r=(RECT){8,104,360,106};FillRect(dc,&r,RGB(210,216,224));
  for(int i = 0; i < file_count; i++){
    int y = 112 + i * 16;
    if(y + 16 >= 356)
      break;
    COLORREF icon = files[i].type == T_DIR ? RGB(242,190,48) : RGB(178,198,220);
    r=(RECT){13,y+2,24,y+13};FillRect(dc,&r,icon);
    paint_text(dc,31,y,RGB(20,20,20),files[i].name);
    paint_text(dc,278,y,RGB(80,80,80),
               files[i].type == T_DIR ? "Folder" : "File");
  }
  if(file_count == 0)
    paint_text(dc,16,118,RGB(170,30,30),"Unable to read /");
  EndPaint(w, &ps);
}

static void
paint_terminal(HWND w)
{
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(w, &ps);
  char snapshot[TERM_ROWS][TERM_COLS + 1];
  uint before, after;
  RECT r;

  if(!dc)
    return;
  do {
    before = term_seq;
    barrier();
    memmove(snapshot, term_lines, sizeof(snapshot));
    barrier();
    after = term_seq;
  } while((before & 1) || before != after);
  r=(RECT){3,27,467,267};FillRect(dc,&r,RGB(12,18,24));
  for(int i = 0; i < TERM_ROWS; i++)
    if(snapshot[i][0])
      paint_text(dc,9,31+i*16,RGB(211,225,215),snapshot[i]);
  if(!term_running)
    paint_text(dc,9,31+(TERM_ROWS-1)*16,RGB(255,170,80),"[shell exited]");
  EndPaint(w, &ps);
}

static void
paint_system(HWND w)
{
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(w, &ps);
  RECT r;

  if(!dc)
    return;
  r=(RECT){12,36,50,74};FillRect(dc,&r,RGB(53,112,184));
  paint_text(dc,25,46,RGB(255,255,255),"i");
  paint_text(dc,62,34,RGB(25,25,25),"Tiny210 / S5PV210");
  paint_text(dc,62,54,RGB(65,65,65),"ARMv7 Cortex-A8");
  paint_text(dc,62,74,RGB(65,65,65),"xv6 win32k + fbdev");
  EndPaint(w, &ps);
}

static void
paint_taskbar(HWND w)
{
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(w, &ps);
  RECT r;

  if(!dc)
    return;
  r=(RECT){0,0,800,36};FillRect(dc,&r,RGB(26,34,47));
  r=(RECT){6,4,76,32};FillRect(dc,&r,RGB(28,126,72));
  paint_text(dc,18,9,RGB(255,255,255),"Start");
  r=(RECT){88,4,250,32};FillRect(dc,&r,RGB(58,70,86));
  paint_text(dc,98,9,RGB(245,245,245),"File Explorer");
  r=(RECT){258,4,400,32};FillRect(dc,&r,RGB(10,82,160));
  paint_text(dc,270,9,RGB(255,255,255),"Terminal");
  paint_text(dc,704,9,RGB(210,220,232),"Tiny210");
  EndPaint(w, &ps);
}

static LRESULT
WndProc(HWND w, uint msg, WPARAM wp, LPARAM lp)
{
  (void)lp;
  if(msg == WM_PAINT){
    if(w == desktop_window) paint_desktop(w);
    else if(w == explorer_window) paint_explorer(w);
    else if(w == terminal_window) paint_terminal(w);
    else if(w == system_window) paint_system(w);
    else if(w == taskbar_window) paint_taskbar(w);
    return 0;
  }
  if(msg == WM_CHAR && w == terminal_window && term_master >= 0){
    char ch = wp;
    write(term_master, &ch, 1);
    return 0;
  }
  if(msg == WM_DESTROY)
    return 0;
  return DefWindowProc(w,msg,wp,lp);
}

static int
register_classes(void)
{
  WNDCLASS wc;

  wc=(WNDCLASS){"desktop",WndProc,RGB(35,105,155),0};
  if(!RegisterClass(&wc)) return -1;
  wc=(WNDCLASS){"explorer",WndProc,RGB(248,249,251),0};
  if(!RegisterClass(&wc)) return -1;
  wc=(WNDCLASS){"terminal",WndProc,RGB(12,18,24),0};
  if(!RegisterClass(&wc)) return -1;
  wc=(WNDCLASS){"system",WndProc,RGB(242,244,247),0};
  if(!RegisterClass(&wc)) return -1;
  wc=(WNDCLASS){"taskbar",WndProc,RGB(26,34,47),0};
  return RegisterClass(&wc) ? 0 : -1;
}

int
main(void)
{
  int sw, sh, explorer_h, terminal_x, terminal_y, terminal_w, terminal_h;

  term_thread.tid = -1;
  if(WinInit()<0){printf("windemo: framebuffer unavailable\n");exit(1);}
  if(register_classes()<0){printf("windemo: RegisterClass failed\n");exit(1);}
  load_root_folder();
  sw=GetSystemMetrics(SM_CXSCREEN);sh=GetSystemMetrics(SM_CYSCREEN);
  explorer_h=clamp(sh-84,180,360);
  terminal_x=clamp(sw*3/8,180,320);
  terminal_y=clamp(sh/3,80,160);
  terminal_w=clamp(sw-terminal_x-18,280,480);
  terminal_h=clamp(sh-terminal_y-50,170,270);

  desktop_window=CreateWindow("desktop","Desktop",WS_VISIBLE,0,0,sw,sh,0,0,0);
  explorer_window=CreateWindow("explorer","File Explorer - /",
      WS_VISIBLE|WS_OVERLAPPEDWINDOW,18,34,370,explorer_h,0,0,0);
  system_window=CreateWindow("system","System",
      WS_VISIBLE|WS_OVERLAPPEDWINDOW,sw-278,38,260,118,0,0,0);
  terminal_window=CreateWindow("terminal","Tiny210 Terminal",
      WS_VISIBLE|WS_OVERLAPPEDWINDOW,terminal_x,terminal_y,
      terminal_w,terminal_h,0,0,0);
  taskbar_window=CreateWindow("taskbar","Taskbar",WS_VISIBLE,
      0,sh-36,sw,36,0,0,0);
  if(!desktop_window||!explorer_window||!terminal_window||!system_window||!taskbar_window){
    printf("windemo: CreateWindow failed\n");
    WinExit();
    exit(1);
  }
  start_terminal();
  SetActiveWindow(terminal_window);

  MSG m;
  while(GetMessage(&m,0,0,0)>0)
    DispatchMessage(&m);
  stop_terminal();
  WinExit();
  exit(0);
}
