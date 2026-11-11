// mpirun -np N prog [args...]
//
// Runs prog with "-np N" inserted after argv[0]; MPI_Init in prog strips it
// and forks the N ranks itself (shared memory set up before exec would not
// survive exec, so the launcher cannot create it).
#include <uapi/types.h>
#include "user/user.h"

#define MAXARGS 32

int
main(int argc, char *argv[])
{
  char *av[MAXARGS + 3];
  char path[128];
  int i, n;

  if(argc < 4 || strcmp(argv[1], "-np") != 0){
    fprintf(2, "usage: mpirun -np N prog [args...]\n");
    exit(1);
  }
  n = atoi(argv[2]);
  if(n < 1 || argc - 3 > MAXARGS){
    fprintf(2, "mpirun: bad -np or too many arguments\n");
    exit(1);
  }

  av[0] = argv[3];
  av[1] = "-np";
  av[2] = argv[2];
  for(i = 4; i < argc; i++)
    av[i - 1] = argv[i];
  av[argc - 1] = 0;

  exec(argv[3], av);
  if(strchr(argv[3], '/') == 0 && strlen(argv[3]) + 6 < sizeof(path)){
    strcpy(path, "/bin/");
    strcpy(path + 5, argv[3]);
    exec(path, av);
  }
  fprintf(2, "mpirun: exec %s failed\n", argv[3]);
  exit(1);
}
