#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "linux/input.h"
#include "user/user.h"

static char *event_name(unsigned short type, unsigned short code)
{
  if(type == EV_SYN && code == SYN_REPORT)
    return "SYN_REPORT";
  if(type == EV_SYN && code == SYN_DROPPED)
    return "SYN_DROPPED";
  if(type == EV_KEY)
    return code >= BTN_MOUSE ? "EV_KEY(BTN)" : "EV_KEY";
  if(type == EV_REL)
    return code == REL_X ? "REL_X" : code == REL_Y ? "REL_Y" :
           code == REL_WHEEL ? "REL_WHEEL" : "EV_REL";
  return "UNKNOWN";
}

int
main(int argc, char **argv)
{
  char *path = argc > 1 ? argv[1] : "/dev/input/event0";
  struct input_event events[16];
  int fd = open(path, O_RDONLY);

  if(fd < 0){
    printf("evtest: cannot open %s\n", path);
    exit(1);
  }
  printf("evtest: reading %s; Tiny210 K1-K8 use KEY_1-KEY_8 (event0),\n"
         "        a USB keyboard is the next eventN\n", path);
  for(;;){
    int n = read(fd, events, sizeof(events));
    if(n <= 0){
      printf("evtest: input device disconnected\n");
      close(fd);
      exit(1);
    }
    if(n % (int)sizeof(events[0])){
      printf("evtest: short event record (%d bytes)\n", n);
      continue;
    }
    for(int i = 0; i < n / (int)sizeof(events[0]); i++)
      printf("input: %s type=%d code=%d value=%d\n",
             event_name(events[i].type, events[i].code),
             events[i].type, events[i].code, events[i].value);
  }
}
