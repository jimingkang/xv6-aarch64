#include "feappv/model.hpp"
#include "elements/common/geometry.hpp"

#include <iomanip>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
constexpr double pi = 3.14159265358979323846;
constexpr double young = 2.1e11, nu = 0.3, span = 1.0;
constexpr double wave = pi/span;
constexpr double wave_squared = 2*wave*wave;
constexpr double kirchhoff_tip = 1e-5;

std::size_t node(int n, int i, int j) {
    return std::size_t(j*(n+1)+i);
}

fem::Point mesh_point(int n, int i, int j, double amplitude) {
    const double h = span/n;
    const bool interior = i > 0 && j > 0 && i < n && j < n;
    const bool center = i == n/2 && j == n/2;
    const double shift = interior && !center ? amplitude*h : 0.0;
    const double direction = (i+j)%2 == 0 ? 1.0 : -1.0;
    return {h*i+direction*shift, h*j+direction*shift};
}

struct PlateResult {
    double ratio;
    double jacobian_min;
};

PlateResult solve_plate(int n, double t, fem::PlateShear formulation, double amplitude = 0.0) {
    const double h = span/n;
    fem::Vector u(3*std::size_t(n+1)*(n+1)), load(u.size());
    fem::Model model(u.size());
    const double rigidity = young*t*t*t/(12*(1-nu*nu));
    const double pressure = rigidity*wave_squared*wave_squared*kirchhoff_tip;
    double jacobian_min = std::numeric_limits<double>::infinity();

    for (int j = 0; j <= n; ++j) {
        for (int i = 0; i <= n; ++i) {
            const auto base = 3*node(n,i,j);
            const bool x_edge = i == 0 || i == n;
            const bool y_edge = j == 0 || j == n;
            if (x_edge || y_edge) {
                model.prescribe(base);  // w = 0
            }
            if (x_edge) {
                model.prescribe(base+1);  // beta_y = -rx = 0
            }
            if (y_edge) {
                model.prescribe(base+2);  // beta_x = ry = 0
            }
        }
    }

    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const std::array<std::size_t,4> nodes{
                node(n,i,j),node(n,i+1,j),node(n,i+1,j+1),node(n,i,j+1)};
            const fem::Quad element{{mesh_point(n,i,j,amplitude),
                                     mesh_point(n,i+1,j,amplitude),
                                     mesh_point(n,i+1,j+1,amplitude),
                                     mesh_point(n,i,j+1,amplitude)}};
            for (double r : {-1.0, 1.0}) {
                for (double s : {-1.0, 1.0}) {
                    const auto q = fem::detail::shape(element, r, s);
                    jacobian_min = std::min(jacobian_min, 4*q.det/(h*h));
                }
            }
            fem::Dofs dofs;
            for (auto a : nodes) {
                dofs.insert(dofs.end(), {3*a,3*a+1,3*a+2});
            }
            model.add_element(std::move(dofs),
                              fem::linear(fem::plate_q4(element, young, nu, t, formulation)));
            for (double r : {-fem::detail::gauss, fem::detail::gauss}) {
                for (double s : {-fem::detail::gauss, fem::detail::gauss}) {
                    const auto q = fem::detail::shape(element, r, s);
                    double x = 0.0, y = 0.0;
                    for (std::size_t a = 0; a < nodes.size(); ++a) {
                        x += q.n[a]*element[a][0];
                        y += q.n[a]*element[a][1];
                    }
                    const double weight = pressure*std::sin(wave*x)*std::sin(wave*y)*q.det;
                    for (std::size_t a = 0; a < nodes.size(); ++a) {
                        load[3*nodes[a]] += weight*q.n[a];
                    }
                }
            }
        }
    }
    model.solve(u, load, 1e-8);
    return {u[3*node(n,n/2,n/2)]/kirchhoff_tip, jacobian_min};
}

void distorted_study() {
    for (double amplitude : {0.10, 0.22}) {
        constexpr int illustration_n = 8;
        for (int j = 0; j <= illustration_n; ++j) {
            for (int i = 0; i <= illustration_n; ++i) {
                const auto [x,y] = mesh_point(illustration_n,i,j,amplitude);
                std::cout << "MESH " << amplitude << ' ' << illustration_n << ' '
                          << i << ' ' << j << ' ' << x << ' ' << y << '\n';
            }
        }
        for (double t : {0.1, 0.03, 0.01, 0.003, 0.001}) {
            const double rm = 1 + t*t*wave_squared/(5*(1-nu));
            double previous_full_error = std::numeric_limits<double>::infinity();
            double previous_mitc_error = previous_full_error;
            for (int n : {4, 8, 12, 16, 20}) {
                const auto full = solve_plate(n, t, fem::PlateShear::full_integration, amplitude);
                const auto mitc = solve_plate(n, t, fem::PlateShear::mitc4, amplitude);
                std::cout << "DIST " << amplitude << ' ' << n << ' ' << t/span << ' '
                          << full.ratio << ' ' << mitc.ratio << ' ' << rm << ' '
                          << mitc.jacobian_min << '\n';
                const double full_error = std::abs(full.ratio-rm);
                const double mitc_error = std::abs(mitc.ratio-rm);
                if (!(full.ratio > 0 && mitc.ratio > 0 &&
                      full_error < previous_full_error && mitc_error < previous_mitc_error) ||
                    std::abs(mitc.jacobian_min-(1-4*amplitude)) > 1e-10) {
                    throw std::runtime_error("Invalid distorted mesh or nonconverging plate");
                }
                previous_full_error = full_error;
                previous_mitc_error = mitc_error;
                if (t == 0.001 && (!(full.ratio < 0.01) || !(std::abs(mitc.ratio-1) < 0.06))) {
                    throw std::runtime_error("Distorted thin plate did not distinguish shear formulations");
                }
            }
        }
    }
}
}

int main(int argc, char** argv) {
    try {
        std::cout << std::setprecision(12);
        if (argc == 2 && std::string_view(argv[1]) == "--distorted") {
            distorted_study();
            return 0;
        }
        if (argc != 1) {
            throw std::invalid_argument("Expected no arguments or --distorted");
        }
        std::cout << "PLATE n t_over_a full mitc4 rm_exact\n";
        for (double t : {0.1, 0.03, 0.01, 0.003, 0.001}) {
            const double rm = 1 + t*t*wave_squared/(5*(1-nu));
            double previous_full_error = 1.0, previous_mitc_error = 1.0;
            for (int n : {4, 8, 12, 16, 20}) {
                const double full = solve_plate(n, t, fem::PlateShear::full_integration).ratio;
                const double mitc = solve_plate(n, t, fem::PlateShear::mitc4).ratio;
                std::cout << "PLATE " << n << ' ' << t/span << ' '
                          << full << ' ' << mitc << ' ' << rm << '\n';

                const double full_error = std::abs(full-rm);
                const double mitc_error = std::abs(mitc-rm);
                if (!(full > 0 && mitc > 0 && full_error < previous_full_error &&
                      mitc_error < previous_mitc_error)) {
                    throw std::runtime_error("Plate grid refinement did not reduce center error");
                }
                previous_full_error = full_error;
                previous_mitc_error = mitc_error;
                if (t == 0.001 && (!(full < 0.03) || !(std::abs(mitc-1) < 0.08))) {
                    throw std::runtime_error("Thin plate did not distinguish the two shear formulations");
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
