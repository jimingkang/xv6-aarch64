#include "include/feappv/elements.hpp"
#include "elements/common/geometry.hpp"
#include "elements/common/integration.hpp"
#include "elements/common/energy.hpp"

namespace fem {
using namespace detail;

namespace {
using Scalar = Jet<24>;
using Spatial = std::array<Scalar, 3>;
using Nodal = std::array<Spatial, 4>;

Scalar dot(const Spatial& a, const Spatial& b) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

Spatial cross(const Spatial& a, const Spatial& b) {
    return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
}

struct Rotation {
    Spatial theta;
    Scalar sinc;
    Scalar cosc;

    Spatial apply(const Point3& axis) const {
        const auto first = cross(theta, {axis[0], axis[1], axis[2]});
        const auto second = cross(theta, first);
        return {axis[0]+sinc*first[0]+cosc*second[0],
                axis[1]+sinc*first[1]+cosc*second[1],
                axis[2]+sinc*first[2]+cosc*second[2]};
    }
};

Rotation rotation(const Spatial& theta) {
    const auto a = dot(theta, theta);
    if (a.value < 1e-4) {
        // Taylor series in squared angle, regular through zero rotation.
        const auto sinc = 1.0 + a*(-1.0/6 + a*(1.0/120 + a*(-1.0/5040)));
        const auto cosc = 0.5 + a*(-1.0/24 + a*(1.0/720 + a*(-1.0/40320)));
        return {theta, sinc, cosc};
    }
    const auto phi = sqrt(a);
    const auto half_sine = sin(0.5*phi);
    return {theta, sin(phi)/phi, 2.0*half_sine*half_sine/a};
}

Spatial interpolate(const Nodal& field, const std::array<double, 4>& shape) {
    Spatial result{};
    for (std::size_t i = 0; i < field.size(); ++i) {
        for (std::size_t j = 0; j < result.size(); ++j) {
            result[j] += shape[i] * field[i][j];
        }
    }
    return result;
}

Scalar plane_energy(const Scalar& x, const Scalar& y, const Scalar& shear, double nu) {
    return x*x + y*y + (2*nu)*x*y + (0.5*(1-nu))*shear*shear;
}
}

Matrix shell_q4(const Quad3& nodes, double e, double nu, double t) {
    const auto [basis, local] = shell_reference(nodes);
    const Matrix membrane = solid_q4(local, e, nu, t, Plane::stress);
    const Matrix plate = plate_q4(local, e, nu, t);
    Matrix k = square(24);
    for (std::size_t i = 0; i < membrane.size(); ++i) {
        for (std::size_t j = 0; j < membrane.size(); ++j) {
            k[6*(i/2)+i%2][6*(j/2)+j%2] = membrane[i][j];
        }
    }
    for (std::size_t i = 0; i < plate.size(); ++i) {
        for (std::size_t j = 0; j < plate.size(); ++j) {
            k[6*(i/3)+2+i%3][6*(j/3)+2+j%3] = plate[i][j];
        }
    }
    // Weakly tie the drilling rotation to in-plane continuum spin.
    const Matrix drilling{{1e-3 * e/(2*(1+nu)) * t}};
    for (double r : {-gauss, gauss}) {
        for (double s : {-gauss, gauss}) {
            const auto q = shape(local, r, s);
            Matrix b(1, Vector(24));
            for (std::size_t i = 0; i < local.size(); ++i) {
                b[0][6*i] = q.dy[i]/2;
                b[0][6*i+1] = -q.dx[i]/2;
                b[0][6*i+5] = q.n[i];
            }
            integrate(k, b, drilling, q.det);
        }
    }
    Matrix global = square(24);
    for (std::size_t i = 0; i < k.size(); ++i) {
        for (std::size_t j = 0; j < k.size(); ++j) {
            for (std::size_t a = 0; a < 3; ++a) {
                for (std::size_t b = 0; b < 3; ++b) {
                    global[3*(i/3)+a][3*(j/3)+b] += basis[i%3][a]*k[i][j]*basis[j%3][b];
                }
            }
        }
    }
    return global;
}

Response shell_q4_finite(const Quad3& nodes, double e, double nu, double t, const Vector& u) {
    material(e, nu, t);
    const auto q = variables<24>(u);
    const auto reference = shell_reference(nodes);
    const auto& local = reference.local;
    const auto& [e1, e2, e3] = reference.basis;
    Nodal position, director, axis1, axis2;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto offset = difference(nodes[i], nodes[0]);
        for (std::size_t j = 0; j < 3; ++j) {
            position[i][j] = offset[j] + q[6*i+j];
        }
        const auto r = rotation({q[6*i+3], q[6*i+4], q[6*i+5]});
        director[i] = r.apply(e3);
        axis1[i] = r.apply(e1);
        axis2[i] = r.apply(e2);
    }
    validate_quad(local);
    auto tied = [&](double r, double s, bool along_r) {
        const auto p = shape(local, r, s);
        return dot(interpolate(position, along_r ? p.dr : p.ds), interpolate(director, p.n));
    };
    const auto bottom = tied(0, -1, true), top = tied(0, 1, true);
    const auto left = tied(-1, 0, false), right = tied(1, 0, false);
    const double membrane = e*t/(1-nu*nu);
    const double bending = membrane*t*t/12;
    const double shear = (5.0/6)*e*t/(2*(1+nu));
    const double drilling = 1e-3*e*t/(2*(1+nu));
    Scalar energy;
    for (double r : {-gauss, gauss}) {
        for (double s : {-gauss, gauss}) {
            const auto p = shape(local, r, s);
            const auto ax = interpolate(position, p.dx), ay = interpolate(position, p.dy);
            const auto nx = interpolate(director, p.dx), ny = interpolate(director, p.dy);
            const auto ex = 0.5*(dot(ax, ax)-1), ey = 0.5*(dot(ay, ay)-1);
            const auto kx = dot(ax, nx), ky = dot(ay, ny);
            const auto kxy = dot(ax, ny)+dot(ay, nx);
            const auto gr = 0.5*((1-s)*bottom+(1+s)*top);
            const auto gs = 0.5*((1-r)*left+(1+r)*right);
            const auto gx = (p.ys/p.det)*gr-(p.yr/p.det)*gs;
            const auto gy = (p.xr/p.det)*gs-(p.xs/p.det)*gr;
            const auto spin = 0.5*(dot(ay, interpolate(axis1, p.n)) -
                                   dot(ax, interpolate(axis2, p.n)));
            energy += (0.5*p.det)*(membrane*plane_energy(ex, ey, dot(ax, ay), nu)
                                 + bending*plane_energy(kx, ky, kxy, nu)
                                 + shear*(gx*gx+gy*gy) + drilling*spin*spin);
        }
    }
    return differentiate(energy);
}
}
