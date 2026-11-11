#include "feappv/model.hpp"

#include <iostream>
#include <numeric>
#include <string_view>

using namespace fem;

namespace {
void near(double actual, double expected, double tolerance) {
    if (!(std::abs(actual - expected) <= tolerance)) {
        throw std::runtime_error("Expected " + std::to_string(expected) +
                                 ", got " + std::to_string(actual));
    }
}

void add(Model& model, Matrix k) {
    Dofs dofs(k.size());
    std::iota(dofs.begin(), dofs.end(), 0);
    model.add_element(std::move(dofs), linear(std::move(k)));
}

void axial_bar() {
    Model bar(2);
    add(bar, bar1(2, 1000));
    bar.prescribe(0, 0.1);
    Vector u(2);
    const auto reaction = bar.solve(u, {0, 5});
    near(u[1], 0.11, 1e-12);
    near(reaction[0], -5, 1e-10);
}

void axial_truss() {
    Model truss(4);
    truss.add_element({0, 1, 2, 3}, [](const Vector& v) { return truss2({2, 0}, 1000, v); });
    truss.prescribe(0);
    truss.prescribe(1);
    truss.prescribe(3);
    Vector v(4);
    // Axial stretch lambda = 1.2, P = EA*(lambda^2-1)*lambda/2.
    const auto reaction = truss.solve(v, {0, 0, 264, 0});
    near(v[2], 0.4, 1e-10);
    near(reaction[0], -264, 1e-7);
}

void beam() {
    // Vertical member checks the local-to-global transformation.
    Model model(6);
    add(model, beam2({0, 2}, 1000, 20));
    for (auto i : {0, 1, 2}) {
        model.prescribe(i);
    }
    Vector u(6);
    const auto r = model.solve(u, {0, 0, 0, -3, 5, 0});
    near(u[3], -3.0*8/(3*20), 1e-12);
    near(u[4], 5.0*2/1000, 1e-12);
    near(u[5], 3.0*4/(2*20), 1e-12);
    near(r[0], 3, 1e-10);
    near(r[1], -5, 1e-10);
    near(r[2], -6, 1e-10);
}

constexpr Quad unit_square{{{0,0}, {1,0}, {1,1}, {0,1}}};

void plane_patch(bool triangle, Plane plane) {
    constexpr std::array<Point, 9> x{{
        {0,0}, {0.5,0}, {1,0}, {0,0.5}, {0.43,0.56}, {1,0.5}, {0,1}, {0.5,1}, {1,1}}};
    constexpr std::array<std::array<std::size_t, 4>, 4> cells{{
        {0,1,4,3}, {1,2,5,4}, {3,4,7,6}, {4,5,8,7}}};
    Model model(18);
    auto add_cell = [&](const auto& connectivity, Matrix stiffness) {
        Dofs dofs;
        for (auto i : connectivity) {
            dofs.push_back(2*i);
            dofs.push_back(2*i+1);
        }
        model.add_element(std::move(dofs), linear(std::move(stiffness)));
    };
    for (const auto& [a,b,c,d] : cells) {
        if (triangle) {
            add_cell(std::array{a,b,c}, solid_t3({x[a],x[b],x[c]}, 1000, 0.25, 0.1, plane));
            add_cell(std::array{a,c,d}, solid_t3({x[a],x[c],x[d]}, 1000, 0.25, 0.1, plane));
        } else {
            add_cell(std::array{a,b,c,d},
                     solid_q4({x[a],x[b],x[c],x[d]}, 1000, 0.25, 0.1, plane));
        }
    }
    Vector exact(18);
    for (std::size_t i = 0; i < x.size(); ++i) {
        exact[2*i] = 0.01*x[i][0] + 0.02*x[i][1];
        exact[2*i+1] = -0.005*x[i][0] - 0.003*x[i][1];
        if (i != 4) {
            model.prescribe(2*i, exact[2*i]);
            model.prescribe(2*i+1, exact[2*i+1]);
        }
    }
    Vector u(18);
    const auto r = model.solve(u, Vector(18));
    near(u[8], exact[8], 1e-12);
    near(u[9], exact[9], 1e-12);
    // Right-edge traction resultant for the uniform strain field.
    const double stress_x = plane == Plane::stress
        ? (1000.0/(1-0.25*0.25)) * (0.01 - 0.25*0.003)
        : (1000.0/(1.25*0.5)) * (0.75*0.01 - 0.25*0.003);
    near(r[4]+r[10]+r[16], 0.1*stress_x, 1e-12);
    near(r[5]+r[11]+r[17], 0.1*(1000/2.5)*(0.02-0.005), 1e-12);
}

void plate_bending() {
    constexpr double t = 0.01, e = 1.2e7, moment = 0.001;
    constexpr double rigidity = e*t*t*t/12;
    Model plate(12);
    add(plate, plate_q4(unit_square, e, 0, t));
    Vector p(12), up(12);
    for (auto i : {0, 1, 2, 9, 10, 11}) {
        plate.prescribe(i);
    }
    p[5] = p[8] = -moment/2;
    const auto rp = plate.solve(up, p);
    near(up[3], moment/(2*rigidity), 1e-10);
    near(up[6], up[3], 1e-10);
    near(up[5], -moment/rigidity, 1e-10);
    near(rp[2]+rp[11], moment, 1e-10);
}

void shell_bending() {
    constexpr double t = 0.01, e = 1.2e7, moment = 0.001;
    constexpr double rigidity = e*t*t*t/12;
    // The panel normal points along -y.
    const Quad3 x{{{0,0,0}, {1,0,0}, {1,0,1}, {0,0,1}}};
    Model shell(24);
    const auto k = shell_q4(x, e, 0, t);
    add(shell, k);
    Vector load(24), u(24);
    for (auto i : {0, 1, 2, 3, 4, 5, 18, 19, 20, 21, 22, 23}) {
        shell.prescribe(i);
    }
    load[11] = load[17] = -moment/2;
    const auto r = shell.solve(u, load);
    near(u[7], -moment/(2*rigidity), 1e-10);
    near(u[13], -moment/(2*rigidity), 1e-10);
    near(u[11], -moment/rigidity, 1e-10);
    near(r[5]+r[23], moment, 1e-10);

    // Rigid translation and rotation about each global axis have zero internal force.
    const auto law = linear(k);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        Vector motion(24);
        const auto j = (axis+1)%3, m = (axis+2)%3;
        for (std::size_t i = 0; i < x.size(); ++i) {
            motion[6*i+axis] = 1;
            motion[6*i+j] = -x[i][m];
            motion[6*i+m] = x[i][j];
            motion[6*i+3+axis] = 1;
        }
        for (double f : law(motion).force) {
            near(f, 0, 1e-9);
        }
    }
}
}

int main(int argc, char** argv) {
    try {
        const std::string_view selected = argc > 1 ? argv[1] : "all";
        const std::pair<std::string_view, void(*)()> cases[] = {
            {"bar1", axial_bar},
            {"truss2", axial_truss},
            {"beam2", beam},
            {"solid_t3", [] {
                plane_patch(true, Plane::stress);
                plane_patch(true, Plane::strain);
            }},
            {"solid_q4", [] {
                plane_patch(false, Plane::stress);
                plane_patch(false, Plane::strain);
            }},
            {"plate_q4", plate_bending},
            {"shell_q4", shell_bending},
        };
        bool ran = false;
        for (const auto& [name, run] : cases) {
            if (selected == "all" || selected == name) {
                run();
                ran = true;
                std::cout << name << " passed\n";
            }
        }
        if (!ran || argc > 2) {
            throw std::invalid_argument("Unknown element benchmark argument");
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
