#include "include/feappv/model.hpp"
#include "elements/common/geometry.hpp"

namespace fem {
namespace {
struct JacobianCoefficients {
    double a, b;
    double da, db;  // Derivatives with respect to squared angle.
};

JacobianCoefficients coefficients(double t) {
    if (t < 0.01) {
        // Use the same polynomial for the value and its derivative.
        return {0.5 + t*(-1.0/24 + t*(1.0/720 - t/40320)),
                1.0/6 + t*(-1.0/120 + t*(1.0/5040 - t/362880)),
                -1.0/24 + t*(1.0/360 - t/13440),
                -1.0/120 + t*(1.0/2520 - t/120960)};
    }
    const double angle = std::sqrt(t);
    const double s = std::sin(angle);
    const double half_sine = std::sin(angle/2);
    const double one_minus_cos = 2*half_sine*half_sine;
    const double angle_minus_sin = angle-s;
    return {one_minus_cos/t, angle_minus_sin/(angle*t),
            (0.5*angle*s-one_minus_cos)/(t*t),
            (angle*one_minus_cos-3*angle_minus_sin)/(2*angle*t*t)};
}
}

Response spatial_moment_load(const Point3& theta, const Point3& moment) {
    const double t = detail::dot(theta, theta);
    if (!std::isfinite(t) || !std::all_of(moment.begin(), moment.end(),
                                         [](double x) { return std::isfinite(x); })) {
        throw std::invalid_argument("Non-finite rotation or spatial moment");
    }
    const auto [a, b, da, db] = coefficients(t);
    const auto first = detail::cross(theta, moment);
    const auto second = detail::cross(theta, first);

    Response result{Vector(3), Matrix(3, Vector(3))};
    for (std::size_t i = 0; i < theta.size(); ++i) {
        result.force[i] = moment[i] - a*first[i] + b*second[i];
    }
    for (std::size_t j = 0; j < theta.size(); ++j) {
        Point3 direction{};
        direction[j] = 1.0;
        const auto d_first = detail::cross(direction, moment);
        const auto d_second_left = detail::cross(direction, first);
        const auto d_second_right = detail::cross(theta, d_first);
        const double dt = 2*theta[j];
        for (std::size_t i = 0; i < theta.size(); ++i) {
            result.tangent[i][j] = -dt*da*first[i] - a*d_first[i]
                + dt*db*second[i] + b*(d_second_left[i] + d_second_right[i]);
        }
    }
    return result;
}
}
