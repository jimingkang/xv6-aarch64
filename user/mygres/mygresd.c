#include <uapi/types.h>
#include "user/user.h"

// PostgreSQL-style convenience entry point.  The storage engine and client
// protocol live in /bin/mygres; this name makes the resident postmaster role
// explicit without linking a second copy of the engine into libc.
int
main(int argc, char **argv)
{
  char *server_argv[4];
  server_argv[0] = "mygres";
  server_argv[1] = "server";
  server_argv[2] = argc > 1 ? argv[1] : "4";
  server_argv[3] = 0;
  exec("/bin/mygres", server_argv);
  printf("mygresd: cannot exec /bin/mygres\n");
  exit(1);
}
