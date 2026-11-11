// petscsys.h -- a small PETSc subset for xv6 (user/petsc).
//
// Enough of PETSc's C API for distributed sparse linear solves on top of the
// xv6 MPI subset: parallel AIJ matrices and vectors assembled from COO
// entries, ghosted vectors, scatter-to-all, and KSP (GMRES, CG, Richardson,
// PREONLY) with PC (ASM, BJACOBI, JACOBI, LU, NONE).  Only real double
// scalars, 32-bit PetscInt and the single communicator PETSC_COMM_WORLD.
// See user/petsc/README for the differences from real PETSc.
#ifndef XV6_PETSCSYS_H
#define XV6_PETSCSYS_H
#include <mpi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int PetscErrorCode;
typedef int PetscInt;
typedef int PetscMPIInt;
typedef long PetscCount;
typedef double PetscScalar;
typedef double PetscReal;
typedef enum { PETSC_FALSE = 0, PETSC_TRUE = 1 } PetscBool;
typedef struct _p_PetscOptions *PetscOptions;

#define PETSC_SUCCESS          0
#define PETSC_ERR_MEM          55
#define PETSC_ERR_SUP          56
#define PETSC_ERR_ARG_WRONG    62
#define PETSC_ERR_ARG_OUTOFRANGE 63
#define PETSC_ERR_ARG_SIZ      60
#define PETSC_ERR_ARG_WRONGSTATE 73
#define PETSC_ERR_MPI          98
#define PETSC_ERR_LIB          76
#define PETSC_ERR_CONV_FAILED  82
#define PETSC_ERR_MAT_LU_ZRPVT 71

#define PETSC_DECIDE    (-1)
#define PETSC_DETERMINE PETSC_DECIDE
#define PETSC_DEFAULT   (-2)
#define PETSC_CURRENT   (-2)
#define PETSC_COMM_WORLD MPI_COMM_WORLD
#define MPIU_INT    MPI_INT
#define MPIU_SCALAR MPI_DOUBLE
#define MPIU_REAL   MPI_DOUBLE
#define MPIU_COUNT  MPI_LONG
#define PETSC_MAX_PATH_LEN 256

PetscErrorCode PetscInitialize(int *argc, char ***argv, const char file[], const char help[]);
PetscErrorCode PetscInitialized(PetscBool *flag);
PetscErrorCode PetscFinalize(void);
PetscErrorCode PetscError(MPI_Comm comm, int line, const char *func, const char *file,
                          PetscErrorCode n, int p, const char *mess, ...);
PetscErrorCode PetscPrintf(MPI_Comm comm, const char format[], ...);
PetscErrorCode PetscOptionsHasName(PetscOptions options, const char pre[], const char name[], PetscBool *set);
PetscErrorCode PetscOptionsGetBool(PetscOptions options, const char pre[], const char name[],
                                   PetscBool *value, PetscBool *set);
PetscErrorCode PetscOptionsGetInt(PetscOptions options, const char pre[], const char name[],
                                  PetscInt *value, PetscBool *set);
PetscErrorCode PetscOptionsGetReal(PetscOptions options, const char pre[], const char name[],
                                   PetscReal *value, PetscBool *set);
PetscErrorCode PetscOptionsGetString(PetscOptions options, const char pre[], const char name[],
                                     char value[], unsigned long len, PetscBool *set);
PetscErrorCode PetscSplitOwnership(MPI_Comm comm, PetscInt *n, PetscInt *N);
PetscErrorCode PetscBarrier(void *obj);
double PetscTime(void);

#ifdef __cplusplus
}
#endif

// Error checking.  The PetscCall forms return the error from the calling
// function; the Abort forms abort the MPI job.
#define PETSC_FUNCTION_NAME __func__
#define SETERRQ(comm, n, ...) \
  return PetscError(comm, __LINE__, PETSC_FUNCTION_NAME, __FILE__, n, 1, __VA_ARGS__)
#define PetscCall(...) do { \
    PetscErrorCode ierr_xv6_ = (__VA_ARGS__); \
    if (ierr_xv6_ != PETSC_SUCCESS) \
      return PetscError(PETSC_COMM_WORLD, __LINE__, PETSC_FUNCTION_NAME, __FILE__, ierr_xv6_, 0, " "); \
  } while (0)
#define PetscCallAbort(comm, ...) do { \
    PetscErrorCode ierr_xv6_ = (__VA_ARGS__); \
    if (ierr_xv6_ != PETSC_SUCCESS) { \
      PetscError(comm, __LINE__, PETSC_FUNCTION_NAME, __FILE__, ierr_xv6_, 0, " "); \
      MPI_Abort(comm, ierr_xv6_); \
    } \
  } while (0)
#define PetscCallMPI(...) do { \
    int ierr_xv6_ = (__VA_ARGS__); \
    if (ierr_xv6_ != MPI_SUCCESS) \
      return PetscError(PETSC_COMM_WORLD, __LINE__, PETSC_FUNCTION_NAME, __FILE__, PETSC_ERR_MPI, 1, "MPI error %d", ierr_xv6_); \
  } while (0)
#define PetscCallMPIAbort(comm, ...) do { \
    int ierr_xv6_ = (__VA_ARGS__); \
    if (ierr_xv6_ != MPI_SUCCESS) { \
      PetscError(comm, __LINE__, PETSC_FUNCTION_NAME, __FILE__, PETSC_ERR_MPI, 1, "MPI error %d", ierr_xv6_); \
      MPI_Abort(comm, PETSC_ERR_MPI); \
    } \
  } while (0)
#define CHKERRQ(n) PetscCall(n)
#define PetscFunctionBeginUser
#define PetscFunctionBegin
#define PetscFunctionReturn(x) return (x)

#endif
