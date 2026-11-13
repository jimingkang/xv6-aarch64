// cmd: Command Prompt -- /bin/sh on a pseudo-terminal, in its own process.
//
// The pty is read by a second thread, which hands the bytes over through a
// ring buffer and wakes the window with PostMessage(WM_FDREADY): the
// message lands in this process's kernel queue, so GetMessage() returns
// without polling -- the usual Windows pattern for I/O in a GUI program.
#include <uapi/types.h>
#include <uapi/stat.h>
#include "user/user.h"
#include <fcntl.h>
#include "user/winapp.h"
#include <sys/wait.h>

// ===========================================================================
// Command Prompt: a terminal emulator on a pty running /bin/sh.

#define CON_COLS 64
#define CON_ROWS 20

struct console {
  char ch[CON_ROWS][CON_COLS];
  int cx, cy, esc;
  int master, pid;
  HWND hwnd;
  thread_t reader;
  // Single-producer (reader thread) / single-consumer (window) byte ring.
  char ring[8192];
  uint head, tail;              // head: consumer, tail: producer
  int eof;
};

static void
reader_main(void *arg)
{
  struct console *c = arg;
  char buf[256];
  for(;;){
    int n = read(c->master, buf, sizeof(buf));
    if(n <= 0){
      __atomic_store_n(&c->eof, 1, __ATOMIC_RELEASE);
      PostMessage(c->hwnd, WM_FDREADY, 0, 0);
      return;
    }
    for(int i = 0; i < n; i++){
      uint t = __atomic_load_n(&c->tail, __ATOMIC_RELAXED);
      while(t - __atomic_load_n(&c->head, __ATOMIC_ACQUIRE) >= sizeof(c->ring))
        sleep(1);               // window is behind; wait for it to drain
      c->ring[t % sizeof(c->ring)] = buf[i];
      __atomic_store_n(&c->tail, t + 1, __ATOMIC_RELEASE);
    }
    PostMessage(c->hwnd, WM_FDREADY, 0, 0);
  }
}

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
    c->hwnd = w;
    const char *banner = "xv6 Command Prompt (/bin/sh on a pty)\r\n\r\n";
    while(*banner)
      con_put(c, *banner++);
    return 0;
  }
  case WM_FDREADY: {
    uint t = __atomic_load_n(&c->tail, __ATOMIC_ACQUIRE);
    while(c->head != t){
      con_put(c, c->ring[c->head % sizeof(c->ring)]);
      __atomic_store_n(&c->head, c->head + 1, __ATOMIC_RELEASE);
    }
    InvalidateRect(w, 0, 1);
    if(__atomic_load_n(&c->eof, __ATOMIC_ACQUIRE) && c->head == c->tail){
      if(c->pid > 0)
        waitpid(c->pid, 0, 0);
      c->pid = 0;
      DestroyWindow(w);         // "exit" closes the window, as in Windows
    }
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
    // The reader thread may still be blocked in read(); exit() takes the
    // whole thread group down, so just stop the shell and leave.
    if(c && c->pid > 0){
      kill(c->pid);
      waitpid(c->pid, 0, 0);
    }
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProc(w, msg, wp, lp);
}

static HWND
open_console(void)
{
  return CreateWindow("Console", "Command Prompt", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
               CW_USEDEFAULT, CW_USEDEFAULT, CON_COLS * FONT_CX + 4 + 6,
               CON_ROWS * FONT_CY + 4 + 18 + 6, 0, 0, 0);
}


int
main(void)
{
  if(WinInit() < 0)
    exit(1);
  WNDCLASS wc = { "Console", ConsoleProc, RGB(0, 0, 0), 0 };
  RegisterClass(&wc);
  HWND w = open_console();
  if(!w)
    exit(1);
  // Start reading only now that the window has its kernel handle, so the
  // reader's PostMessage goes through the kernel queue.
  struct console *c = (struct console *)GetWindowLong(w, GWLP_USERDATA);
  if(c->master >= 0)
    thread_create(&c->reader, reader_main, c);
  run_message_loop();
  exit(0);
}
