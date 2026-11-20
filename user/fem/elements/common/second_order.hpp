#pragma once

#include <array>
#include <cmath>
#include <cstddef>

namespace fem::detail {

// Value, gradient and Hessian with respect to element DOFs.
//
// The Hessian is symmetric, so only its upper triangle is stored, packed
// row by row (H(i,j), j >= i, at i*N - i*(i-1)/2 + (j-i)); symmetric()
// returns the full matrix (differentiate() in energy.hpp uses it).  That
// halves both the N*N arithmetic and the bytes every Jet copy and
// zero-fill moves, which dominate the finite-rotation shell and beam
// elements.  Subtraction also avoids the temporary a + (-1 * b) built.
template <std::size_t N>
struct Jet {
    static constexpr std::size_t H = N*(N+1)/2;
    double value = 0.0;
    std::array<double, N> gradient{};
    std::array<double, H> hessian{};   // packed upper triangle

    Jet() = default;
    Jet(double v) : value(v) {}

    static Jet variable(double v, std::size_t i) {
        Jet result(v);
        result.gradient[i] = 1.0;
        return result;
    }

    Jet& operator+=(const Jet& rhs) {
        value += rhs.value;
        for (std::size_t i = 0; i < N; ++i) gradient[i] += rhs.gradient[i];
        for (std::size_t k = 0; k < H; ++k) hessian[k] += rhs.hessian[k];
        return *this;
    }

    Jet& operator-=(const Jet& rhs) {
        value -= rhs.value;
        for (std::size_t i = 0; i < N; ++i) gradient[i] -= rhs.gradient[i];
        for (std::size_t k = 0; k < H; ++k) hessian[k] -= rhs.hessian[k];
        return *this;
    }

    friend Jet operator+(Jet a, const Jet& b) { return a += b; }
    friend Jet operator-(Jet a, const Jet& b) { return a -= b; }

    friend Jet operator*(double a, Jet b) {
        b.value *= a;
        for (std::size_t i = 0; i < N; ++i) b.gradient[i] *= a;
        for (std::size_t k = 0; k < H; ++k) b.hessian[k] *= a;
        return b;
    }

    friend Jet operator*(const Jet& a, const Jet& b) {
        Jet result(a.value * b.value);
        for (std::size_t i = 0; i < N; ++i) {
            result.gradient[i] = a.gradient[i]*b.value + a.value*b.gradient[i];
        }
        for (std::size_t i = 0, k = 0; i < N; ++i) {
            const double ai = a.gradient[i], bi = b.gradient[i];
            for (std::size_t j = i; j < N; ++j, ++k) {
                result.hessian[k] = a.hessian[k]*b.value + a.value*b.hessian[k]
                    + ai*b.gradient[j] + bi*a.gradient[j];
            }
        }
        return result;
    }

    Jet compose(double f, double df, double ddf) const {
        Jet result = df * *this;
        result.value = f;
        for (std::size_t i = 0, k = 0; i < N; ++i) {
            const double gi = ddf * gradient[i];
            for (std::size_t j = i; j < N; ++j, ++k) {
                result.hessian[k] += gi * gradient[j];
            }
        }
        return result;
    }

    // The full N x N Hessian.
    std::array<std::array<double, N>, N> symmetric() const {
        std::array<std::array<double, N>, N> full;
        for (std::size_t i = 0, k = 0; i < N; ++i) {
            for (std::size_t j = i; j < N; ++j, ++k) {
                full[i][j] = full[j][i] = hessian[k];
            }
        }
        return full;
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
