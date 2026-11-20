#include "shell_case.hpp"

#include <petscksp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <ctime>
#include <stdexcept>
#include <sys/resource.h>

fem::PetscMpiCalls shell_mpi_calls();

namespace {
constexpr int steps = 4;

struct State {
    fem::Vector u, reaction;
};

using History = std::array<State, steps>;

History solve(shell_test::ShellCase& shell, fem::PetscSolveTimings* timing, bool progress=false) {
    History result;
    int rank=0;
    PetscCallMPIAbort(PETSC_COMM_WORLD,MPI_Comm_rank(PETSC_COMM_WORLD,&rank));
    const double start=MPI_Wtime();
    const auto cpu_start=std::clock();
    fem::Vector u(shell.load.size()), load(shell.load.size());
    for (int step = 1; step <= steps; ++step) {
        for (std::size_t i = 0; i < load.size(); ++i) {
            load[i] = shell.load[i]*step/steps;
        }
        const auto reaction = timing ? shell.model.solve_petsc(u, load, {}, 1e-8, timing)
                                     : shell.model.solve(u, load, 1e-8);
        result[step-1] = {u, reaction};
        if(progress && rank==0) std::cout<<std::setprecision(12)<<"MPI_STEP "<<step
            <<" elapsed "<<MPI_Wtime()-start<<" tip "<<u[shell.tip]
            <<" cpu "<<double(std::clock()-cpu_start)/CLOCKS_PER_SEC
            <<" linear_solves "<<timing->linear_solves
            <<" iterations "<<timing->linear_iterations<<'\n'<<std::flush;
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
    PetscInt nx = refined ? 32 : benchmark ? 16 : 4, ny = refined ? 16 : benchmark ? 8 : 2;
    PetscCallAbort(PETSC_COMM_WORLD, PetscOptionsGetInt(nullptr,nullptr,"-shell_nx",&nx,nullptr));
    PetscCallAbort(PETSC_COMM_WORLD, PetscOptionsGetInt(nullptr,nullptr,"-shell_ny",&ny,nullptr));
    if (nx < 1 || ny < 1) throw std::runtime_error("Invalid shell grid");
    auto shell = shell_test::cantilever(nx,ny);
    char reference_path[4096] = {};
    PetscBool write_reference = PETSC_FALSE, read_reference = PETSC_FALSE;
    PetscCallAbort(PETSC_COMM_WORLD, PetscOptionsGetString(nullptr,nullptr,"-shell_reference_write",
        reference_path,sizeof(reference_path),&write_reference));
    if (!write_reference) PetscCallAbort(PETSC_COMM_WORLD, PetscOptionsGetString(nullptr,nullptr,
        "-shell_reference_read",reference_path,sizeof(reference_path),&read_reference));
    History reference;
    if (rank == 0) {
        if (read_reference) {
            std::ifstream input(reference_path);
            std::size_t count;
            if (!(input >> count) || count != shell.load.size()) throw std::runtime_error("Invalid reference size");
            for (auto& state : reference) {
                state.u.resize(count); state.reaction.resize(count);
                for (auto& value : state.u) input >> value;
                for (auto& value : state.reaction) input >> value;
            }
            if (!input) throw std::runtime_error("Cannot read shell reference");
        } else reference = solve(shell, nullptr);
        if (write_reference) {
            std::ofstream output(reference_path);
            output << shell.load.size() << '\n' << std::setprecision(17);
            for (const auto& state : reference) {
                for (auto value : state.u) output << value << ' ';
                for (auto value : state.reaction) output << value << ' ';
                output << '\n';
            }
            if (!output) throw std::runtime_error("Cannot write shell reference");
        }
    }
    if (write_reference) return;
    PetscInt repetitions=benchmark ? 3 : 1;
    PetscBool progress=PETSC_FALSE;
    PetscCallAbort(PETSC_COMM_WORLD,PetscOptionsGetInt(nullptr,nullptr,"-shell_repetitions",&repetitions,nullptr));
    PetscCallAbort(PETSC_COMM_WORLD,PetscOptionsGetBool(nullptr,nullptr,"-shell_progress",&progress,nullptr));
    if(repetitions<1) throw std::runtime_error("Invalid repetitions");
    using Sample = std::array<double,20>;
    std::vector<Sample> samples;
    PetscInt ghost_slots = 0;
    for (int rep = 0; rep < repetitions; ++rep) {
        fem::PetscSolveTimings timing;
        timing.read_mpi_calls = shell_mpi_calls;
        timing.check_true_residual = true;
        PetscCallMPIAbort(PETSC_COMM_WORLD, MPI_Barrier(PETSC_COMM_WORLD));
        const double start = MPI_Wtime();
        const auto history = solve(shell, &timing,progress);
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
            double support_error=0, free_residual=0, support=0;
            for (int step = 0; step < steps; ++step) {
                u_error = std::max(u_error, difference(reference[step].u, history[step].u));
                reaction_error = std::max(reaction_error,
                                          difference(reference[step].reaction, history[step].reaction));
                support=0;
                for(PetscInt j=0;j<=ny;++j) support+=history[step].reaction[6*std::size_t(j*(nx+1))+2];
                support_error=std::max(support_error,std::abs(support-.5*(step+1)/steps));
                for(PetscInt j=0;j<=ny;++j) for(PetscInt i=1;i<=nx;++i) for(int k=0;k<6;++k)
                    free_residual=std::max(free_residual,std::abs(history[step].reaction[6*std::size_t(j*(nx+1)+i)+k]));
            }
            if (!(u_error < 1e-8 && reaction_error < 1e-7)) {
                throw std::runtime_error("PETSc displacement or reaction differs from Eigen");
            }
            if(!(support_error<1e-6 && free_residual<1e-7)) throw std::runtime_error("Shell force balance failed");
            Sample maximum{};
            for (const auto& values : per_rank) for (std::size_t i=0; i<maximum.size(); ++i)
                maximum[i] = std::max(maximum[i], values[i]);
            samples.push_back(maximum);
            std::cout << std::setprecision(12) << "MPI_CHECK " << size << ' ' << u_error << ' '
                      << reaction_error << ' ' << history.back().u[shell.tip] << '\n' << std::flush;
            std::cout<<"MPI_BALANCE "<<size<<' '<<support_error<<' '<<free_residual<<' '<<support<<'\n'<<std::flush;
        }
    }
    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage)) throw std::runtime_error("getrusage failed");
#ifdef __APPLE__
    const double peak_mib = double(usage.ru_maxrss)/(1024*1024);
#else
    const double peak_mib = double(usage.ru_maxrss)/1024;
#endif
    std::vector<double> peaks(rank == 0 ? size : 0);
    PetscCallMPIAbort(PETSC_COMM_WORLD, MPI_Gather(&peak_mib,1,MPI_DOUBLE,
        peaks.data(),1,MPI_DOUBLE,0,PETSC_COMM_WORLD));
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
        std::cout << "MPI_MEMORY_MIB " << size;
        for (auto value : peaks) std::cout << ' ' << value;
        std::cout << '\n';
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
