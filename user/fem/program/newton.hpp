#pragma once

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

// Model-specific callbacks assemble equilibrium and apply one Newton correction.
template <class Assemble, class Update>
auto solve_load_step(std::vector<double>& displacement, double load_scale,
                     Assemble assemble_trial, Update update_trial,
                     double absolute_tolerance = 1e-12) {
    constexpr int max_updates = 25;
    constexpr double relative_tolerance = 1e-10;
    const double tolerance = absolute_tolerance + relative_tolerance * load_scale;
    auto trial = displacement;

    for (int iteration = 0; iteration <= max_updates; ++iteration) {
        const auto system = assemble_trial(trial);
        double residual_norm = 0.0;
        for (double value : system.residual) {
            if (!std::isfinite(value)) {
                throw std::runtime_error("Non-finite residual");
            }
            residual_norm = std::max(residual_norm, std::abs(value));
        }
        std::cout << "  " << iteration << "  " << residual_norm << '\n';
        if (residual_norm <= tolerance) {
            displacement = std::move(trial);
            return system;
        }
        if (iteration == max_updates) {
            break;
        }

        update_trial(system, trial);
    }
    throw std::runtime_error("Newton iteration did not converge");
}
