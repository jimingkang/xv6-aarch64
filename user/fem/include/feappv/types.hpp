#pragma once
#include <array>
#include <vector>

namespace fem {
using Vector = std::vector<double>;
using Matrix = std::vector<Vector>;
using Point = std::array<double, 2>;
using Point3 = std::array<double, 3>;
using Quad = std::array<Point, 4>;
using Quad3 = std::array<Point3, 4>;

struct Response {
    Vector force;
    Matrix tangent;
};
}
