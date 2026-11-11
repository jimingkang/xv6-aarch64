#pragma once
#include "include/feappv/elements.hpp"
#include <cmath>
#include <stdexcept>

namespace fem::detail {
inline void positive(double value) {
    if (!(value > 0.0) || !std::isfinite(value)) {
        throw std::invalid_argument("Expected a positive finite material or geometry parameter");
    }
}

inline void material(double e, double nu, double t) {
    positive(e);
    positive(t);
    if (!(nu > -1.0 && nu < 0.5)) {
        throw std::invalid_argument("Poisson ratio must be in (-1, 0.5)");
    }
}

struct QuadPoint {
    std::array<double, 4> n, dr, ds, dx, dy;
    double xr, yr, xs, ys, det;
};

inline QuadPoint shape(const Quad& nodes, double r, double s) {
    constexpr std::array<double, 4> rnode{-1, 1, 1, -1}, snode{-1, -1, 1, 1};
    QuadPoint q{};
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        q.n[i] = 0.25 * (1 + r * rnode[i]) * (1 + s * snode[i]);
        q.dr[i] = 0.25 * rnode[i] * (1 + s * snode[i]);
        q.ds[i] = 0.25 * snode[i] * (1 + r * rnode[i]);
        q.xr += q.dr[i] * nodes[i][0];
        q.yr += q.dr[i] * nodes[i][1];
        q.xs += q.ds[i] * nodes[i][0];
        q.ys += q.ds[i] * nodes[i][1];
    }
    q.det = q.xr * q.ys - q.xs * q.yr;
    if (!(q.det > 0.0) || !std::isfinite(q.det)) {
        throw std::invalid_argument("Q4 requires a nondegenerate, counterclockwise quadrilateral");
    }
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        q.dx[i] = (q.ys * q.dr[i] - q.yr * q.ds[i]) / q.det;
        q.dy[i] = (-q.xs * q.dr[i] + q.xr * q.ds[i]) / q.det;
    }
    return q;
}

inline void validate_quad(const Quad& nodes) {
    // The bilinear mapping determinant is affine on the parent square.
    for (double r : {-1.0, 1.0}) {
        for (double s : {-1.0, 1.0}) {
            shape(nodes, r, s);
        }
    }
}

constexpr double gauss = 0.5773502691896257645;

inline Point3 difference(const Point3& a, const Point3& b) {
    return {a[0]-b[0], a[1]-b[1], a[2]-b[2]};
}
inline double dot(const Point3& a, const Point3& b) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
inline Point3 cross(const Point3& a, const Point3& b) {
    return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
}
inline Point3 unit(Point3 a) {
    const double norm = std::sqrt(dot(a, a));
    positive(norm);
    for (double& x : a) {
        x /= norm;
    }
    return a;
}

struct ShellReference {
    std::array<Point3, 3> basis;
    Quad local;
};

inline ShellReference shell_reference(const Quad3& nodes) {
    const auto e1 = unit(difference(nodes[1], nodes[0]));
    const auto e3 = unit(cross(e1, difference(nodes[3], nodes[0])));
    const auto e2 = cross(e3, e1);
    ShellReference result{{e1, e2, e3}, {}};
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto offset = difference(nodes[i], nodes[0]);
        if (!(std::abs(dot(e3, offset)) <= 1e-10 * std::sqrt(dot(offset, offset)))) {
            throw std::invalid_argument("Flat shell requires coplanar reference nodes");
        }
        result.local[i] = {dot(e1, offset), dot(e2, offset)};
    }
    return result;
}
}
