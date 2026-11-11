#ifndef XV6_PETSCPC_H
#define XV6_PETSCPC_H
#include <petscmat.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _p_PC *PC;
typedef const char *PCType;
#define PCNONE    "none"
#define PCJACOBI  "jacobi"
#define PCBJACOBI "bjacobi"
#define PCASM     "asm"
#define PCLU      "lu"

PetscErrorCode PCSetType(PC pc, PCType type);
PetscErrorCode PCGetType(PC pc, PCType *type);
PetscErrorCode PCASMSetOverlap(PC pc, PetscInt overlap);
PetscErrorCode PCSetFromOptions(PC pc);
PetscErrorCode PCSetUp(PC pc);
PetscErrorCode PCApply(PC pc, Vec x, Vec y);

#ifdef __cplusplus
}
#endif
#endif
