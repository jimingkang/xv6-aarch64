// notepad: edit and save text files -- its own process, talking to the
// kernel window manager through user32 (win32.c).
//
//   notepad [FILE]
#include <uapi/types.h>
#include <uapi/stat.h>
#include "user/user.h"
#include <fcntl.h>
#include "user/winapp.h"

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
  case WM_DESTROY:
    PostQuitMessage(0);
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


int
main(int argc, char **argv)
{
  if(WinInit() < 0)
    exit(1);
  WNDCLASS wc = { "Notepad", NotepadProc, CLR_FACE, 0 };
  RegisterClass(&wc);
  open_notepad(argc > 1 ? argv[1] : 0);
  run_message_loop();
  exit(0);
}
