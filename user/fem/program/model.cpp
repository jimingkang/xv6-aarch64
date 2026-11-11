#include "include/feappv/model.hpp"
#include "newton.hpp"
#include <Eigen/SparseLU>
#include <chrono>
#include <exception>
#include <limits>
#ifdef _OPENMP
#include <omp.h>
#endif

namespace fem {
namespace {
using Clock = std::chrono::steady_clock;
double seconds_since(Clock::time_point begin) {
    return std::chrono::duration<double>(Clock::now()-begin).count();
}

Eigen::VectorXd solve_sparse(const SparseMatrix& matrix, const Vector& rhs) {
    for (Eigen::Index i = 0; i < matrix.nonZeros(); ++i) {
        if (!std::isfinite(matrix.valuePtr()[i])) {
            throw std::runtime_error("Non-finite sparse tangent");
        }
    }
    Eigen::SparseLU<SparseMatrix> solver;
    solver.compute(matrix);
    if (solver.info() != Eigen::Success) {
        throw std::runtime_error("Sparse LU factorization failed: " + solver.lastErrorMessage());
    }
    Eigen::VectorXd delta = solver.solve(Eigen::Map<const Eigen::VectorXd>(
        rhs.data(), static_cast<Eigen::Index>(rhs.size())));
    if (solver.info() != Eigen::Success || !delta.allFinite()) {
        throw std::runtime_error("Sparse LU solve failed");
    }
    return delta;
}
}

void Model::set_element_threads(int threads) {
    if (threads < 1) {
        throw std::invalid_argument("Element thread count must be positive");
    }
#ifndef _OPENMP
    if (threads != 1) {
        throw std::runtime_error("Build with OpenMP to use multiple element threads");
    }
#endif
    element_threads_ = threads;
}
ElementLaw linear(Matrix stiffness) {
    for (const auto& row : stiffness) {
        if (row.size() != stiffness.size()) {
            throw std::invalid_argument("Element stiffness must be square");
        }
    }
    return [stiffness = std::move(stiffness)](const Vector& u) {
        if (u.size() != stiffness.size()) {
            throw std::invalid_argument("Element stiffness and DOF counts differ");
        }
        Vector force(u.size());
        for (std::size_t i = 0; i < u.size(); ++i) {
            for (std::size_t j = 0; j < u.size(); ++j) {
                force[i] += stiffness[i][j] * u[j];
            }
        }
        return Response{std::move(force), stiffness};
    };
}

Model::Model(std::size_t n) : dof_count_(n), fixed_(n), prescribed_(n) {}

void Model::add_element(Dofs dofs, ElementLaw law) {
    if (dofs.empty() || !law) {
        throw std::invalid_argument("Element requires DOFs and a response law");
    }
    for (auto dof : dofs) {
        if (dof >= dof_count_) {
            throw std::out_of_range("Element DOF outside the model");
        }
    }
    elements_.push_back({std::move(dofs), std::move(law)});
}

void Model::prescribe(std::size_t dof, double value) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument("Non-finite prescribed displacement");
    }
    prescribed_.at(dof) = value;
    fixed_[dof] = true;
}

Dofs Model::free_dofs() const {
    Dofs free;
    for (std::size_t i = 0; i < dof_count_; ++i) {
        if (!fixed_[i]) {
            free.push_back(i);
        }
    }
    return free;
}

