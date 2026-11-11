#pragma once
#include "include/feappv/elements.hpp"

namespace fem::detail {
inline Matrix square(std::size_t n) { return Matrix(n, Vector(n)); }

inline Matrix elasticity(double e, double nu, Plane plane) {
    if (plane == Plane::stress) {
        const double c = e / (1.0 - nu * nu);
        return {{c, nu*c, 0}, {nu*c, c, 0}, {0, 0, 0.5*(1.0-nu)*c}};
    }
    const double c = e / ((1.0 + nu) * (1.0 - 2.0*nu));
    return {{(1.0-nu)*c, nu*c, 0}, {nu*c, (1.0-nu)*c, 0}, {0, 0, 0.5*(1.0-2.0*nu)*c}};
}

inline void integrate(Matrix& k, const Matrix& b, const Matrix& d, double weight) {
    Matrix db(b.size(), Vector(k.size()));
    for (std::size_t i = 0; i < b.size(); ++i) {
        for (std::size_t j = 0; j < b.size(); ++j) {
            for (std::size_t col = 0; col < k.size(); ++col) {
                db[i][col] += d[i][j] * b[j][col];
            }
        }
    }
    for (std::size_t i = 0; i < k.size(); ++i) {
        for (std::size_t j = 0; j < k.size(); ++j) {
            for (std::size_t m = 0; m < b.size(); ++m) {
                k[i][j] += weight * b[m][i] * db[m][j];
            }
        }
    }
}
}
