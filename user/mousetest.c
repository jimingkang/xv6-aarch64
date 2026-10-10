#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/input.h"
#include "kernel/fcntl.h"
#include "user/user.h"

static char *
event_name(int type, int code)
{
  if(type == EV_REL){
    if(code == REL_X) return "REL_X";
    if(code == REL_Y) return "REL_Y";
    if(code == REL_WHEEL) return "REL_WHEEL";
  }
  if(type == EV_KEY){
    if(code == BTN_LEFT) return "BTN_LEFT";
    if(code == BTN_RIGHT) return "BTN_RIGHT";
    if(code == BTN_MIDDLE) return "BTN_MIDDLE";
    if(code == BTN_SIDE) return "BTN_SIDE";
    if(code == BTN_EXTRA) return "BTN_EXTRA";
  }
  if(type == EV_SYN && code == SYN_REPORT)
    return "SYN_REPORT";
  return "unknown";
}

int
main(int argc, char **argv)
{
  char *path = argc > 1 ? argv[1] : "/dev/input/event0";
  struct input_event events[16];
  int fd, n;

  fd = open(path, O_RDONLY);
  if(fd < 0){
    printf("mousetest: cannot open %s\n", path);
    exit(1);
  }
  printf("mousetest: reading %s; move, click or scroll the mouse\n", path);
  while((n = read(fd, events, sizeof(events))) > 0){
    for(int i = 0; i < n / (int)sizeof(events[0]); i++)
      printf("%s type=%d code=%d value=%d\n",
             event_name(events[i].type, events[i].code),
             events[i].type, events[i].code, events[i].value);
  }
  printf("mousetest: device disconnected\n");
  close(fd);
  exit(0);
}
