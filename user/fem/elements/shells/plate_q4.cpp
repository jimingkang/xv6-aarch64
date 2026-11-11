#include "include/feappv/elements.hpp"
#include "elements/common/geometry.hpp"
#include "elements/common/integration.hpp"

namespace fem {
using namespace detail;

namespace {
// Covariant gamma_r or gamma_s at its edge midpoint.
// beta = (ry, -rx) and gamma = grad(w) + beta.
Vector tied_shear(const QuadPoint& q, bool along_r) {
    Vector row(12);
    for (std::size_t i = 0; i < q.n.size(); ++i) {
        row[3*i] = along_r ? q.dr[i] : q.ds[i];
        row[3*i+1] = -(along_r ? q.yr : q.ys) * q.n[i];
        row[3*i+2] = (along_r ? q.xr : q.xs) * q.n[i];
    }
    return row;
}


}

Matrix plate_q4(const Quad& nodes, double e, double nu, double t) {
    return plate_q4(nodes, e, nu, t, PlateShear::mitc4);
}

Matrix plate_q4(const Quad& nodes, double e, double nu, double t, PlateShear formulation) {
    material(e, nu, t);
    validate_quad(nodes);
    const Matrix d = elasticity(e, nu, Plane::stress);
    const double shear_rigidity = (5.0/6) * e/(2*(1+nu)) * t;
    const Matrix shear{{shear_rigidity, 0}, {0, shear_rigidity}};
    const auto bottom = tied_shear(shape(nodes, 0, -1), true);
    const auto top = tied_shear(shape(nodes, 0, 1), true);
    const auto left = tied_shear(shape(nodes, -1, 0), false);
    const auto right = tied_shear(shape(nodes, 1, 0), false);
    Matrix k = square(12);

    for (double r : {-gauss, gauss}) {
        for (double s : {-gauss, gauss}) {
            const auto q = shape(nodes, r, s);
            Matrix bending(3, Vector(12)), bs(2, Vector(12));
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                bending[0][3*i+2] = q.dx[i];
                bending[1][3*i+1] = -q.dy[i];
                bending[2][3*i+1] = -q.dx[i];
                bending[2][3*i+2] = q.dy[i];
            }
            if (formulation == PlateShear::mitc4) {
                for (std::size_t i = 0; i < k.size(); ++i) {
                    const double gr = 0.5*((1-s)*bottom[i] + (1+s)*top[i]);
                    const double gs = 0.5*((1-r)*left[i] + (1+r)*right[i]);
                    bs[0][i] = (q.ys*gr - q.yr*gs)/q.det;
                    bs[1][i] = (-q.xs*gr + q.xr*gs)/q.det;
                }
            } else {
                for (std::size_t i = 0; i < nodes.size(); ++i) {
                    bs[0][3*i] = q.dx[i];
                    bs[0][3*i+2] = q.n[i];
                    bs[1][3*i] = q.dy[i];
                    bs[1][3*i+1] = -q.n[i];
                }
            }
            integrate(k, bending, d, (t*t*t/12)*q.det);
            integrate(k, bs, shear, q.det);
        }
    }
    return k;
}
}
