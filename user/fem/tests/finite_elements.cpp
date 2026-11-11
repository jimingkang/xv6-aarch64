#include "feappv/model.hpp"

#include <iomanip>
#include <iostream>
#include <string_view>

using namespace fem;

namespace {
constexpr int steps = 20;
constexpr int beam_elements = 16;
constexpr double pi = 3.14159265358979323846;

void near(double value, double expected, double tolerance) {
    if (!(std::abs(value-expected) <= tolerance)) {
        throw std::runtime_error("Expected " + std::to_string(expected) +
                                 ", got " + std::to_string(value));
    }
}

void tangent_check(const ElementLaw& law, const Vector& u, double scale) {
    const auto exact = law(u);
    constexpr double h = 1e-6;
    for (std::size_t j = 0; j < u.size(); ++j) {
        auto plus = u, minus = u;
        plus[j] += h;
        minus[j] -= h;
        const auto fp = law(plus).force, fm = law(minus).force;
        for (std::size_t i = 0; i < u.size(); ++i) {
            near(exact.tangent[i][j], exact.tangent[j][i], 1e-13*scale);
            near((fp[i]-fm[i])/(2*h), exact.tangent[i][j], 2e-8*scale);
        }
    }
}

void beam_tangent() {
    const Point chord{0.8, 0.6};
    const ElementLaw beam = [&](const Vector& u) { return beam2_finite(chord, 1000, 400, 10, u); };
    const Vector deformed{0.1, 0.02, 0.4, -0.2, 0.7, 0.9};
    tangent_check(beam, deformed, 1000);
    for (const auto& u : {Vector(6), deformed}) {
        const auto analytic = beam(u);
        const auto ad = beam2_finite_ad(chord, 1000, 400, 10, u);
        for (std::size_t i = 0; i < u.size(); ++i) {
            near(analytic.force[i], ad.force[i], 1e-10);
            for (std::size_t j = 0; j < u.size(); ++j) {
                near(analytic.tangent[i][j], ad.tangent[i][j], 1e-10);
            }
        }
    }
    const double angle = 0.9, c = std::cos(angle), s = std::sin(angle);
    const double dx = (c-1)*chord[0]-s*chord[1], dy = s*chord[0]+(c-1)*chord[1];
    for (double f : beam({0.1, 0.2, angle, 0.1+dx, 0.2+dy, angle}).force) {
        near(f, 0, 1e-11);
    }

}

void shell_tangent() {
    const double angle = 0.9, c = std::cos(angle), s = std::sin(angle);
    const Quad3 x{{{0,0,0}, {1.2,0,0}, {1.1,0.9,0}, {0,1,0}}};
    const ElementLaw shell = [&](const Vector& u) { return shell_q4_finite(x, 1e6, 0.25, 0.01, u); };
    Vector deformed(24), rigid(24);
    const auto finite = shell(Vector(24)).tangent;
    const auto linear_shell = shell_q4(x, 1e6, 0.25, 0.01);
    for (std::size_t i = 0; i < finite.size(); ++i) {
        for (std::size_t j = 0; j < finite.size(); ++j) {
            near(finite[i][j], linear_shell[i][j], 1e-10);
        }
    }
    for (std::size_t i = 0; i < x.size(); ++i) {
        rigid[6*i] = 0.1;
        rigid[6*i+1] = 0.2+(c-1)*x[i][1];
        rigid[6*i+2] = 0.3+s*x[i][1];
        rigid[6*i+3] = angle;
        for (std::size_t j = 0; j < 6; ++j) {
            deformed[6*i+j] = rigid[6*i+j] + 0.02*std::sin(double(6*i+j));
        }
    }
    for (double f : shell(rigid).force) {
        near(f, 0, 1e-10);
    }
    tangent_check(shell, deformed, 1e4);
}

using BeamLaw = decltype(&beam2_finite);
using TipHistory = std::array<std::array<double, 3>, steps>;

TipHistory cantilever(BeamLaw law, int elements) {
    constexpr double length = 2, e = 1e7, nu = 0.3, area = 0.01, inertia = 1e-6;
    constexpr double ea = e*area, ei = e*inertia, kga = (5.0/6)*ea/(2*(1+nu));
    constexpr double force = 10;
    Model model(3*(elements+1));
    for (int i = 0; i < elements; ++i) {
        const std::size_t a = 3*i;
        model.add_element({a,a+1,a+2,a+3,a+4,a+5}, [=](const Vector& u) {
            return law({length/elements, 0}, ea, kga, ei, u);
        });
    }
    for (auto i : {0, 1, 2}) {
        model.prescribe(i);
    }
    Vector u(3*(elements+1)), load(u.size());
    TipHistory history{};
    const auto tip = u.size()-3;
    for (int step = 1; step <= steps; ++step) {
        load[tip+1] = force*step/steps;
        const auto reaction = model.solve(u, load);
        near(reaction[1], -load[tip+1], 1e-7);
        near(reaction[2], -(length+u[tip])*load[tip+1], 1e-7);
        history[step-1] = {u[tip], u[tip+1], u[tip+2]};
    }
    if (!(u[tip] < -0.1 && u[tip+1] > 0.5)) {
        throw std::runtime_error("Cantilever did not reach the large-displacement regime");
    }
    return history;
}

void compare_beams() {
    const auto explicit_history = cantilever(beam2_finite, beam_elements);
    const auto ad_history = cantilever(beam2_finite_ad, beam_elements);
    const auto refined = cantilever(beam2_finite, 2*beam_elements).back();
    for (std::size_t i = 0; i < explicit_history.size(); ++i) {
        const auto& [x, y, angle] = explicit_history[i];
        const auto& [ad_x, ad_y, ad_angle] = ad_history[i];
        near(x, ad_x, 1e-10);
        near(y, ad_y, 1e-10);
        near(angle, ad_angle, 1e-10);
        std::cout << "BEAM " << i+1 << ' ' << x << ' ' << y << ' ' << angle << '\n';
        std::cout << "BEAM_AD " << i+1 << ' ' << ad_x << ' ' << ad_y << ' ' << ad_angle << '\n';
    }
    std::cout << "BEAM_REFINED " << refined[0] << ' ' << refined[1] << ' ' << refined[2] << '\n';
}

Model spatial_beam_chain(int count, double length, const Beam3Section& section) {
    Model model(6*(count+1));
    for (int i = 0; i < count; ++i) {
        const std::size_t a = 6*i;
        model.add_element({a,a+1,a+2,a+3,a+4,a+5,a+6,a+7,a+8,a+9,a+10,a+11},
                          [=](const Vector& u) {
                              return beam3_simo({length/count,0,0}, {0,1,0}, section, u);
                          });
    }
    for (std::size_t i = 0; i < 6; ++i) {
        model.prescribe(i);
    }
    return model;
}

void spatial_beam_checks() {
    constexpr Beam3Section section{{1000,400,350}, {17,23,10}};
    const Point3 chord{0.8,0.6,0}, transverse{0,1,0};
    const ElementLaw law = [&](const Vector& u) { return beam3_simo(chord, transverse, section, u); };
    tangent_check(law, {0.1,-0.2,0.1,0.13,0.36,-0.21, 0.4,0.1,-0.3,-0.36,0.21,0.5}, 1000);

    constexpr std::array<std::size_t, 6> planar{0,1,5,6,7,11};
    const Vector q{0.1,0.02,0.4,-0.2,0.7,0.9};
    Vector spatial(12);
    for (std::size_t i = 0; i < planar.size(); ++i) {
        spatial[planar[i]] = q[i];
    }
    const auto reference = beam2_finite({0.8,0.6}, 1000,400,10,q);
    const auto response = law(spatial);
    for (std::size_t i = 0; i < planar.size(); ++i) {
        near(response.force[planar[i]], reference.force[i], 1e-10);
        for (std::size_t j = 0; j < planar.size(); ++j) {
            near(response.tangent[planar[i]][planar[j]], reference.tangent[i][j], 1e-10);
        }
    }

    // Finite rigid rotation about global y, plus a translation.
    const double angle = 1.2;
    const Vector rigid{0.2,-0.1,0.3,0,angle,0,
                       0.2+(std::cos(angle)-1)*chord[0],-0.1,
                       0.3-std::sin(angle)*chord[0],0,angle,0};
    for (double force : law(rigid).force) {
        near(force, 0, 1e-10);
    }
}

void spatial_planar_limit() {
    constexpr double ea = 1e5, kga = (5.0/6)*ea/(2*(1+0.3));
    const auto planar = cantilever(beam2_finite, beam_elements);
    auto model = spatial_beam_chain(beam_elements, 2, {{ea,kga,kga}, {8,12,10}});
    Vector u(6*(beam_elements+1)), load(u.size());
    const auto tip = u.size()-6;
    double max_error = 0;
    for (int step = 1; step <= steps; ++step) {
        load[tip+1] = 10.0*step/steps;
        const auto reaction = model.solve(u, load);
        near(reaction[1], -load[tip+1], 1e-7);
        near(reaction[5], -(2+u[tip])*load[tip+1], 1e-7);
        for (std::size_t i = 0; i < u.size(); i += 6) {
            near(u[i+2], 0, 1e-10);
            near(u[i+3], 0, 1e-10);
            near(u[i+4], 0, 1e-10);
        }
        const auto& [x,y,angle] = planar[step-1];
        max_error = std::max({max_error, std::abs(u[tip]-x),
                              std::abs(u[tip+1]-y), std::abs(u[tip+5]-angle)});
    }
    near(max_error, 0, 1e-9);
    std::cout << "BEAM3 planar tip " << u[tip] << ' ' << u[tip+1] << ' ' << u[tip+5]
              << "; maximum component error " << max_error << '\n';
}

void spatial_bending_torsion() {
    constexpr int count = 16;
    constexpr double length = 2, rigidity = 10;
    constexpr Point3 moment{4,6,-3};
    auto model = spatial_beam_chain(count, length, {{1e4,5e3,4e3}, {rigidity,rigidity,rigidity}});
    Vector u(6*(count+1)), load(u.size());
    const auto tip = u.size()-6;
    for (int step = 1; step <= steps; ++step) {
        for (std::size_t i = 0; i < 3; ++i) {
            load[tip+3+i] = moment[i]*step/steps;
        }
        // The couple and tip rotation are coaxial on this solution branch.
        const auto reaction = model.solve(u, load);
        for (std::size_t i = 0; i < 3; ++i) {
            near(reaction[i], 0, 1e-7);
            near(reaction[3+i], -load[tip+3+i], 1e-7);
        }
    }
    const double norm = std::hypot(moment[0],moment[1],moment[2]);
    const Point3 axis{moment[0]/norm,moment[1]/norm,moment[2]/norm};
    const double turn = norm*length/rigidity;
    const double ds = length/count;
    Point3 midpoint_sum{};
    for (int j = 0; j < count; ++j) {
        const double angle = turn*(j+0.5)/count;
        const double c = std::cos(angle), s = std::sin(angle);
        midpoint_sum[0] += ds*(c+(1-c)*axis[0]*axis[0]);
        midpoint_sum[1] += ds*(s*axis[2]+(1-c)*axis[1]*axis[0]);
        midpoint_sum[2] += ds*(-s*axis[1]+(1-c)*axis[2]*axis[0]);
    }
    const double c = std::sin(turn)/turn, s = (1-std::cos(turn))/turn;
    const Point3 helix{length*(c+(1-c)*axis[0]*axis[0]),
                      length*(s*axis[2]+(1-c)*axis[1]*axis[0]),
                      length*(-s*axis[1]+(1-c)*axis[2]*axis[0])};
    const Point3 current_tip{length+u[tip],u[tip+1],u[tip+2]};
    for (std::size_t i = 0; i < 3; ++i) {
        near(u[tip+3+i], length*moment[i]/rigidity, 1e-8);
        near(current_tip[i], midpoint_sum[i], 1e-8);
        near(current_tip[i], helix[i], 1e-3);
    }
    std::cout << "BEAM3 helix tip " << current_tip[0] << ' ' << current_tip[1]
              << ' ' << current_tip[2] << "; continuum "
              << helix[0] << ' ' << helix[1] << ' ' << helix[2] << '\n';
}

void spatial_end_moment() {
    constexpr Point3 moment{4,6,-3};
    const auto undeformed = spatial_moment_load({0,0,0}, moment);
    const Matrix expected{{0,1.5,3}, {-1.5,0,-2}, {-3,2,0}};
    for (std::size_t i = 0; i < 3; ++i) {
        near(undeformed.force[i], moment[i], 1e-14);
        for (std::size_t j = 0; j < 3; ++j) {
            near(undeformed.tangent[i][j], expected[i][j], 1e-14);
        }
    }

    constexpr int count = 12;
    auto model = spatial_beam_chain(count, 2, {{1e4,5e3,4e3}, {7,13,21}});
    Vector u(6*(count+1)), load(u.size());
    const auto tip = u.size()-6;
    const std::array<std::size_t, 3> tip_rotation{tip+3,tip+4,tip+5};
    std::vector<SpatialMoment> applied{{tip_rotation, {}}};
    for (int step = 1; step <= steps; ++step) {
        for (std::size_t i = 0; i < 3; ++i) {
            applied[0].moment[i] = moment[i]*step/steps;
        }
        const auto reaction = model.solve(u, load, applied);
        // At the clamp theta = 0, so the generalized reaction is the spatial one.
        for (std::size_t i = 0; i < 3; ++i) {
            near(reaction[i], 0, 1e-7);
            near(reaction[3+i], -applied[0].moment[i], 1e-7);
        }
    }
    const Point3 theta{u[tip+3],u[tip+4],u[tip+5]};
    const auto generalized = spatial_moment_load(theta, moment);
    double difference = 0.0;
    for (std::size_t i = 0; i < 3; ++i) {
        difference = std::max(difference, std::abs(generalized.force[i]-moment[i]));
    }
    if (!(difference > 0.25)) {
        throw std::runtime_error("Expected a non-coaxial tip rotation and moment");
    }

    const auto system = model.assemble(u, load, applied);
    const auto internal = model.assemble(u, load);
    double max_error = 0, load_skew = 0;
    constexpr double h = 1e-6;
    for (std::size_t j = 0; j < system.residual.size(); ++j) {
        auto plus = u, minus = u;
        plus[6+j] += h;
        minus[6+j] -= h;
        const auto rp = model.assemble(plus, load, applied).residual;
        const auto rm = model.assemble(minus, load, applied).residual;
        for (std::size_t i = 0; i < system.residual.size(); ++i) {
            const double fd = (rm[i]-rp[i])/(2*h);
            near(system.tangent.coeff(i,j), fd, 5e-6);
            max_error = std::max(max_error, std::abs(system.tangent.coeff(i,j)-fd));
            near(internal.tangent.coeff(i,j), internal.tangent.coeff(j,i), 1e-9);
            load_skew = std::max(load_skew, std::abs(system.tangent.coeff(i,j)-system.tangent.coeff(j,i)));
        }
    }
    if (!(load_skew > 0.25)) {
        throw std::runtime_error("Spatial moment tangent was unexpectedly symmetric");
    }
    std::cout << "BEAM3 spatial moment tip rotation " << theta[0] << ' ' << theta[1] << ' '
              << theta[2] << "; generalized moment " << generalized.force[0] << ' '
              << generalized.force[1] << ' ' << generalized.force[2]
              << "; FD max error " << max_error << "; tangent asymmetry " << load_skew << '\n';
}

void cylinder(int sectors) {
    constexpr double e = 1e6, nu = 0.25, thickness = 0.01, radius = 1, length = 1;
    constexpr double membrane = e*thickness/(1-nu*nu);
    constexpr double final_force_per_radian = 2816;
    const double half_angle = pi/sectors;
    const double c = std::cos(half_angle), s = std::sin(half_angle);
    const Quad3 nodes{{{radius*c,-radius*s,0}, {radius*c,radius*s,0},
                       {radius*c,radius*s,length}, {radius*c,-radius*s,length}}};
    const std::array<double, 4> sign{-1, 1, 1, -1};
    // Symmetry maps one radial displacement onto the translations at all four nodes.
    Vector radial_mode(24);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        radial_mode[6*i] = c;
        radial_mode[6*i+1] = sign[i]*s;
    }
    Model sector(1);
    sector.add_element({0}, [&](const Vector& radial) {
        Vector u(24);
        for (std::size_t i = 0; i < u.size(); ++i) {
            u[i] = radial_mode[i]*radial[0];
        }
        const auto spatial = shell_q4_finite(nodes, e, nu, thickness, u);
        Response response{{0}, {{0}}};
        for (std::size_t i = 0; i < u.size(); ++i) {
            response.force[0] += radial_mode[i]*spatial.force[i];
            for (std::size_t j = 0; j < u.size(); ++j) {
                response.tangent[0][0] += radial_mode[i]*spatial.tangent[i][j]*radial_mode[j];
            }
        }
        return response;
    });

