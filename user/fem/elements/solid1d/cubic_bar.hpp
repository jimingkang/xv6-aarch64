#pragma once

namespace fem {
struct BarElement {
    double length;
    double area;
    double young;
    double cubic;

    struct Response {
        double axial_force;
        double stiffness;
    };

    Response evaluate(double left, double right) const {
        const double strain = (right - left) / length;
        const double stress = (young + cubic * strain * strain) * strain;
        const double tangent_modulus = young + 3.0 * cubic * strain * strain;
        return {area * stress, area * tangent_modulus / length};
    }
};
}
