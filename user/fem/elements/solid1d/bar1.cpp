#include "include/feappv/elements.hpp"
#include "elements/common/geometry.hpp"

namespace fem {
using namespace detail;

Matrix bar1(double length, double ea) {
    positive(length);
    positive(ea);
    const double k = ea / length;
    return {{k, -k}, {-k, k}};
}
}
