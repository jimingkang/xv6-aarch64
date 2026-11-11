#include "shell_case.hpp"

#include <petscksp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>

fem::PetscMpiCalls shell_mpi_calls();

namespace {
constexpr int steps = 4;

struct State {
    fem::Vector u, reaction;
};

using History = std::array<State, steps>;

History solve(shell_test::ShellCase& shell, fem::PetscSolveTimings* timing) {
    History result;
    fem::Vector u(shell.load.size()), load(shell.load.size());
    for (int step = 1; step <= steps; ++step) {
        for (std::size_t i = 0; i < load.size(); ++i) {
            load[i] = shell.load[i]*step/steps;
        }
        const auto reaction = timing ? shell.model.solve_petsc(u, load, {}, 1e-8, timing)
                                     : shell.model.solve(u, load, 1e-8);
        result[step-1] = {u, reaction};
    }
    return result;
}

double difference(const fem::Vector& a, const fem::Vector& b) {
    double result = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        result = std::max(result, std::abs(a[i]-b[i]));
    }
    return result;
}

void run() {
    PetscMPIInt rank, size;
    PetscCallMPIAbort(PETSC_COMM_WORLD, MPI_Comm_rank(PETSC_COMM_WORLD, &rank));
    PetscCallMPIAbort(PETSC_COMM_WORLD, MPI_Comm_size(PETSC_COMM_WORLD, &size));
    PetscBool benchmark = PETSC_FALSE;
    PetscCallAbort(PETSC_COMM_WORLD, PetscOptionsGetBool(
        nullptr, nullptr, "-shell_benchmark", &benchmark, nullptr));
    PetscBool refined = PETSC_FALSE;
    PetscCallAbort(PETSC_COMM_WORLD, PetscOptionsGetBool(
        nullptr, nullptr, "-shell_refined", &refined, nullptr));
    auto shell = refined ? shell_test::cantilever(32,16)
                         : benchmark ? shell_test::cantilever(16,8) : shell_test::cantilever(4,2);
    History reference;
    if (rank == 0) {
        reference = solve(shell, nullptr);
    }
    using Sample = std::array<double,20>;
    std::vector<Sample> samples;
    PetscInt ghost_slots = 0;
    for (int rep = 0; rep < (benchmark ? 3 : 1); ++rep) {
        fem::PetscSolveTimings timing;
        timing.read_mpi_calls = shell_mpi_calls;
        timing.check_true_residual = true;
        PetscCallMPIAbort(PETSC_COMM_WORLD, MPI_Barrier(PETSC_COMM_WORLD));
        const double start = MPI_Wtime();
        const auto history = solve(shell, &timing);
        const double wall = MPI_Wtime()-start;
        const Sample sample{wall, timing.element_seconds, timing.assembly_seconds,
                            timing.communication_seconds, timing.linear_seconds,
                            double(timing.assemblies), double(timing.linear_solves),
                            double(timing.linear_iterations), timing.halo_seconds,
                            timing.setup_seconds, timing.iteration_seconds,
                            timing.setup_mpi_seconds, timing.iteration_mpi_seconds,
                            double(timing.iteration_mpi.reductions),
                            double(timing.iteration_mpi.async_reductions),
                            timing.iteration_mpi.reduction_seconds,
                            double(timing.iteration_mpi.point_operations),
                            timing.iteration_mpi.point_seconds,
                            timing.max_true_relative_residual, timing.max_true_absolute_residual};
        const auto local_ghosts = static_cast<PetscInt>(timing.ghost_dofs);
        PetscCallMPIAbort(PETSC_COMM_WORLD, MPI_Reduce(
            &local_ghosts, &ghost_slots, 1, MPIU_INT, MPI_SUM, 0, PETSC_COMM_WORLD));
        std::vector<Sample> per_rank(rank == 0 ? size : 0);
        PetscCallMPIAbort(PETSC_COMM_WORLD, MPI_Gather(
            sample.data(), sample.size(), MPI_DOUBLE, per_rank.data(), sample.size(),
            MPI_DOUBLE, 0, PETSC_COMM_WORLD));

        if (rank == 0) {
            double u_error = 0, reaction_error = 0;
            for (int step = 0; step < steps; ++step) {
                u_error = std::max(u_error, difference(reference[step].u, history[step].u));
                reaction_error = std::max(reaction_error,
                                          difference(reference[step].reaction, history[step].reaction));
            }
            if (!(u_error < 1e-8 && reaction_error < 1e-7)) {
                throw std::runtime_error("PETSc displacement or reaction differs from Eigen");
            }
            samples.push_back(*std::max_element(per_rank.begin(), per_rank.end()));
            std::cout << std::setprecision(12) << "MPI_CHECK " << size << ' ' << u_error << ' '
                      << reaction_error << ' ' << history.back().u[shell.tip] << '\n';
        }
    }
    if (rank == 0) {
        if ((size == 1 && ghost_slots != 0) || (size > 1 && ghost_slots == 0)) {
            throw std::runtime_error("Shell ghost dependencies do not match MPI partitioning");
        }
        std::sort(samples.begin(), samples.end());
        std::cout << "MPI_TIMING " << size;
        for (auto value : samples[samples.size()/2]) {
            std::cout << ' ' << value;
        }
        std::cout << '\n';
        std::cout << "MPI_GHOST " << size << ' ' << ghost_slots << '\n';
    }
}
}

int main(int argc, char** argv) {
    const auto ierr = PetscInitialize(&argc, &argv, nullptr, nullptr);
    if (ierr != PETSC_SUCCESS) {
        return ierr;
    }
    try {
        run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        MPI_Abort(PETSC_COMM_WORLD, 1);
        return 1;
    }
    return PetscFinalize();
}
