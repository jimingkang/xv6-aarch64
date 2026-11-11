#pragma once

#include <array>
#include <cmath>
#include <cstddef>

namespace fem::detail {

// Value, gradient and Hessian with respect to element DOFs.
template <std::size_t N>
struct Jet {
    double value = 0.0;
    std::array<double, N> gradient{};
    std::array<std::array<double, N>, N> hessian{};

    Jet() = default;
    Jet(double v) : value(v) {}

    static Jet variable(double v, std::size_t i) {
        Jet result(v);
        result.gradient[i] = 1.0;
        return result;
    }

    Jet& operator+=(const Jet& rhs) {
        value += rhs.value;
        for (std::size_t i = 0; i < N; ++i) {
            gradient[i] += rhs.gradient[i];
            for (std::size_t j = 0; j < N; ++j) {
                hessian[i][j] += rhs.hessian[i][j];
            }
        }
        return *this;
    }

    friend Jet operator+(Jet a, const Jet& b) { return a += b; }
    friend Jet operator-(const Jet& a, const Jet& b) { return a + (-1.0 * b); }

    friend Jet operator*(double a, Jet b) {
        b.value *= a;
        for (std::size_t i = 0; i < N; ++i) {
            b.gradient[i] *= a;
            for (double& entry : b.hessian[i]) {
                entry *= a;
            }
        }
        return b;
    }

    friend Jet operator*(const Jet& a, const Jet& b) {
        Jet result(a.value * b.value);
        for (std::size_t i = 0; i < N; ++i) {
            result.gradient[i] = a.gradient[i]*b.value + a.value*b.gradient[i];
            for (std::size_t j = 0; j < N; ++j) {
                result.hessian[i][j] = a.hessian[i][j]*b.value + a.value*b.hessian[i][j]
                    + a.gradient[i]*b.gradient[j] + b.gradient[i]*a.gradient[j];
            }
        }
        return result;
    }

    Jet compose(double f, double df, double ddf) const {
        Jet result = df * *this;
        result.value = f;
        for (std::size_t i = 0; i < N; ++i) {
            for (std::size_t j = 0; j < N; ++j) {
                result.hessian[i][j] += ddf * gradient[i] * gradient[j];
            }
        }
        return result;
    }

    friend Jet sin(const Jet& a) {
        return a.compose(std::sin(a.value), std::cos(a.value), -std::sin(a.value));
    }
    friend Jet cos(const Jet& a) {
        return a.compose(std::cos(a.value), -std::sin(a.value), -std::cos(a.value));
    }
    friend Jet atan(const Jet& a) {
        const double derivative = 1.0/(1.0+a.value*a.value);
        return a.compose(std::atan(a.value), derivative, -2*a.value*derivative*derivative);
    }
    friend Jet sqrt(const Jet& a) {
        const double root = std::sqrt(a.value);
        return a.compose(root, 0.5/root, -0.25/(a.value*root));
    }
    friend Jet operator/(const Jet& a, const Jet& b) {
        const double inv = 1.0 / b.value;
        return a * b.compose(inv, -inv*inv, 2*inv*inv*inv);
    }
};
}
