#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"

static char *
value(char *text, char *name)
{
  int n = strlen(name);
  char *p = text;
  while(*p){
    if(strncmp(p, name, n) == 0 && p[n] == '='){
      char *v = p + n + 1;
      char *end = v;
      while(*end && *end != '\n' && *end != '\r')
        end++;
      *end = 0;
      return v;
    }
    while(*p && *p != '\n')
      p++;
    if(*p)
      p++;
  }
  return 0;
}

int
main(int argc, char **argv)
{
  char config[256], *ssid, *psk;
  int fd, n;

  if(argc == 3){
    ssid = argv[1];
    psk = argv[2];
  } else {
    fd = open("/etc/wifi.conf", O_RDONLY);
    if(fd < 0){
      printf("wifi: usage: wifi SSID PASSPHRASE\n");
      exit(1);
    }
    n = read(fd, config, sizeof(config) - 1);
    close(fd);
    if(n <= 0){
      printf("wifi: empty /etc/wifi.conf\n");
      exit(1);
    }
    config[n] = 0;
    psk = value(config, "psk");
    ssid = value(config, "ssid");
    if(ssid == 0 || psk == 0){
      printf("wifi: expected ssid= and psk= in /etc/wifi.conf\n");
      exit(1);
    }
  }
  printf("wifi: connecting to %s\n", ssid);
  if(wifi_connect(ssid, psk) < 0){
    printf("wifi: association failed\n");
    exit(1);
  }
  printf("wifi: associated\n");
  exit(0);
}
