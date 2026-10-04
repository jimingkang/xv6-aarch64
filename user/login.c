#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define PASSWD_MAX 512

static void
chomp(char *s)
{
  int n = strlen(s);
  while(n > 0 && (s[n-1] == '\n' || s[n-1] == '\r'))
    s[--n] = 0;
}

// Minimal /etc/passwd parser for the educational single-user system.
// Format: name:password:uid:gid:gecos:home:shell
static int
authenticate(char *name, char *password, char *home, char *shell)
{
  char buf[PASSWD_MAX], *line, *end;
  int fd, n;

  fd = open("/etc/passwd", O_RDONLY);
  if(fd < 0)
    return -1;
  n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if(n <= 0)
    return -1;
  buf[n] = 0;

  for(line = buf; *line; line = end){
    char *field[7], *p = line;
    int valid = 1;
    end = strchr(line, '\n');
    if(end)
      *end++ = 0;
    else
      end = line + strlen(line);
    if(*line == 0 || *line == '#')
      continue;
    field[0] = p;
    for(int i = 0; i < 6; i++){
      p = strchr(p, ':');
      if(p == 0){
        valid = 0;
        break;
      }
      *p++ = 0;
      field[i+1] = p;
    }
    if(valid && strcmp(field[0], name) == 0 &&
       strcmp(field[1], password) == 0){
      strncpy(home, field[5], 63);
      home[63] = 0;
      strncpy(shell, field[6], 63);
      shell[63] = 0;
      return 0;
    }
  }
  return -1;
}

int
main(void)
{
  char name[32], password[32], home[64], shell[64];

  if(tty_attach(0) < 0){
    printf("login: cannot acquire ttyS0\n");
    exit(1);
  }
  for(int attempt = 0; attempt < 3; attempt++){
    printf("\nxv6-rpi3 login: ");
    gets(name, sizeof(name));
    chomp(name);
    printf("Password: ");
    gets(password, sizeof(password));
    chomp(password);

    if(authenticate(name, password, home, shell) == 0){
      char *argv[] = { "-sh", home, 0 };
      printf("Welcome %s\n", name);
      if(home[0] && chdir(home) < 0)
        chdir("/");
      exec(shell, argv);
      exec("/bin/sh", argv);
      printf("login: cannot execute %s\n", shell);
      exit(1);
    }
    printf("Login incorrect\n");
    sleep_ticks(20);
  }
  exit(1);
}
