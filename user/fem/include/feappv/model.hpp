#pragma once
#include "elements.hpp"
#include <Eigen/SparseCore>

#include <functional>

namespace fem {
using Dofs = std::vector<std::size_t>;
using ElementLaw = std::function<Response(const Vector&)>;
using SparseMatrix = Eigen::SparseMatrix<double>;

struct SolveTimings {
    double element_seconds = 0;
    double assembly_seconds = 0;
    double linear_seconds = 0;
    int actual_threads = 1;
    int assemblies = 0;
    int linear_solves = 0;
};

#ifdef FEAPPV_HAS_PETSC
}
struct _p_KSP;
namespace fem {
struct PetscMpiCalls {
    double seconds = 0;
    double reduction_seconds = 0;
    double point_seconds = 0;
    std::size_t reductions = 0;
    std::size_t async_reductions = 0;
    std::size_t point_operations = 0;
};

// Local elapsed times. communication_seconds excludes KSP, whose setup and
// iteration phases include their respective MPI call times.
struct PetscSolveTimings {
    double element_seconds = 0;
    double assembly_seconds = 0;
    double communication_seconds = 0;
    double linear_seconds = 0;
    double halo_seconds = 0;
    double setup_seconds = 0;
    double iteration_seconds = 0;
    double setup_mpi_seconds = 0;
    double iteration_mpi_seconds = 0;
    PetscMpiCalls (*read_mpi_calls)() = nullptr;
    // Called once after KSPSetFromOptions, e.g. to attach a diagnostic monitor.
    void (*configure_ksp)(_p_KSP* ksp, void* context) = nullptr;
    void* configure_context = nullptr;
    PetscMpiCalls iteration_mpi;
    bool check_true_residual = false;
    double max_true_relative_residual = 0;
    double max_true_absolute_residual = 0;
    std::size_t ghost_dofs = 0;  // This rank's receive slots in the current step.
    int assemblies = 0;
    int linear_solves = 0;
    std::size_t linear_iterations = 0;
};
#endif

// Adapter for small-displacement linear elements.
ElementLaw linear(Matrix stiffness);

struct SpatialMoment {
    std::array<std::size_t, 3> dofs;  // Global rotation-vector DOFs, in x/y/z order.
    Point3 moment;                   // Fixed global moment at this load step.
};

// Generalized external force and its derivative with respect to the rotation vector.
Response spatial_moment_load(const Point3& rotation, const Point3& moment);

class Model {
public:
    explicit Model(std::size_t dof_count);
    void add_element(Dofs dofs, ElementLaw law);
    void prescribe(std::size_t dof, double displacement = 0.0);
    // Parallel evaluation requires reentrant, read-only element callbacks.
    void set_element_threads(int threads);

    struct System {
        SparseMatrix tangent;
        Vector residual;
        Vector imbalance;
    };
    System assemble(const Vector& displacement, const Vector& load,
                    const std::vector<SpatialMoment>& moments = {},
                    SolveTimings* timings = nullptr) const;

    // Prescribed displacements are fixed targets, and load is the total step load.
    // Returns internal minus external force; constrained entries are reactions.
    Vector solve(Vector& displacement, const Vector& load,
                 double absolute_tolerance = 1e-12, SolveTimings* timings = nullptr) const;

#ifdef FEAPPV_HAS_PETSC
    // Collective on PETSC_COMM_WORLD after PetscInitialize. Model and inputs
    // must agree on every rank. Inputs and converged outputs are replicated,
    // while the trial state is distributed and ghosted.
    Vector solve_petsc(Vector& displacement, const Vector& load,
                       const std::vector<SpatialMoment>& moments = {},
                       double absolute_tolerance = 1e-12,
                       PetscSolveTimings* timings = nullptr) const;
#endif
    Vector solve(Vector& displacement, const Vector& load,
                 const std::vector<SpatialMoment>& moments,
                 double absolute_tolerance = 1e-12, SolveTimings* timings = nullptr) const;

private:
    struct Element { Dofs dofs; ElementLaw law; };
    std::size_t dof_count_;
    std::vector<Element> elements_;
    std::vector<bool> fixed_;
    Vector prescribed_;
    int element_threads_ = 1;
    Dofs free_dofs() const;
};
}
