// Two-element bar on two MPI ranks (xv6 build): nodes 0-1-2, EA/L = 2 and 3,
// node 0 fixed, unit force at node 2.  -fem_trace prints every rank's COO
// arrays, ghost state, residual slots and Newton updates (program/petsc_model.cpp);
// -ksp_trace makes user/petsc print each GMRES step per rank: basis vectors,
// the off-process entries received by each matrix-vector product, local dot
// products and their reductions, and the true residual.
//
//   mpirun -np 2 bar_mpi_trace
// runs with "-fem_trace -ksp_trace -pc_type none -ksp_converged_reason" when
// no options are given.
#include <feappv/model.hpp>

#include <petscksp.h>
#include <cmath>
#include <cstring>
#include <iostream>

int main(int argc, char** argv) {
    // mpirun inserts "-np N" after argv[0]; MPI_Init (inside PetscInitialize) strips it.
    const int launcher_args = argc >= 3 && std::strcmp(argv[1], "-np") == 0 ? 2 : 0;
    static const char* defaults[] = {"-fem_trace", "-ksp_trace", "-pc_type", "none", "-ksp_converged_reason"};
    static char* with_defaults[16];
    if (argc == 1 + launcher_args) {
        int k = 0;
        for (int i = 0; i < argc; ++i) {
            with_defaults[k++] = argv[i];
        }
        for (const char* option : defaults) {
            with_defaults[k++] = const_cast<char*>(option);
        }
        with_defaults[k] = nullptr;
        argc = k;
        argv = with_defaults;
    }
    PetscCall(PetscInitialize(&argc, &argv, nullptr, nullptr));
    int status = 0;
    {
        fem::Model model(3);
        model.add_element({0, 1}, fem::linear(fem::bar1(1.0, 2.0)));
        model.add_element({1, 2}, fem::linear(fem::bar1(1.0, 3.0)));
        model.prescribe(0);
        fem::Vector u(3), load{0.0, 0.0, 1.0};
        const auto reaction = model.solve_petsc(u, load, {}, 1e-12);
        PetscMPIInt rank;
        PetscCallMPI(MPI_Comm_rank(PETSC_COMM_WORLD, &rank));
        if (rank == 0) {
            std::cout.precision(12);
            std::cout << "RESULT u=[" << u[0] << ',' << u[1] << ',' << u[2] << "] reaction=["
                      << reaction[0] << ',' << reaction[1] << ',' << reaction[2] << "]\n";
            if (std::abs(u[1]-0.5) > 1e-12 || std::abs(u[2]-5.0/6.0) > 1e-12 ||
                std::abs(reaction[0]+1.0) > 1e-12) {
                std::cout << "bar_mpi_trace: hand solution mismatch\n";
                status = 1;
            }
        }
    }
    PetscCall(PetscFinalize());
    return status;
}
