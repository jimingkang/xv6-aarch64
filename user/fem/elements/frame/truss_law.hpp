#pragma once
#include "include/feappv/types.hpp"
#include <cmath>
#include <stdexcept>

namespace fem {
using ElementVector = std::array<double, 4>;

struct TrussLaw {
    double axial_rigidity;  // E * A0, in N

    struct Response {
        ElementVector force{};
        std::array<ElementVector, 4> tangent{};
    };

    Response evaluate(const Point& reference_chord, const ElementVector& u) const {
        const double dx = reference_chord[0] + u[2] - u[0];
        const double dy = reference_chord[1] + u[3] - u[1];
        const double length_squared =
            reference_chord[0] * reference_chord[0] +
            reference_chord[1] * reference_chord[1];
        if (!(length_squared > 0.0)) {
            throw std::runtime_error("Invalid reference length");
        }
        const double strain = 0.5 * ((dx * dx + dy * dy) / length_squared - 1.0);
        const double scale = axial_rigidity / std::sqrt(length_squared);
        const ElementVector g{-dx, -dy, dx, dy};

        Response response;
        for (std::size_t i = 0; i < g.size(); ++i) {
            response.force[i] = scale * strain * g[i];
            for (std::size_t j = 0; j < g.size(); ++j) {
                // D^T D has blocks I, -I, -I, I.
                const double geometric = i % 2 == j % 2
                    ? (i / 2 == j / 2 ? strain : -strain)
                    : 0.0;
                response.tangent[i][j] = scale * (g[i] * g[j] / length_squared + geometric);
            }
        }
        return response;
    }
};

}  // namespace fem
