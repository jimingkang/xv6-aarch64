#include "feappv/model.hpp"

#include <petscksp.h>
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace fem {
namespace {
template <class Handle, PetscErrorCode (*Destroy)(Handle*)>
struct PetscObjectOwner {
    Handle value = nullptr;
    PetscObjectOwner() = default;
    PetscObjectOwner(const PetscObjectOwner&) = delete;
    PetscObjectOwner& operator=(const PetscObjectOwner&) = delete;
    ~PetscObjectOwner() {
        if (value) {
            PetscCallAbort(PETSC_COMM_WORLD, Destroy(&value));
        }
    }
};

struct Contribution {
    Dofs dofs;
    std::vector<PetscInt> equations;
    std::vector<PetscInt> state_slots;
};
}

Vector Model::solve_petsc(Vector& u, const Vector& load,
                          const std::vector<SpatialMoment>& moments,
                          double absolute_tolerance, PetscSolveTimings* timings) const {
    static_assert(std::is_same<PetscScalar, double>::value, "FEM requires real double PETSc");
    if (u.size() != dof_count_ || load.size() != dof_count_ ||
        !(absolute_tolerance >= 0.0) || !std::isfinite(absolute_tolerance)) {
        throw std::invalid_argument("Invalid PETSc model inputs");
    }
    if (dof_count_ > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("PETSc backend exceeds the supported MPI reaction count");
    }
    double scale = 0;
    for (double value : load) {
        if (!std::isfinite(value)) {
            throw std::invalid_argument("Non-finite load");
        }
        scale = std::max(scale, std::abs(value));
    }
    for (const auto& [dofs, moment] : moments) {
        if (dofs[0] == dofs[1] || dofs[1] == dofs[2] || dofs[0] == dofs[2]) {
            throw std::invalid_argument("Spatial moment requires three distinct rotation DOFs");
        }
        for (std::size_t i = 0; i < dofs.size(); ++i) {
            if (dofs[i] >= dof_count_ || !std::isfinite(moment[i])) {
                throw std::invalid_argument("Invalid spatial moment");
            }
            scale = std::max(scale, std::abs(moment[i]));
        }
    }

    PetscMPIInt rank, size;
    PetscCallMPIAbort(PETSC_COMM_WORLD, MPI_Comm_rank(PETSC_COMM_WORLD, &rank));
    PetscCallMPIAbort(PETSC_COMM_WORLD, MPI_Comm_size(PETSC_COMM_WORLD, &size));
    const auto begin = elements_.size()*rank/size;
    const auto end = elements_.size()*(rank+1)/size;
    const auto element_count = end-begin;
    const auto free = free_dofs();
    const auto n = static_cast<PetscInt>(free.size());
    std::vector<PetscInt> equation(dof_count_, -1);
    for (PetscInt i = 0; i < n; ++i) {
        equation[free[i]] = i;
    }
    double start = MPI_Wtime();
    std::vector<Contribution> contributions;
    const auto add_pattern = [&](const Dofs& dofs) {
        Contribution c{dofs, {}, {}};
        for (auto dof : dofs) {
            c.equations.push_back(equation[dof]);
        }
        contributions.push_back(std::move(c));
    };
    for (auto e = begin; e < end; ++e) {
        add_pattern(elements_[e].dofs);
    }
    if (rank == 0) {
        for (const auto& moment : moments) {
            add_pattern(Dofs(moment.dofs.begin(), moment.dofs.end()));
        }
    }

    std::vector<PetscInt> rows, columns, rhs_rows;
    for (const auto& c : contributions) {
        for (auto row : c.equations) {
            if (row < 0) {
                continue;
            }
            rhs_rows.push_back(row);
            for (auto col : c.equations) {
                if (col >= 0) {
                    rows.push_back(row);
                    columns.push_back(col);
                }
            }
        }
    }
    if (timings) {
        timings->assembly_seconds += MPI_Wtime()-start;
    }

    PetscObjectOwner<Mat, MatDestroy> matrix;
    PetscObjectOwner<Vec, VecDestroy> rhs, delta, state, true_residual;
    PetscObjectOwner<KSP, KSPDestroy> ksp;
    PetscInt owned_begin = 0, owned_end = 0;
    start = MPI_Wtime();
    if (n != 0) {
        PetscCallAbort(PETSC_COMM_WORLD, MatCreate(PETSC_COMM_WORLD, &matrix.value));
        PetscCallAbort(PETSC_COMM_WORLD, MatSetSizes(matrix.value, PETSC_DECIDE, PETSC_DECIDE, n, n));
        PetscCallAbort(PETSC_COMM_WORLD, MatSetType(matrix.value, MATAIJ));
        PetscCallAbort(PETSC_COMM_WORLD, MatSetPreallocationCOO(
            matrix.value, static_cast<PetscCount>(rows.size()), rows.data(), columns.data()));
        PetscCallAbort(PETSC_COMM_WORLD, MatCreateVecs(matrix.value, &delta.value, &rhs.value));
        PetscCallAbort(PETSC_COMM_WORLD, VecGetOwnershipRange(rhs.value, &owned_begin, &owned_end));
        if (timings) {
            timings->communication_seconds += MPI_Wtime()-start;
        }

        start = MPI_Wtime();
        std::vector<PetscInt> ghosts;
        for (auto row : rhs_rows) {
            if (row < owned_begin || row >= owned_end) {
                ghosts.push_back(row);
            }
        }
        std::sort(ghosts.begin(), ghosts.end());
        ghosts.erase(std::unique(ghosts.begin(), ghosts.end()), ghosts.end());
        const auto owned = owned_end-owned_begin;
        for (auto& c : contributions) {
            c.state_slots.reserve(c.equations.size());
            for (auto row : c.equations) {
                if (row < 0) {
                    c.state_slots.push_back(-1);
                } else if (row >= owned_begin && row < owned_end) {
                    c.state_slots.push_back(row-owned_begin);
                } else {
                    c.state_slots.push_back(owned +
                        (std::lower_bound(ghosts.begin(), ghosts.end(), row)-ghosts.begin()));
                }
            }
        }
        for (auto row = owned_begin; row < owned_end; ++row) {
            rhs_rows.push_back(row);
        }
        if (timings) {
            timings->assembly_seconds += MPI_Wtime()-start;
            timings->ghost_dofs = ghosts.size();
        }

        start = MPI_Wtime();
        PetscCallAbort(PETSC_COMM_WORLD, VecCreateGhost(PETSC_COMM_WORLD, owned, n,
            static_cast<PetscInt>(ghosts.size()), ghosts.data(), &state.value));
        PetscCallAbort(PETSC_COMM_WORLD, VecSetPreallocationCOO(
            rhs.value, static_cast<PetscCount>(rhs_rows.size()), rhs_rows.data()));
        PetscScalar* owned_values;
        PetscCallAbort(PETSC_COMM_WORLD, VecGetArray(state.value, &owned_values));
        for (auto row = owned_begin; row < owned_end; ++row) {
            owned_values[row-owned_begin] = u[free[row]];
        }
        PetscCallAbort(PETSC_COMM_WORLD, VecRestoreArray(state.value, &owned_values));
        PetscCallAbort(PETSC_COMM_WORLD, KSPCreate(PETSC_COMM_WORLD, &ksp.value));
        PetscCallAbort(PETSC_COMM_WORLD, KSPSetType(ksp.value, KSPGMRES));
        PetscCallAbort(PETSC_COMM_WORLD, KSPGMRESSetRestart(ksp.value, 200));
        PC pc;
        PetscCallAbort(PETSC_COMM_WORLD, KSPGetPC(ksp.value, &pc));
        PetscCallAbort(PETSC_COMM_WORLD, PCSetType(pc, PCASM));
        PetscCallAbort(PETSC_COMM_WORLD, PCASMSetOverlap(pc, 2));
        PetscCallAbort(PETSC_COMM_WORLD, KSPSetTolerances(ksp.value, 1e-12, 1e-14, PETSC_DEFAULT, 4000));
        PetscCallAbort(PETSC_COMM_WORLD, KSPSetFromOptions(ksp.value));
    }
    if (timings) {
        timings->communication_seconds += MPI_Wtime()-start;
    }

    Vector matrix_values(rows.size()), rhs_values(rhs_rows.size()), imbalance(dof_count_);
    std::vector<Response> responses(contributions.size());
    constexpr int max_updates = 25;
    const double tolerance = absolute_tolerance + 1e-10*scale;
    for (int iteration = 0; iteration <= max_updates; ++iteration) {
        Vec local_state = nullptr;
        const PetscScalar* local_values = nullptr;
        if (state.value) {
            start = MPI_Wtime();
            PetscCallAbort(PETSC_COMM_WORLD, VecGhostUpdateBegin(state.value, INSERT_VALUES, SCATTER_FORWARD));
            PetscCallAbort(PETSC_COMM_WORLD, VecGhostUpdateEnd(state.value, INSERT_VALUES, SCATTER_FORWARD));
            if (timings) {
                const double elapsed = MPI_Wtime()-start;
                timings->communication_seconds += elapsed;
                timings->halo_seconds += elapsed;
            }
            PetscCallAbort(PETSC_COMM_WORLD, VecGhostGetLocalForm(state.value, &local_state));
            PetscCallAbort(PETSC_COMM_WORLD, VecGetArrayRead(local_state, &local_values));
        }
        std::exception_ptr failure;
        start = MPI_Wtime();
        try {
            for (std::size_t e = 0; e < contributions.size(); ++e) {
                const auto& c = contributions[e];
                const auto& dofs = c.dofs;
                const auto value_at = [&](std::size_t i) {
                    return c.equations[i] < 0 ? prescribed_[dofs[i]] : local_values[c.state_slots[i]];
                };
                if (e < element_count) {
                    Vector local;
                    local.reserve(dofs.size());
                    for (std::size_t i = 0; i < dofs.size(); ++i) {
                        local.push_back(value_at(i));
                    }
                    responses[e] = elements_[begin+e].law(local);
                } else {
                    responses[e] = spatial_moment_load(
                        {value_at(0), value_at(1), value_at(2)},
                        moments[e-element_count].moment);
                }
                const auto& response = responses[e];
                if (response.force.size() != dofs.size() || response.tangent.size() != dofs.size() ||
                    !std::all_of(response.force.begin(), response.force.end(),
                                 [](double x) { return std::isfinite(x); })) {
                    throw std::runtime_error("Invalid PETSc contribution force or tangent");
                }
                for (const auto& row : response.tangent) {
                    if (row.size() != dofs.size() ||
                        !std::all_of(row.begin(), row.end(), [](double x) { return std::isfinite(x); })) {
                        throw std::runtime_error("Invalid PETSc contribution tangent row");
                    }
                }
            }
        } catch (const std::exception& xv6_error_) {  // xv6: was catch (...)
            failure = std::current_exception();
        }
        if (timings) {
            timings->element_seconds += MPI_Wtime()-start;
        }
        if (local_state) {
            PetscCallAbort(PETSC_COMM_WORLD, VecRestoreArrayRead(local_state, &local_values));
            PetscCallAbort(PETSC_COMM_WORLD, VecGhostRestoreLocalForm(state.value, &local_state));
        }
        start = MPI_Wtime();
        const int local_failure = failure ? 1 : 0;
        int global_failure;
        PetscCallMPIAbort(PETSC_COMM_WORLD, MPI_Allreduce(
            &local_failure, &global_failure, 1, MPI_INT, MPI_MAX, PETSC_COMM_WORLD));
        if (global_failure) {
            if (failure) {
                std::rethrow_exception(failure);
            }
            throw std::runtime_error("PETSc element evaluation failed on another rank");
        }
        if (timings) {
            timings->communication_seconds += MPI_Wtime()-start;
        }

        start = MPI_Wtime();
        std::fill(imbalance.begin(), imbalance.end(), 0.0);
        std::size_t k = 0, f = 0;
        for (std::size_t e = 0; e < contributions.size(); ++e) {
            const auto& c = contributions[e];
            const auto& response = responses[e];
            const double sign = e < element_count ? 1.0 : -1.0;
            for (std::size_t i = 0; i < c.dofs.size(); ++i) {
                const double force = sign*response.force[i];
                imbalance[c.dofs[i]] += force;
                if (c.equations[i] < 0) {
                    continue;
                }
                rhs_values[f++] = -force;
                for (std::size_t j = 0; j < c.dofs.size(); ++j) {
                    if (c.equations[j] >= 0) {
                        matrix_values[k++] = sign*response.tangent[i][j];
                    }
                }
            }
        }
        for (auto row = owned_begin; row < owned_end; ++row) {
            rhs_values[f++] = load[free[row]];
        }
        if (timings) {
            timings->assembly_seconds += MPI_Wtime()-start;
            ++timings->assemblies;
        }

        start = MPI_Wtime();
        PetscReal norm = 0;
        if (n != 0) {
            // COO INSERT sums duplicate local and remote entries, replacing the previous iterate.
            PetscCallAbort(PETSC_COMM_WORLD, MatSetValuesCOO(matrix.value, matrix_values.data(), INSERT_VALUES));
            PetscCallAbort(PETSC_COMM_WORLD, VecSetValuesCOO(rhs.value, rhs_values.data(), INSERT_VALUES));
            PetscCallAbort(PETSC_COMM_WORLD, VecNorm(rhs.value, NORM_INFINITY, &norm));
        }
        if (!std::isfinite(norm)) {
            throw std::runtime_error("Non-finite PETSc residual");
        }
        if (norm <= tolerance) {
            PetscCallMPIAbort(PETSC_COMM_WORLD, MPI_Allreduce(
                MPI_IN_PLACE, imbalance.data(), static_cast<int>(dof_count_), MPI_DOUBLE, MPI_SUM,
                PETSC_COMM_WORLD));
            PetscObjectOwner<Vec, VecDestroy> gathered;
            PetscObjectOwner<VecScatter, VecScatterDestroy> gather;
            if (state.value) {
                // The public API returns a full converged state once per load step.
                PetscCallAbort(PETSC_COMM_WORLD, VecScatterCreateToAll(state.value, &gather.value, &gathered.value));
                PetscCallAbort(PETSC_COMM_WORLD, VecScatterBegin(
                    gather.value, state.value, gathered.value, INSERT_VALUES, SCATTER_FORWARD));
                PetscCallAbort(PETSC_COMM_WORLD, VecScatterEnd(
                    gather.value, state.value, gathered.value, INSERT_VALUES, SCATTER_FORWARD));
                const PetscScalar* converged;
                PetscCallAbort(PETSC_COMM_WORLD, VecGetArrayRead(gathered.value, &converged));
                for (PetscInt i = 0; i < n; ++i) {
                    u[free[i]] = converged[i];
                }
                PetscCallAbort(PETSC_COMM_WORLD, VecRestoreArrayRead(gathered.value, &converged));
            }
            if (timings) {
                timings->communication_seconds += MPI_Wtime()-start;
            }
            for (std::size_t i = 0; i < imbalance.size(); ++i) {
                imbalance[i] -= load[i];
                if (fixed_[i]) {
                    u[i] = prescribed_[i];
                }
            }
            return imbalance;
        }
        if (timings) {
            timings->communication_seconds += MPI_Wtime()-start;
        }
        if (iteration == max_updates) {
            break;
        }

        const auto read_mpi = timings ? timings->read_mpi_calls : nullptr;
        const auto mpi_start = read_mpi ? read_mpi() : PetscMpiCalls{};
        start = MPI_Wtime();
        PetscCallAbort(PETSC_COMM_WORLD, KSPSetOperators(ksp.value, matrix.value, matrix.value));
        PetscCallAbort(PETSC_COMM_WORLD, KSPSetUp(ksp.value));
        PetscCallAbort(PETSC_COMM_WORLD, KSPSetUpOnBlocks(ksp.value));
        const double solve_start = MPI_Wtime();
        const auto mpi_solve_start = read_mpi ? read_mpi() : PetscMpiCalls{};
        PetscCallAbort(PETSC_COMM_WORLD, KSPSolve(ksp.value, rhs.value, delta.value));
        const double solve_end = MPI_Wtime();
        if (timings) {
            timings->setup_seconds += solve_start-start;
            timings->iteration_seconds += solve_end-solve_start;
            const auto end_mpi = read_mpi ? read_mpi() : PetscMpiCalls{};
            timings->setup_mpi_seconds += mpi_solve_start.seconds-mpi_start.seconds;
            auto& mpi = timings->iteration_mpi;
            mpi.seconds += end_mpi.seconds-mpi_solve_start.seconds;
            timings->iteration_mpi_seconds = mpi.seconds;
            mpi.reduction_seconds += end_mpi.reduction_seconds-mpi_solve_start.reduction_seconds;
            mpi.point_seconds += end_mpi.point_seconds-mpi_solve_start.point_seconds;
            mpi.reductions += end_mpi.reductions-mpi_solve_start.reductions;
            mpi.async_reductions += end_mpi.async_reductions-mpi_solve_start.async_reductions;
            mpi.point_operations += end_mpi.point_operations-mpi_solve_start.point_operations;
        }
        KSPConvergedReason reason;
        PetscInt iterations;
        PetscCallAbort(PETSC_COMM_WORLD, KSPGetConvergedReason(ksp.value, &reason));
        PetscCallAbort(PETSC_COMM_WORLD, KSPGetIterationNumber(ksp.value, &iterations));
        if (reason <= 0) {
            throw std::runtime_error("PETSc KSP failed to converge, reason " + std::to_string(reason));
        }
        if (timings) {
            timings->linear_seconds += MPI_Wtime()-start;
            ++timings->linear_solves;
            timings->linear_iterations += iterations;
        }

        if (timings && timings->check_true_residual) {
            if (!true_residual.value) {
                PetscCallAbort(PETSC_COMM_WORLD, VecDuplicate(rhs.value, &true_residual.value));
            }
            PetscReal residual_norm, rhs_norm;
            PetscCallAbort(PETSC_COMM_WORLD, MatMult(matrix.value, delta.value, true_residual.value));
            PetscCallAbort(PETSC_COMM_WORLD, VecAYPX(true_residual.value, -1.0, rhs.value));
            PetscCallAbort(PETSC_COMM_WORLD, VecNorm(true_residual.value, NORM_2, &residual_norm));
            PetscCallAbort(PETSC_COMM_WORLD, VecNorm(rhs.value, NORM_2, &rhs_norm));
            const double relative = rhs_norm > 0 ? residual_norm/rhs_norm : residual_norm;
            if (!std::isfinite(relative)) {
                throw std::runtime_error("Non-finite true PETSc residual");
            }
            timings->max_true_relative_residual = std::max(timings->max_true_relative_residual, relative);
            timings->max_true_absolute_residual = std::max(timings->max_true_absolute_residual, residual_norm);
        }

        PetscCallAbort(PETSC_COMM_WORLD, VecAXPY(state.value, 1.0, delta.value));
    }
    throw std::runtime_error("PETSc Newton iteration did not converge");
}
}
