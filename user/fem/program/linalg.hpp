#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include "include/feappv/types.hpp"

namespace fem {

// Dense elimination with partial pivoting for small trusses.
inline Vector solve_increment(Matrix tangent, Vector rhs) {
    const std::size_t n = rhs.size();
    double scale = 0.0;
    for (const auto& row : tangent) {
        for (double value : row) {
            if (!std::isfinite(value)) {
                throw std::runtime_error("Non-finite tangent");
            }
            scale = std::max(scale, std::abs(value));
        }
    }
    const double pivot_tolerance = std::numeric_limits<double>::epsilon() * scale * n;
    for (std::size_t col = 0; col < n; ++col) {
        std::size_t pivot = col;
        for (std::size_t row = col + 1; row < n; ++row) {
            if (std::abs(tangent[row][col]) > std::abs(tangent[pivot][col])) {
                pivot = row;
            }
        }
        if (std::abs(tangent[pivot][col]) <= pivot_tolerance) {
            throw std::runtime_error("Singular truss tangent");
        }
        std::swap(tangent[col], tangent[pivot]);
        std::swap(rhs[col], rhs[pivot]);

        for (std::size_t row = col + 1; row < n; ++row) {
            const double multiplier = tangent[row][col] / tangent[col][col];
            for (std::size_t j = col + 1; j < n; ++j) {
                tangent[row][j] -= multiplier * tangent[col][j];
            }
            rhs[row] -= multiplier * rhs[col];
        }
    }
    for (std::size_t row = n; row-- > 0;) {
        for (std::size_t j = row + 1; j < n; ++j) {
            rhs[row] -= tangent[row][j] * rhs[j];
        }
        rhs[row] /= tangent[row][row];
    }
    return rhs;
}

}  // namespace fem
