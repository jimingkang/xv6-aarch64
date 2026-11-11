#include "include/feappv/elements.hpp"
#include "elements/common/geometry.hpp"
#include "elements/common/integration.hpp"

namespace fem {
using namespace detail;

namespace {
void membrane_column(Matrix& b, std::size_t col, double dx, double dy) {
    b[0][col] = b[2][col+1] = dx;
    b[1][col+1] = b[2][col] = dy;
}
}

Matrix solid_t3(const std::array<Point, 3>& x, double e, double nu, double t, Plane plane) {
    material(e, nu, t);
    const double twice_area = (x[1][0]-x[0][0])*(x[2][1]-x[0][1]) -
                              (x[2][0]-x[0][0])*(x[1][1]-x[0][1]);
    positive(twice_area);
    Matrix b(3, Vector(6));
    for (std::size_t i = 0; i < x.size(); ++i) {
        const auto& next = x[(i+1)%3];
        const auto& last = x[(i+2)%3];
        membrane_column(b, 2*i, (next[1]-last[1])/twice_area, (last[0]-next[0])/twice_area);
    }
    Matrix k = square(6);
    integrate(k, b, elasticity(e, nu, plane), t*twice_area/2);
    return k;
}

Matrix solid_q4(const Quad& nodes, double e, double nu, double t, Plane plane) {
    material(e, nu, t);
    validate_quad(nodes);
    const Matrix d = elasticity(e, nu, plane);
    Matrix k = square(8);
    for (double r : {-gauss, gauss}) {
        for (double s : {-gauss, gauss}) {
            const auto q = shape(nodes, r, s);
            Matrix b(3, Vector(8));
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                membrane_column(b, 2*i, q.dx[i], q.dy[i]);
            }
            integrate(k, b, d, t*q.det);
        }
    }
    return k;
}
}
