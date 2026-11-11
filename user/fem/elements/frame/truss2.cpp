#include "include/feappv/elements.hpp"
#include "truss_law.hpp"
#include "elements/common/geometry.hpp"
#include "elements/common/integration.hpp"

namespace fem {
using namespace detail;

Response truss2(const Point& chord, double ea, const Vector& u) {
    positive(ea);
    positive(std::hypot(chord[0], chord[1]));
    if (u.size() != 4) {
        throw std::invalid_argument("Truss2 requires four displacement components");
    }
    const auto response = TrussLaw{ea}.evaluate(chord, {u[0], u[1], u[2], u[3]});
    Response result{Vector(response.force.begin(), response.force.end()), square(4)};
    for (std::size_t i = 0; i < 4; ++i) {
        std::copy(response.tangent[i].begin(), response.tangent[i].end(), result.tangent[i].begin());
    }
    return result;
}
}
