#ifndef XV6_PETSCVEC_H
#define XV6_PETSCVEC_H
#include <petscsys.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _p_Vec *Vec;
typedef struct _p_VecScatter *VecScatter;
typedef enum { NOT_SET_VALUES = 0, INSERT_VALUES = 1, ADD_VALUES = 2 } InsertMode;
typedef enum { SCATTER_FORWARD = 0, SCATTER_REVERSE = 1 } ScatterMode;
typedef enum { NORM_1 = 0, NORM_2 = 1, NORM_FROBENIUS = 2, NORM_INFINITY = 3, NORM_1_AND_2 = 4 } NormType;
#define NORM_MAX NORM_INFINITY

// Parallel vectors over PETSC_COMM_WORLD (local size n, or PETSC_DECIDE).
PetscErrorCode VecCreateMPI(MPI_Comm comm, PetscInt n, PetscInt N, Vec *v);
// A sequential vector, private to the calling rank (comm is ignored).
PetscErrorCode VecCreateSeq(MPI_Comm comm, PetscInt n, Vec *v);
// Owned entries followed by local copies of the listed global entries.
PetscErrorCode VecCreateGhost(MPI_Comm comm, PetscInt n, PetscInt N, PetscInt nghost,
                              const PetscInt ghosts[], Vec *v);
PetscErrorCode VecDuplicate(Vec v, Vec *w);
PetscErrorCode VecDestroy(Vec *v);
PetscErrorCode VecGetSize(Vec v, PetscInt *N);
PetscErrorCode VecGetLocalSize(Vec v, PetscInt *n);
PetscErrorCode VecGetOwnershipRange(Vec v, PetscInt *lo, PetscInt *hi);
PetscErrorCode VecGetArray(Vec v, PetscScalar **a);
PetscErrorCode VecRestoreArray(Vec v, PetscScalar **a);
PetscErrorCode VecGetArrayRead(Vec v, const PetscScalar **a);
PetscErrorCode VecRestoreArrayRead(Vec v, const PetscScalar **a);
PetscErrorCode VecSet(Vec v, PetscScalar a);
PetscErrorCode VecSetValues(Vec v, PetscInt n, const PetscInt idx[], const PetscScalar y[], InsertMode mode);
PetscErrorCode VecAssemblyBegin(Vec v);
PetscErrorCode VecAssemblyEnd(Vec v);
PetscErrorCode VecCopy(Vec x, Vec y);
PetscErrorCode VecScale(Vec x, PetscScalar a);
PetscErrorCode VecAXPY(Vec y, PetscScalar a, Vec x);
PetscErrorCode VecAYPX(Vec y, PetscScalar b, Vec x);
PetscErrorCode VecWAXPY(Vec w, PetscScalar a, Vec x, Vec y);
PetscErrorCode VecPointwiseMult(Vec w, Vec x, Vec y);
PetscErrorCode VecDot(Vec x, Vec y, PetscScalar *d);
PetscErrorCode VecNorm(Vec x, NormType type, PetscReal *r);
PetscErrorCode VecSetPreallocationCOO(Vec v, PetscCount ncoo, const PetscInt coo_i[]);
PetscErrorCode VecSetValuesCOO(Vec v, const PetscScalar values[], InsertMode mode);
PetscErrorCode VecGhostUpdateBegin(Vec v, InsertMode mode, ScatterMode smode);
PetscErrorCode VecGhostUpdateEnd(Vec v, InsertMode mode, ScatterMode smode);
PetscErrorCode VecGhostGetLocalForm(Vec v, Vec *l);
PetscErrorCode VecGhostRestoreLocalForm(Vec v, Vec *l);
PetscErrorCode VecScatterCreateToAll(Vec v, VecScatter *ctx, Vec *all);
PetscErrorCode VecScatterCreateToZero(Vec v, VecScatter *ctx, Vec *zero);
PetscErrorCode VecScatterBegin(VecScatter ctx, Vec x, Vec y, InsertMode mode, ScatterMode smode);
PetscErrorCode VecScatterEnd(VecScatter ctx, Vec x, Vec y, InsertMode mode, ScatterMode smode);
PetscErrorCode VecScatterDestroy(VecScatter *ctx);
PetscErrorCode VecView(Vec v, void *viewer);

#ifdef __cplusplus
}
#endif
#endif
