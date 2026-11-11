#pragma once
#include "feappv/model.hpp"

namespace shell_test {
struct ShellCase {
    fem::Model model;
    fem::Vector load;
    std::size_t tip;
};

inline ShellCase cantilever(int nx, int ny) {
    constexpr double length = 2.0, width = 1.0;
    const auto node = [nx](int i, int j) { return std::size_t(j*(nx+1)+i); };
    const double hx = length/nx, hy = width/ny;
    const fem::Quad3 x{{{0,0,0}, {hx,0,0}, {hx,hy,0}, {0,hy,0}}};
    const std::size_t dofs = 6*std::size_t(nx+1)*(ny+1);
    ShellCase result{fem::Model(dofs), fem::Vector(dofs), 6*node(nx,ny/2)+2};
    for (int j = 0; j <= ny; ++j) {
        for (std::size_t k = 0; k < 6; ++k) {
            result.model.prescribe(6*node(0,j)+k);
        }
        result.load[6*node(nx,j)+2] = -0.5/ny * ((j == 0 || j == ny) ? 0.5 : 1.0);
    }
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            fem::Dofs local;
            for (auto a : {node(i,j),node(i+1,j),node(i+1,j+1),node(i,j+1)}) {
                for (std::size_t k = 0; k < 6; ++k) {
                    local.push_back(6*a+k);
                }
            }
            result.model.add_element(std::move(local), [x](const fem::Vector& u) {
                return fem::shell_q4_finite(x, 1e6, 0.3, 0.04, u);
            });
        }
    }
    return result;
}

}
