#include "program/newton.hpp"
#include "program/linalg.hpp"
#include "elements/frame/truss_law.hpp"

#include <array>
#include <iomanip>

using fem::Vector;
using fem::Matrix;
using fem::Point;
using fem::ElementVector;
using fem::solve_increment;

struct TrussElement {
    std::array<std::size_t, 2> nodes;
    double axial_rigidity;

    auto evaluate(const Point& chord, const ElementVector& u) const {
        return fem::TrussLaw{axial_rigidity}.evaluate(chord, u);
    }
};

struct Truss {
    std::vector<Point> reference;
    std::vector<TrussElement> elements;
    std::vector<int> equation;  // Global DOF to free equation, -1 at a support.
    std::size_t free_count;

    struct System {
        Matrix tangent;
        Vector residual;
        Vector imbalance;  // Internal minus external force at every DOF.
    };

    System assemble(const Vector& u, const Vector& load) const {
        System system{Matrix(free_count, Vector(free_count)), Vector(free_count), load};
        for (double& value : system.imbalance) {
            value = -value;
        }

        for (const auto& element : elements) {
            const auto [left, right] = element.nodes;
            const Point chord{reference[right][0] - reference[left][0],
                              reference[right][1] - reference[left][1]};
            const std::array<std::size_t, 4> dof{
                2 * left, 2 * left + 1, 2 * right, 2 * right + 1};
            const auto response =
                element.evaluate(chord, {u[dof[0]], u[dof[1]], u[dof[2]], u[dof[3]]});
            for (std::size_t i = 0; i < dof.size(); ++i) {
                system.imbalance[dof[i]] += response.force[i];
                const int row = equation[dof[i]];
                if (row < 0) {
                    continue;
                }
                for (std::size_t j = 0; j < dof.size(); ++j) {
                    const int col = equation[dof[j]];
                    if (col >= 0) {
                        system.tangent[row][col] += response.tangent[i][j];
                    }
                }
            }
        }

        for (std::size_t i = 0; i < equation.size(); ++i) {
            if (equation[i] >= 0) {
                system.residual[equation[i]] = -system.imbalance[i];
            }
        }
        return system;
    }
};

double check_element_tangent(const TrussElement& element, const Point& chord) {
    const ElementVector u{0.03, -0.04, -0.11, 0.18};
    constexpr double h = 1e-6;  // m
    const auto exact = element.evaluate(chord, u).tangent;
    double max_error = 0.0;
    for (std::size_t j = 0; j < u.size(); ++j) {
        auto plus = u;
        auto minus = u;
        plus[j] += h;
        minus[j] -= h;
        const auto force_plus = element.evaluate(chord, plus).force;
        const auto force_minus = element.evaluate(chord, minus).force;
        for (std::size_t i = 0; i < u.size(); ++i) {
            const double difference = (force_plus[i] - force_minus[i]) / (2.0 * h);
            max_error = std::max(max_error, std::abs(difference - exact[i][j]));
        }
    }
    if (max_error > 1e-5) {
        throw std::runtime_error("Element tangent failed finite-difference check");
    }
    return max_error;
}

int main() {
    constexpr double half_span = 1.0;   // m
    constexpr double height = 0.2;      // m
    constexpr double axial_rigidity = 1000.0;  // N
    constexpr double expected_uy = 0.6;  // m
    constexpr int load_steps = 8;
    const Truss truss{{{-half_span, 0.0}, {half_span, 0.0}, {0.0, height}},
                      {{{0, 2}, axial_rigidity}, {{1, 2}, axial_rigidity}},
                      {-1, -1, -1, -1, 0, 1}, 2};

    // Independent scalar equilibrium on the symmetric branch.
    const double length = std::hypot(half_span, height);
    const double final_height = height + expected_uy;
    const double final_load = axial_rigidity * final_height *
        (final_height * final_height - height * height) / (length * length * length);

    Vector u(truss.equation.size());
    Vector load(u.size());
    try {
        std::cout << std::scientific << std::setprecision(10);
        const double tangent_error =
            check_element_tangent(truss.elements.front(), {half_span, height});
        std::cout << "Max element tangent FD error [N/m] = " << tangent_error << '\n';

        for (int step = 1; step <= load_steps; ++step) {
            load[5] = final_load * step / load_steps;
            std::cout << "Step " << step << ", vertical load [N] = " << load[5]
                      << "\n  iter  ||R||_inf [N]\n";
            const auto system = solve_load_step(
                u, load[5], [&](const auto& trial) { return truss.assemble(trial, load); },
                [&](const Truss::System& assembled, auto& trial) {
                    const auto increment = solve_increment(assembled.tangent, assembled.residual);
                    for (std::size_t i = 0; i < trial.size(); ++i) {
                        const int eq = truss.equation[i];
                        if (eq >= 0) {
                            trial[i] += increment[eq];
                        }
                    }
                });
            const auto& reaction = system.imbalance;
            std::cout << "  apex displacement [m] = (" << u[4] << ", " << u[5] << ")\n"
                      << "  support reactions [N] = (" << reaction[0] << ", " << reaction[1]
                      << "), (" << reaction[2] << ", " << reaction[3] << ")\n";

            constexpr double force_tolerance = 1e-7;  // N
            const double current_height = height + u[5];
            const double horizontal_reaction = 0.5 * load[5] * half_span / current_height;
            if (std::abs(u[4]) > 1e-9 ||
                std::abs(reaction[0] + horizontal_reaction) > force_tolerance ||
                std::abs(reaction[2] - horizontal_reaction) > force_tolerance ||
                std::abs(reaction[1] + 0.5 * load[5]) > force_tolerance ||
                std::abs(reaction[3] + 0.5 * load[5]) > force_tolerance) {
                throw std::runtime_error("Two-bar symmetry or reaction check failed");
            }
        }

        if (std::abs(u[5] - expected_uy) > 1e-9) {
            throw std::runtime_error("Two-bar displacement check failed");
        }
        std::cout << "Two-bar equilibrium checks passed\n";
    } catch (const std::runtime_error& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
