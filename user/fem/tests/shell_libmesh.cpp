#include "shell_case.hpp"

#include <libmesh/boundary_info.h>
#include <libmesh/dirichlet_boundaries.h>
#include <libmesh/dof_map.h>
#include <libmesh/elem.h>
#include <libmesh/equation_systems.h>
#include <libmesh/explicit_system.h>
#include <libmesh/libmesh.h>
#include <libmesh/linear_partitioner.h>
#include <libmesh/replicated_mesh.h>
#include <libmesh/zero_function.h>
#include <petscsys.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
constexpr int nx = 16, ny = 8, steps = 4;
constexpr libMesh::subdomain_id_type shell_id = 1;
constexpr libMesh::boundary_id_type clamp_id = 11, load_id = 12;
constexpr double line_load = -0.5; // Force per unit reference length, in global z.
constexpr std::array<const char*, 6> fields{"ux", "uy", "uz", "rx", "ry", "rz"};

std::size_t reference_node(const libMesh::Point& point) {
    const auto i = std::lround(point(0)*nx/2.0);
    const auto j = std::lround(point(1)*ny);
    if (i < 0 || i > nx || j < 0 || j > ny ||
        std::abs(point(0)-2.0*i/nx) > 1e-8 || std::abs(point(1)-double(j)/ny) > 1e-8 ||
        std::abs(point(2)) > 1e-8) {
        throw std::runtime_error("Mesh node does not match the reference cantilever grid");
    }
    return j*(nx+1)+i;
}