    Vector u(1);
    for (int step = 1; step <= steps; ++step) {
        const double load = (2*half_angle)*final_force_per_radian*step/steps;
        sector.solve(u, {load});
        const double stretch = 1+u[0]/radius;
        const double expected = membrane*length*std::sin(half_angle)*(stretch*stretch-1)*stretch;
        near(load, expected, 1e-7);
        std::cout << "SHELL " << sectors << ' ' << step << ' ' << u[0] << '\n';
    }
    near(u[0], 0.2, 0.01/(sectors/8.0)/(sectors/8.0));
}

void shell_bending(bool record_nodes = false) {
    constexpr int panels = 8;
    constexpr double h = 1.0/panels, e = 1.2e7, thickness = 0.01;
    const Quad3 nodes{{{0,0,0}, {h,0,0}, {h,1,0}, {0,1,0}}};
    // Cylindrical bending: the two nodes on a cross section share (ux, uz, ry).
    constexpr std::array<std::size_t, 12> panel_dof{0,2,4,6,8,10,12,14,16,18,20,22};
    constexpr std::array<std::size_t, 12> section_dof{0,1,2,3,4,5,3,4,5,0,1,2};
    Model strip(3*(panels+1));
    for (int i = 0; i < panels; ++i) {
        const std::size_t a = 3*i;
        strip.add_element({a,a+1,a+2,a+3,a+4,a+5}, [&](const Vector& reduced) {
            Vector u(24);
            for (std::size_t j = 0; j < panel_dof.size(); ++j) {
                u[panel_dof[j]] = reduced[section_dof[j]];
            }
            const auto panel = shell_q4_finite(nodes, e, 0, thickness, u);
            Response response{Vector(6), Matrix(6, Vector(6))};
            for (std::size_t j = 0; j < panel_dof.size(); ++j) {
                response.force[section_dof[j]] += panel.force[panel_dof[j]];
                for (std::size_t k = 0; k < panel_dof.size(); ++k) {
                    response.tangent[section_dof[j]][section_dof[k]] +=
                        panel.tangent[panel_dof[j]][panel_dof[k]];
                }
            }
            return response;
        });
    }
    for (auto i : {0, 1, 2}) {
        strip.prescribe(i);
    }
    Vector u(3*(panels+1)), load(u.size());
    for (int step = record_nodes ? 0 : 1; step <= steps; ++step) {
        load.back() = double(step)/steps;
        if (step != 0) {
            strip.solve(u, load, 1e-9);
        }
        if (record_nodes) {
            for (int i = 0; i <= panels; ++i) {
                std::cout << "CURL " << step << ' ' << load.back() << ' ' << i << ' '
                          << i*h+u[3*i] << ' ' << u[3*i+1] << ' ' << u[3*i+2] << '\n';
            }
        }
    }
    // A unit moment on the unit-length strip with unit flexural rigidity
    // gives a circular arc of radius one in the thin, inextensible limit.
    near(u[u.size()-3], std::sin(1.0)-1.0, 0.004);
    near(u[u.size()-2], std::cos(1.0)-1.0, 0.004);
    near(u.back(), 1.0, 0.004);
    std::cout << "BENDING " << u[u.size()-3] << ' ' << u[u.size()-2]
              << ' ' << u.back() << '\n';
}
}

int main(int argc, char** argv) {
    try {
        const std::string_view selected = argc > 1 ? argv[1] : "all";
        if (argc > 2 || (selected != "all" && selected != "beam2_finite" && selected != "beam3_simo" &&
                         selected != "beam3_spatial_moment" && selected != "shell_q4_finite" &&
                         selected != "shell_plot")) {
            throw std::invalid_argument("Unknown finite element benchmark argument");
        }
        std::cout << std::setprecision(16);
        if (selected == "all" || selected == "beam2_finite") {
            beam_tangent();
            compare_beams();
        }
        if (selected == "all" || selected == "beam3_simo") {
            spatial_beam_checks();
            spatial_planar_limit();
            spatial_bending_torsion();
        }
        if (selected == "all" || selected == "beam3_spatial_moment") {
            spatial_end_moment();
        }
        if (selected == "all" || selected == "shell_q4_finite" || selected == "shell_plot") {
            shell_tangent();
            for (int n : {8, 16, 32, 64}) {
                cylinder(n);
            }
            shell_bending(selected == "shell_plot");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
