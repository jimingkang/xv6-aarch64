// calc: the calculator, as its own process.
#include <uapi/types.h>
#include <uapi/stat.h>
#include "user/user.h"
#include <fcntl.h>
#include "user/winapp.h"

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
    PostQuitMessage(0);
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


int
main(void)
{
  if(WinInit() < 0)
    exit(1);
  WNDCLASS wc = { "Calculator", CalcProc, CLR_FACE, 0 };
  RegisterClass(&wc);
  open_calc();
  run_message_loop();
  exit(0);
}
