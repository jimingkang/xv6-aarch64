#pragma once
#include "types.hpp"

namespace fem {
Response truss2(const Point& chord, double ea, const Vector& displacement);
Matrix bar1(double length, double ea);
Matrix beam2(const Point& chord, double ea, double ei);
// Midpoint finite-rotation beam with elastic axial, shear and bending resultants.
Response beam2_finite(const Point& chord, double ea, double kga, double ei,
                      const Vector& displacement);
// Same element evaluated by differentiating the discrete energy with AD.
Response beam2_finite_ad(const Point& chord, double ea, double kga, double ei,
                         const Vector& displacement);

struct Beam3Section {
    Point3 force_rigidity;   // EA, k2 GA, k3 GA
    Point3 moment_rigidity;  // GJ, EI2, EI3
};

// Each node has global translation followed by a spatial rotation vector.
// transverse points along the approximate second reference director.
Response beam3_simo(const Point3& chord, const Point3& transverse,
                    const Beam3Section& section, const Vector& displacement);

enum class Plane { stress, strain };
Matrix solid_t3(const std::array<Point, 3>& nodes, double e, double nu,
                double thickness, Plane plane);
Matrix solid_q4(const Quad& nodes, double e, double nu, double thickness, Plane plane);

// Plate DOFs are (w, rx, ry); shell DOFs are (ux, uy, uz, rx, ry, rz).
Matrix plate_q4(const Quad& nodes, double e, double nu, double thickness);
enum class PlateShear { mitc4, full_integration };
Matrix plate_q4(const Quad& nodes, double e, double nu, double thickness, PlateShear formulation);
Matrix shell_q4(const Quad3& nodes, double e, double nu, double thickness);
// Flat reference panel; the last three DOFs per node are a spatial rotation vector.
Response shell_q4_finite(const Quad3& nodes, double e, double nu, double thickness,
                         const Vector& displacement);
}
