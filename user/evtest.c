#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"

struct input_event {
  uint16 type;
  uint16 code;
  int value;
};

int
main(int argc, char **argv)
{
  char *path = argc > 1 ? argv[1] : "/dev/input/event0";
  struct input_event events[16];
  int fd = open(path, O_RDONLY);
  if(fd < 0){
    printf("evtest: cannot open %s (empty input slot?)\n", path);
    exit(1);
  }
  printf("evtest: reading %s; unplug returns from blocked read\n", path);
  for(;;){
    int n = read(fd, events, sizeof(events));
    if(n <= 0){
      printf("evtest: %s disconnected\n", path);
      break;
    }
    for(int i = 0; i + (int)sizeof(events[0]) <= n;
        i += sizeof(events[0])){
      struct input_event *ev = (struct input_event *)((char *)events + i);
      printf("event type=%d code=%d value=%d\n",
             ev->type, ev->code, ev->value);
    }
  }
  close(fd);
  exit(0);
}
