#include "elements/solid1d/cubic_bar.hpp"
#include "program/newton.hpp"

#include <cmath>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

using fem::BarElement;

// Free equations are numbered by node minus one. Node zero is fixed.
struct System {
    std::vector<double> diagonal;
    std::vector<double> off_diagonal;
    std::vector<double> residual;
    double reaction = 0.0;
};

System assemble(const std::vector<BarElement>& elements,
                const std::vector<double>& displacement, double end_load) {
    const auto count = elements.size();
    System system{std::vector<double>(count), std::vector<double>(count - 1),
                  std::vector<double>(count)};
    system.residual.back() = end_load;

    for (std::size_t e = 0; e < count; ++e) {
        const auto [force, stiffness] =
            elements[e].evaluate(displacement[e], displacement[e + 1]);
        system.residual[e] -= force;
        system.diagonal[e] += stiffness;
        if (e == 0) {
            system.reaction = -force;
        } else {
            system.residual[e - 1] += force;
            system.diagonal[e - 1] += stiffness;
            system.off_diagonal[e - 1] -= stiffness;
        }
    }
    return system;
}

// Tridiagonal elimination for the positive-definite reduced tangent.
std::vector<double> solve_increment(System system) {
    auto& diagonal = system.diagonal;
    auto& increment = system.residual;
    const auto& off = system.off_diagonal;

    for (std::size_t i = 0; i < diagonal.size(); ++i) {
        if (!std::isfinite(diagonal[i]) || diagonal[i] <= 0.0) {
            throw std::runtime_error("Invalid tangent pivot");
        }
        if (i < off.size()) {
            const double multiplier = off[i] / diagonal[i];
            diagonal[i + 1] -= multiplier * off[i];
            increment[i + 1] -= multiplier * increment[i];
        }
    }
    increment.back() /= diagonal.back();
    for (std::size_t i = off.size(); i > 0; --i) {
        increment[i - 1] = (increment[i - 1] - off[i - 1] * increment[i]) /
                           diagonal[i - 1];
    }
    return increment;
}

int main() {
    constexpr int element_count = 4;
    constexpr int load_steps = 4;
    constexpr double length = 2.0;          // m
    constexpr double final_load = 4.0;      // N
    const std::vector<BarElement> elements(
        element_count, {length / element_count, 0.01, 1000.0, 25000.0});
    std::vector<double> displacement(element_count + 1);
    double reaction = 0.0;

    try {
        std::cout << std::scientific << std::setprecision(10);
        for (int step = 1; step <= load_steps; ++step) {
            const double load = final_load * step / load_steps;
            std::cout << "Step " << step << ", load [N] = " << load
                      << "\n  iter  ||R||_inf [N]\n";
            reaction = solve_load_step(
                displacement, std::abs(load),
                [&](const auto& trial) { return assemble(elements, trial, load); },
                [](const System& system, auto& trial) {
                    const auto increment = solve_increment(system);
                    for (std::size_t i = 0; i < increment.size(); ++i) {
                        trial[i + 1] += increment[i];
                    }
                }).reaction;
            std::cout << "  tip displacement [m] = " << displacement.back()
                      << ", support reaction [N] = " << reaction << '\n';
        }
        // Strain 0.2 gives a 4 N axial force for the chosen cubic law.
        if (std::abs(displacement.back() - 0.4) > 1e-9 ||
            std::abs(reaction + final_load) > 1e-9) {
            throw std::runtime_error("Cubic bar displacement or reaction check failed");
        }
    } catch (const std::runtime_error& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
