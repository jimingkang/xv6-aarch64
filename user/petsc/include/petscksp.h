#ifndef XV6_PETSCKSP_H
#define XV6_PETSCKSP_H
#include <petscpc.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _p_KSP *KSP;
typedef const char *KSPType;
#define KSPGMRES      "gmres"
#define KSPCG         "cg"
#define KSPRICHARDSON "richardson"
#define KSPPREONLY    "preonly"

typedef enum {
  KSP_CONVERGED_ITERATING = 0,
  KSP_CONVERGED_RTOL_NORMAL = 1,
  KSP_CONVERGED_RTOL = 2,
  KSP_CONVERGED_ATOL = 3,
  KSP_CONVERGED_ITS = 4,
  KSP_CONVERGED_HAPPY_BREAKDOWN = 5,
  KSP_DIVERGED_NULL = -2,
  KSP_DIVERGED_ITS = -3,
  KSP_DIVERGED_DTOL = -4,
  KSP_DIVERGED_BREAKDOWN = -5,
  KSP_DIVERGED_INDEFINITE_PC = -8,
  KSP_DIVERGED_NANORINF = -9,
  KSP_DIVERGED_PC_FAILED = -11
} KSPConvergedReason;

PetscErrorCode KSPCreate(MPI_Comm comm, KSP *ksp);
PetscErrorCode KSPSetType(KSP ksp, KSPType type);
PetscErrorCode KSPGMRESSetRestart(KSP ksp, PetscInt restart);
PetscErrorCode KSPGetPC(KSP ksp, PC *pc);
PetscErrorCode KSPSetTolerances(KSP ksp, PetscReal rtol, PetscReal abstol, PetscReal dtol, PetscInt maxits);
PetscErrorCode KSPSetInitialGuessNonzero(KSP ksp, PetscBool flg);
PetscErrorCode KSPSetFromOptions(KSP ksp);
PetscErrorCode KSPSetOperators(KSP ksp, Mat A, Mat P);
PetscErrorCode KSPSetUp(KSP ksp);
PetscErrorCode KSPSetUpOnBlocks(KSP ksp);
PetscErrorCode KSPSolve(KSP ksp, Vec b, Vec x);
PetscErrorCode KSPGetConvergedReason(KSP ksp, KSPConvergedReason *reason);
PetscErrorCode KSPGetIterationNumber(KSP ksp, PetscInt *its);
PetscErrorCode KSPGetResidualNorm(KSP ksp, PetscReal *rnorm);
PetscErrorCode KSPDestroy(KSP *ksp);

#ifdef __cplusplus
}
#endif
#endif
