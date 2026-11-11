#include "include/feappv/elements.hpp"
#include "elements/common/energy.hpp"
#include "elements/common/geometry.hpp"

namespace fem {
namespace {
using Scalar = detail::Jet<12>;
using Spatial = std::array<Scalar, 3>;

Scalar dot(const Spatial& a, const Spatial& b) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

Spatial cross(const Spatial& a, const Spatial& b) {
    return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
}

struct Quaternion {
    Scalar w;
    Spatial v;

    Quaternion conjugate() const { return {w, {-1.0*v[0], -1.0*v[1], -1.0*v[2]}}; }
};

Quaternion operator*(const Quaternion& a, const Quaternion& b) {
    const auto c = cross(a.v, b.v);
    return {a.w*b.w-dot(a.v,b.v),
            {a.w*b.v[0]+b.w*a.v[0]+c[0],
             a.w*b.v[1]+b.w*a.v[1]+c[1],
             a.w*b.v[2]+b.w*a.v[2]+c[2]}};
}

Quaternion rotation(const Spatial& theta) {
    const auto a = dot(theta, theta);
    constexpr double chart_limit = 6.283185307179586 - 1e-6;
    if (!(a.value < chart_limit*chart_limit)) {
        throw std::domain_error("Beam rotation vector reached the 2*pi chart limit");
    }
    Scalar w, scale;
    if (a.value < 1e-6) {
        w = 1.0 + a*(-1.0/8 + a*(1.0/384 - (1.0/46080)*a));
        scale = 0.5 + a*(-1.0/48 + a*(1.0/3840 - (1.0/645120)*a));
    } else {
        const auto half_angle = 0.5*sqrt(a);
        w = cos(half_angle);
        scale = 0.5*sin(half_angle)/half_angle;
    }
    return {w, {scale*theta[0], scale*theta[1], scale*theta[2]}};
}

Spatial rotation_log(const Quaternion& q) {
    const auto a = dot(q.v, q.v);
    Scalar scale;
    if (a.value < 1e-6) {
        // 2 asin(sqrt(a))/sqrt(a) for the unit quaternion near identity.
        scale = 2.0 + a*(1.0/3 + a*(3.0/20 + (5.0/56)*a));
    } else {
        const auto v = sqrt(a);
        scale = 2.0*atan(v/q.w)/v;
    }
    return {scale*q.v[0], scale*q.v[1], scale*q.v[2]};
}
}

Response beam3_simo(const Point3& chord, const Point3& transverse,
                    const Beam3Section& section, const Vector& u) {
    const double length = std::sqrt(detail::dot(chord, chord));
    detail::positive(length);
    const auto e1 = detail::unit(chord);
    const auto e3 = detail::unit(detail::cross(e1, transverse));
    const auto e2 = detail::cross(e3, e1);
    const std::array<Point3, 3> basis{e1, e2, e3};
    for (std::size_t i = 0; i < basis.size(); ++i) {
        detail::positive(section.force_rigidity[i]);
        detail::positive(section.moment_rigidity[i]);
    }

    const auto q = detail::variables<12>(u);
    const auto left = rotation({q[3], q[4], q[5]});
    const auto right = rotation({q[9], q[10], q[11]});
    auto relative = left.conjugate()*right;
    if (std::abs(relative.w.value) < 1e-8) {
        throw std::domain_error("Beam relative rotation is at the pi logarithm cut");
    }
    if (relative.w.value < 0.0) {
        relative = {-1.0*relative.w, {-1.0*relative.v[0], -1.0*relative.v[1], -1.0*relative.v[2]}};
    }

    // Unit quaternion square root on the principal arc.
    const auto midpoint_w = sqrt(0.5*(1.0+relative.w));
    const auto scale = 0.5/midpoint_w;
    const Quaternion half{midpoint_w, {scale*relative.v[0], scale*relative.v[1], scale*relative.v[2]}};
    const auto midpoint = left*half;
    const auto rotation_difference = rotation_log(relative);
    Spatial tangent;
    for (std::size_t i = 0; i < tangent.size(); ++i) {
        tangent[i] = e1[i] + (1.0/length)*(q[6+i]-q[i]);
    }
    const auto material_tangent = (midpoint.conjugate()*Quaternion{0, tangent}*midpoint).v;

    Scalar energy;
    for (std::size_t i = 0; i < basis.size(); ++i) {
        const Spatial director{basis[i][0], basis[i][1], basis[i][2]};
        const auto strain = dot(director, material_tangent) - (i == 0 ? 1.0 : 0.0);
        const auto curvature = (1.0/length)*dot(director, rotation_difference);
        energy += (0.5*length)*(section.force_rigidity[i]*strain*strain
                               + section.moment_rigidity[i]*curvature*curvature);
    }
    return detail::differentiate(energy);
}
}
