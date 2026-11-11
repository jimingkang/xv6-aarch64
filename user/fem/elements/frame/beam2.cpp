#include "include/feappv/elements.hpp"
#include "elements/common/geometry.hpp"
#include "elements/common/integration.hpp"
#include "elements/common/energy.hpp"

namespace fem {
using namespace detail;

Matrix beam2(const Point& chord, double ea, double ei) {
    const double length = std::hypot(chord[0], chord[1]);
    positive(length);
    positive(ea);
    positive(ei);
    const double c = chord[0]/length, s = chord[1]/length;
    Matrix b(1, Vector{-c/length, -s/length, 0, c/length, s/length, 0});
    Matrix k = square(6);
    integrate(k, b, {{ea}}, length);

    for (double xi : {-gauss, gauss}) {
        // Second derivatives of the four cubic Hermite shape functions.
        const double v = 6.0 * xi / (length * length);
        b[0] = {-s*v, c*v, (3*xi-1)/length, s*v, -c*v, (3*xi+1)/length};
        integrate(k, b, {{ei}}, length/2);
    }
    return k;
}

Response beam2_finite(const Point& chord, double ea, double kga, double ei, const Vector& u) {
    const double length = std::hypot(chord[0], chord[1]);
    positive(length);
    positive(ea);
    positive(kga);
    positive(ei);
    if (u.size() != 6) {
        throw std::invalid_argument("Finite beam requires six displacement components");
    }

    const double cx = chord[0]/length, cy = chord[1]/length;
    const double dx = u[3]-u[0], dy = u[4]-u[1];
    const double gx = 1.0 + (cx*dx + cy*dy)/length;
    const double gy = (-cy*dx + cx*dy)/length;
    const double curvature = (u[5]-u[2])/length;
    const double angle = 0.5*(u[2]+u[5]);
    const double c = std::cos(angle), s = std::sin(angle);
    const double stretch = c*gx + s*gy;
    const double shear = c*gy - s*gx;

    // b2modl: resultants conjugate to (gx, gy, curvature, angle).
    const double axial_force = ea*(stretch-1.0);
    const double shear_force = kga*shear;
    const double fx = c*axial_force - s*shear_force;
    const double fy = s*axial_force + c*shear_force;
    const std::array<double, 4> force{fx, fy, ei*curvature, gy*fx-gx*fy};

    const std::array<double, 4> axial_gradient{c, s, 0, shear};
    const std::array<double, 4> shear_gradient{-s, c, 0, -stretch};
    Matrix a = square(4);
    for (std::size_t i = 0; i < a.size(); ++i) {
        for (std::size_t j = 0; j < a.size(); ++j) {
            a[i][j] = ea*axial_gradient[i]*axial_gradient[j]
                    + kga*shear_gradient[i]*shear_gradient[j];
        }
    }
    a[2][2] = ei;
    // Current-force terms in FEAPpv's gflag branch.
    a[0][3] -= fy;
    a[3][0] -= fy;
    a[1][3] += fx;
    a[3][1] += fx;
    a[3][3] -= gx*fx + gy*fy;

    // framf2d: midpoint shape functions and initial local-to-global mapping.
    const double x = cx/length, y = cy/length, inv_length = 1.0/length;
    const Matrix h{{-x, -y, 0, x, y, 0},
                   { y, -x, 0,-y, x, 0},
                   { 0,  0,-inv_length, 0, 0, inv_length},
                   { 0,  0, 0.5, 0, 0, 0.5}};
    Response result{Vector(6), square(6)};
    for (std::size_t i = 0; i < result.force.size(); ++i) {
        for (std::size_t j = 0; j < force.size(); ++j) {
            result.force[i] += length*h[j][i]*force[j];
        }
    }
    integrate(result.tangent, h, a, length);
    return result;
}

Response beam2_finite_ad(const Point& chord, double ea, double kga, double ei, const Vector& u) {
    const double length = std::hypot(chord[0], chord[1]);
    positive(length);
    positive(ea);
    positive(kga);
    positive(ei);
    const auto q = variables<6>(u);
    const double c = chord[0]/length, s = chord[1]/length;
    const auto dx = q[3] - q[0], dy = q[4] - q[1];
    const auto gx = 1.0 + (c/length)*dx + (s/length)*dy;
    const auto gy = (-s/length)*dx + (c/length)*dy;
    const auto angle = 0.5*(q[2]+q[5]);
    const auto axial = cos(angle)*gx + sin(angle)*gy - 1.0;
    const auto shear = cos(angle)*gy - sin(angle)*gx;
    const auto curvature = (1.0/length)*(q[5]-q[2]);
    return differentiate((0.5*length)*(ea*axial*axial + kga*shear*shear + ei*curvature*curvature));
}
}
