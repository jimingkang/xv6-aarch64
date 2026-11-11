#pragma once
#include "include/feappv/elements.hpp"
#include "second_order.hpp"
#include <stdexcept>

namespace fem::detail {
template <std::size_t N>
std::array<Jet<N>, N> variables(const Vector& u) {
    if (u.size() != N) {
        throw std::invalid_argument("Wrong finite element displacement size");
    }
    std::array<Jet<N>, N> q;
    for (std::size_t i = 0; i < N; ++i) {
        q[i] = Jet<N>::variable(u[i], i);
    }
    return q;
}

template <std::size_t N>
Response differentiate(const Jet<N>& energy) {
    Response response{Vector(energy.gradient.begin(), energy.gradient.end()), {}};
    for (const auto& row : energy.hessian) {
        response.tangent.emplace_back(row.begin(), row.end());
    }
    return response;
}
}