Model::System Model::assemble(const Vector& u, const Vector& load,
                              const std::vector<SpatialMoment>& moments, SolveTimings* timings) const {
    if (u.size() != dof_count_ || load.size() != dof_count_) {
        throw std::invalid_argument("Displacement or load size differs from model DOF count");
    }
    auto begin = Clock::now();
    std::vector<Response> responses(elements_.size());
    std::vector<std::exception_ptr> failures(elements_.size());
    const auto count = static_cast<std::ptrdiff_t>(elements_.size());
    int actual_threads = 1;
    const auto evaluate = [&](std::ptrdiff_t index) {
        try {
            const auto& element = elements_[index];
            Vector local;
            local.reserve(element.dofs.size());
            for (auto dof : element.dofs) {
                local.push_back(u[dof]);
            }
            responses[index] = element.law(local);
        } catch (const std::exception& xv6_error_) {  // xv6: was catch (...)
            failures[index] = std::current_exception();
        }
    };
#ifdef _OPENMP
    if (element_threads_ > 1) {
#pragma omp parallel num_threads(element_threads_)
        {
#pragma omp single nowait
            actual_threads = omp_get_num_threads();
#pragma omp for schedule(static)
            for (std::ptrdiff_t e = 0; e < count; ++e) {
                evaluate(e);
            }
        }
    } else
#endif
    {
        for (std::ptrdiff_t e = 0; e < count; ++e) {
            evaluate(e);
        }
    }
    if (timings) {
        timings->element_seconds += seconds_since(begin);
        timings->actual_threads = actual_threads;
    }

    begin = Clock::now();
    for (const auto& failure : failures) {
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
    const auto free = free_dofs();
    if (free.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("Sparse equation count exceeds Eigen storage index");
    }
    Dofs equation(dof_count_, free.size());
    for (std::size_t i = 0; i < free.size(); ++i) {
        equation[free[i]] = i;
    }
    const auto n = static_cast<Eigen::Index>(free.size());
    System system{SparseMatrix(n, n), Vector(free.size()), load};
    std::vector<Eigen::Triplet<double>> triplets;
    const auto add = [&](std::size_t row, std::size_t col, double value) {
        if (value != 0.0) {
            triplets.emplace_back(static_cast<int>(row), static_cast<int>(col), value);
        }
    };
    for (double& value : system.imbalance) {
        value = -value;
    }
    // Merge in element order, with a single writer for shared equations.
    for (std::size_t e = 0; e < elements_.size(); ++e) {
        const auto& element = elements_[e];
        const auto& response = responses[e];
        const auto local_size = element.dofs.size();
        if (response.force.size() != local_size || response.tangent.size() != local_size) {
            throw std::runtime_error("Invalid element response size");
        }
        for (std::size_t i = 0; i < local_size; ++i) {
            if (response.tangent[i].size() != local_size) {
                throw std::runtime_error("Invalid element tangent size");
            }
            const auto global = element.dofs[i];
            system.imbalance[global] += response.force[i];
            const auto row = equation[global];
            if (row == free.size()) {
                continue;
            }
            for (std::size_t j = 0; j < local_size; ++j) {
                const auto col = equation[element.dofs[j]];
                if (col != free.size()) {
                    add(row, col, response.tangent[i][j]);
                }
            }
        }
    }
    for (const auto& [dofs, moment] : moments) {
        if (dofs[0] == dofs[1] || dofs[1] == dofs[2] || dofs[0] == dofs[2]) {
            throw std::invalid_argument("Spatial moment requires three distinct rotation DOFs");
        }
        const auto response = spatial_moment_load(
            {u.at(dofs[0]), u.at(dofs[1]), u.at(dofs[2])}, moment);
        for (std::size_t i = 0; i < dofs.size(); ++i) {
            system.imbalance[dofs[i]] -= response.force[i];
            const auto row = equation[dofs[i]];
            if (row == free.size()) {
                continue;
            }
            for (std::size_t j = 0; j < dofs.size(); ++j) {
                const auto col = equation[dofs[j]];
                if (col != free.size()) {
                    add(row, col, -response.tangent[i][j]);
                }
            }
        }
    }
    for (std::size_t i = 0; i < free.size(); ++i) {
        system.residual[i] = -system.imbalance[free[i]];
    }
    system.tangent.setFromTriplets(triplets.begin(), triplets.end());
    if (timings) {
        timings->assembly_seconds += seconds_since(begin);
        ++timings->assemblies;
    }
    return system;
}

Vector Model::solve(Vector& u, const Vector& load, double absolute_tolerance, SolveTimings* timings) const {
    return solve(u, load, {}, absolute_tolerance, timings);
}

Vector Model::solve(Vector& u, const Vector& load, const std::vector<SpatialMoment>& moments,
                    double absolute_tolerance, SolveTimings* timings) const {
    if (u.size() != dof_count_) {
        throw std::invalid_argument("Displacement size differs from model DOF count");
    }
    if (!(absolute_tolerance >= 0.0) || !std::isfinite(absolute_tolerance)) {
        throw std::invalid_argument("Invalid absolute residual tolerance");
    }
    auto trial = u;
    double scale = 0.0;
    for (double value : load) {
        if (!std::isfinite(value)) {
            throw std::invalid_argument("Non-finite load");
        }
        scale = std::max(scale, std::abs(value));
    }
    for (const auto& entry : moments) {
        for (double value : entry.moment) {
            scale = std::max(scale, std::abs(value));
        }
    }
    for (std::size_t i = 0; i < dof_count_; ++i) {
        if (fixed_[i]) {
            trial[i] = prescribed_[i];
        }
    }
    const auto free = free_dofs();
    auto system = solve_load_step(
        trial, scale, [&](const Vector& v) { return assemble(v, load, moments, timings); },
        [&](const System& assembled, Vector& v) {
            const auto begin = Clock::now();
            const auto delta = solve_sparse(assembled.tangent, assembled.residual);
            if (timings) {
                timings->linear_seconds += seconds_since(begin);
                ++timings->linear_solves;
            }
            for (std::size_t i = 0; i < free.size(); ++i) {
                v[free[i]] += delta[i];
            }
        }, absolute_tolerance);
    u = std::move(trial);
    return std::move(system.imbalance);
}
}