void compare(const libMesh::Parallel::Communicator& comm, const std::string& filename) {
    libMesh::ReplicatedMesh mesh(comm);
    mesh.partitioner() = std::make_unique<libMesh::LinearPartitioner>();
    mesh.read(filename);
    if (mesh.mesh_dimension() != 2 || mesh.n_nodes() != (nx+1)*(ny+1) ||
        mesh.n_active_elem() != nx*ny) {
        throw std::runtime_error("Expected the 16 x 8 QUAD4 cantilever mesh");
    }
    const auto& boundary = mesh.get_boundary_info();
    if (boundary.get_sideset_name(clamp_id) != "CLAMP" || boundary.get_sideset_name(load_id) != "LOAD") {
        throw std::runtime_error("Expected CLAMP and LOAD physical curves");
    }

    libMesh::EquationSystems equations(mesh);
    auto& system = equations.add_system<libMesh::ExplicitSystem>("shell");
    std::vector<unsigned int> variables;
    for (auto name : fields) {
        variables.push_back(system.add_variable(name, libMesh::FIRST, libMesh::LAGRANGE));
    }
    auto& dof_map = system.get_dof_map();
    libMesh::ZeroFunction<libMesh::Number> zero;
    dof_map.add_dirichlet_boundary(libMesh::DirichletBoundary({clamp_id}, variables, zero));
    equations.init();

    const std::size_t count = system.n_dofs();
    if (count != mesh.n_nodes()*fields.size()) {
        throw std::runtime_error("Expected six nodal DOFs");
    }
    fem::Model imported(count);
    fem::Vector load(count);
    std::vector<std::size_t> reference_dof(count, count);
    std::vector<unsigned int> fixed(count);
    const auto dof = [&](const libMesh::Node& node, std::size_t field) {
        return node.dof_number(system.number(), variables[field], 0);
    };
    for (const auto* node : mesh.node_ptr_range()) {
        const auto ref = reference_node(*node);
        for (std::size_t k = 0; k < fields.size(); ++k) {
            const auto index = dof(*node, k);
            reference_dof.at(index) = fields.size()*ref+k;
            fixed[index] = dof_map.is_constrained_dof(index);
        }
    }
    // The existing Model expects the complete constraint mask on every rank.
    PetscCallMPIAbort(PETSC_COMM_WORLD, MPI_Allreduce(
        MPI_IN_PLACE, fixed.data(), static_cast<int>(fixed.size()), MPI_UNSIGNED, MPI_MAX,
        PETSC_COMM_WORLD));
    std::vector<bool> mapped(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto ref = reference_dof[index];
        if (ref >= count || mapped[ref] || bool(fixed[index]) != ((ref/fields.size())%(nx+1) == 0)) {
            throw std::runtime_error("Imported DOFs or CLAMP constraints differ from the reference");
        }
        mapped[ref] = true;
        if (fixed[index]) {
            imported.prescribe(index);
        }
    }

    std::size_t loaded_sides = 0;
    for (const auto* elem : mesh.active_element_ptr_range()) {
        if (elem->type() != libMesh::QUAD4 || elem->subdomain_id() != shell_id) {
            throw std::runtime_error("The MITC4 adapter requires QUAD4 elements in SHELL");
        }
        fem::Quad3 x{};
        fem::Dofs local;
        for (unsigned int a = 0; a < elem->n_nodes(); ++a) {
            const auto& node = *elem->node_ptr(a);
            x[a] = {node(0), node(1), node(2)};
            for (std::size_t k = 0; k < fields.size(); ++k) {
                local.push_back(dof(node, k));
            }
        }
        imported.add_element(std::move(local), [x](const fem::Vector& u) {
            return fem::shell_q4_finite(x, 1e6, 0.3, 0.04, u);
        });

        for (unsigned int side = 0; side < elem->n_sides(); ++side) {
            std::vector<libMesh::boundary_id_type> ids;
            boundary.boundary_ids(elem, side, ids);
            if (std::find(ids.begin(), ids.end(), load_id) == ids.end()) {
                continue;
            }
            const auto edge = elem->build_side_ptr(side);
            const double length = (edge->point(1)-edge->point(0)).norm();
            for (unsigned int a = 0; a < edge->n_nodes(); ++a) {
                load[dof(*edge->node_ptr(a), 2)] += line_load*length/2;
            }
            ++loaded_sides;
        }
    }

    auto reference = shell_test::cantilever(nx, ny);
    if (loaded_sides != ny) {
        throw std::runtime_error("LOAD does not contain all eight reference edge segments");
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (std::abs(load[index]-reference.load[reference_dof[index]]) > 1e-8) {
            throw std::runtime_error("Imported edge load differs from the reference");
        }
    }

    fem::Vector u(count), ref_u(count), step_load(count), ref_load(count);
    for (int step = 1; step <= steps; ++step) {
        const double fraction = double(step)/steps;
        for (std::size_t i = 0; i < count; ++i) {
            step_load[i] = fraction*load[i];
            ref_load[i] = fraction*reference.load[i];
        }
        const auto imbalance = imported.solve_petsc(u, step_load, {}, 1e-8);
        const auto ref_imbalance = reference.model.solve_petsc(ref_u, ref_load, {}, 1e-8);
        double u_error = 0, reaction_error = 0, tip = 0, support_z = 0, ref_support_z = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const auto ref = reference_dof[i];
            u_error = std::max(u_error, std::abs(u[i]-ref_u[ref]));
            if (fixed[i]) {
                reaction_error = std::max(reaction_error, std::abs(imbalance[i]-ref_imbalance[ref]));
                if (ref%fields.size() == 2) {
                    support_z += imbalance[i];
                    ref_support_z += ref_imbalance[ref];
                }
            }
            if (ref == reference.tip) {
                tip = u[i];
            }
        }
        if (comm.rank() == 0) {
            std::cout << std::setprecision(12) << "LIBMESH_CHECK " << comm.size() << ' ' << step << ' '
                      << tip << ' ' << ref_u[reference.tip] << ' ' << support_z << ' '
                      << ref_support_z << ' ' << u_error << ' ' << reaction_error << '\n';
        }
        if (!(u_error < 1e-8 && reaction_error < 1e-7 && std::abs(support_z-0.5*fraction) < 1e-7)) {
            throw std::runtime_error("Imported cantilever differs from the original PETSc case");
        }
    }
}
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: shell_libmesh <mesh.msh> [PETSc options]\n";
        return 1;
    }
    const std::string mesh_file = argv[1];
    libMesh::LibMeshInit init(argc, argv);
    try {
        compare(init.comm(), mesh_file);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        MPI_Abort(PETSC_COMM_WORLD, 1);
        return 1;
    }
}
