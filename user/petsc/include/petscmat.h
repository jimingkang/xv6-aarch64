#ifndef XV6_PETSCMAT_H
#define XV6_PETSCMAT_H
#include <petscvec.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _p_Mat *Mat;
typedef const char *MatType;
#define MATAIJ    "aij"
#define MATMPIAIJ "mpiaij"
#define MATSEQAIJ "seqaij"
typedef enum { MAT_FLUSH_ASSEMBLY = 1, MAT_FINAL_ASSEMBLY = 0 } MatAssemblyType;

// Square parallel AIJ matrices, assembled with the COO interface.
PetscErrorCode MatCreate(MPI_Comm comm, Mat *A);
PetscErrorCode MatSetSizes(Mat A, PetscInt m, PetscInt n, PetscInt M, PetscInt N);
PetscErrorCode MatSetType(Mat A, MatType type);
PetscErrorCode MatSetFromOptions(Mat A);
PetscErrorCode MatSetUp(Mat A);
PetscErrorCode MatSetPreallocationCOO(Mat A, PetscCount ncoo, PetscInt coo_i[], PetscInt coo_j[]);
PetscErrorCode MatSetValuesCOO(Mat A, const PetscScalar values[], InsertMode mode);
PetscErrorCode MatAssemblyBegin(Mat A, MatAssemblyType type);
PetscErrorCode MatAssemblyEnd(Mat A, MatAssemblyType type);
PetscErrorCode MatGetSize(Mat A, PetscInt *M, PetscInt *N);
PetscErrorCode MatGetLocalSize(Mat A, PetscInt *m, PetscInt *n);
PetscErrorCode MatGetOwnershipRange(Mat A, PetscInt *lo, PetscInt *hi);
PetscErrorCode MatCreateVecs(Mat A, Vec *right, Vec *left);
PetscErrorCode MatMult(Mat A, Vec x, Vec y);
PetscErrorCode MatGetDiagonal(Mat A, Vec d);
PetscErrorCode MatDestroy(Mat *A);

#ifdef __cplusplus
}
#endif
#endif
