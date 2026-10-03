// Small helpers shared by chrt, taskset, nice, renice and rtlat.
#include "kernel/sched.h"

// Parse decimal (optionally negative) or 0x-prefixed hex.  Returns 1 with
// *out set, or 0 on a malformed number.
static __attribute__((unused)) int
parse_num(const char *s, int *out)
{
  int neg = 0, v = 0, any = 0;

  if(*s == '-'){
    neg = 1;
    s++;
  }
  if(s[0] == '0' && (s[1] == 'x' || s[1] == 'X')){
    for(s += 2; *s; s++, any = 1){
      int d;
      if(*s >= '0' && *s <= '9') d = *s - '0';
      else if(*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
      else if(*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
      else return 0;
      v = v * 16 + d;
    }
  } else {
    for(; *s; s++, any = 1){
      if(*s < '0' || *s > '9')
        return 0;
      v = v * 10 + (*s - '0');
    }
  }
  if(!any)
    return 0;
  *out = neg ? -v : v;
  return 1;
}

static __attribute__((unused)) const char *
policy_name(int policy)
{
  if(policy == SCHED_FIFO) return "SCHED_FIFO";
  if(policy == SCHED_RR) return "SCHED_RR";
  return "SCHED_OTHER";
}

// exec() argv[0] the way sh does: as given if it has a '/', else /bin, then /.
static __attribute__((unused)) void
exec_cmd(char **argv)
{
  char path[64];
  char *name = argv[0];
  int n;

  for(char *c = name; *c; c++)
    if(*c == '/'){
      exec(name, argv);
      return;
    }
  for(int pass = 0; pass < 2; pass++){
    const char *prefix = pass == 0 ? "/bin/" : "/";
    n = 0;
    for(const char *c = prefix; *c; c++)
      path[n++] = *c;
    for(char *c = name; *c && n < (int)sizeof(path) - 1; c++)
      path[n++] = *c;
    path[n] = 0;
    exec(path, argv);
  }
}
