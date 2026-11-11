cmake_minimum_required(VERSION 3.16)
project(feappv_cpp LANGUAGES CXX)
find_package(Eigen3 REQUIRED NO_MODULE)
find_package(OpenMP QUIET COMPONENTS CXX)
option(FEAPPV_ENABLE_PETSC "Build the MPI/PETSc backend" OFF)

add_library(feappv_elements
    elements/solid1d/bar1.cpp
    elements/frame/truss2.cpp
    elements/frame/beam2.cpp
    elements/frame/beam3.cpp
    elements/solid2d/continuum.cpp
    elements/shells/plate_q4.cpp
    elements/shells/shell_q4.cpp
    program/model.cpp
    program/spatial_moment.cpp
)
target_compile_features(feappv_elements PUBLIC cxx_std_17)
target_link_libraries(feappv_elements PUBLIC Eigen3::Eigen)
target_compile_definitions(feappv_elements PRIVATE EIGEN_DONT_PARALLELIZE)
if(OpenMP_CXX_FOUND)
    target_link_libraries(feappv_elements PRIVATE OpenMP::OpenMP_CXX)
endif()
if(FEAPPV_ENABLE_PETSC)
    find_package(MPI REQUIRED COMPONENTS CXX)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PETSC REQUIRED IMPORTED_TARGET PETSc)
    target_sources(feappv_elements PRIVATE program/petsc_model.cpp)
    target_compile_definitions(feappv_elements PUBLIC FEAPPV_HAS_PETSC)
    target_link_libraries(feappv_elements PRIVATE MPI::MPI_CXX PkgConfig::PETSC)
endif()
target_include_directories(feappv_elements PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_executable(bar_demo main/bar.cpp)
target_compile_features(bar_demo PRIVATE cxx_std_17)
add_executable(truss_demo main/truss.cpp)
target_compile_features(truss_demo PRIVATE cxx_std_17)

target_include_directories(bar_demo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(truss_demo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

include(CTest)
if(BUILD_TESTING)
    add_executable(feappv_plot_benchmarks EXCLUDE_FROM_ALL output/pdf/plot_benchmarks.cpp)
    target_link_libraries(feappv_plot_benchmarks PRIVATE feappv_elements)
    add_executable(element_benchmarks tests/elements.cpp)
    target_link_libraries(element_benchmarks PRIVATE feappv_elements)
    foreach(element IN ITEMS bar1 truss2 beam2 solid_t3 solid_q4 plate_q4 shell_q4)
        add_test(NAME ${element} COMMAND element_benchmarks ${element})
    endforeach()
    add_executable(plate_locking_benchmark tests/plate_locking.cpp)
    target_link_libraries(plate_locking_benchmark PRIVATE feappv_elements)
    add_test(NAME plate_locking COMMAND plate_locking_benchmark)
    add_test(NAME plate_locking_distorted COMMAND plate_locking_benchmark --distorted)
    if(OpenMP_CXX_FOUND)
        add_executable(shell_parallel_benchmark tests/shell_parallel.cpp)
        target_link_libraries(shell_parallel_benchmark PRIVATE feappv_elements OpenMP::OpenMP_CXX)
        add_test(NAME shell_parallel COMMAND shell_parallel_benchmark)
    endif()
    if(FEAPPV_ENABLE_PETSC)
        add_library(shell_mpi_profile SHARED tests/shell_mpi_profile.cpp)
        target_compile_features(shell_mpi_profile PRIVATE cxx_std_17)
        target_include_directories(shell_mpi_profile PRIVATE include)
        target_compile_definitions(shell_mpi_profile PRIVATE FEAPPV_HAS_PETSC)
        target_link_libraries(shell_mpi_profile PRIVATE Eigen3::Eigen MPI::MPI_CXX)
        add_executable(shell_mpi_benchmark tests/shell_mpi.cpp)
        target_link_libraries(shell_mpi_benchmark PRIVATE
            shell_mpi_profile feappv_elements MPI::MPI_CXX PkgConfig::PETSC)
        foreach(ranks IN ITEMS 1 2)
            add_test(NAME shell_mpi_${ranks}
                COMMAND "${MPIEXEC_EXECUTABLE}" "${MPIEXEC_NUMPROC_FLAG}" ${ranks}
                ${MPIEXEC_PREFLAGS} $<TARGET_FILE:shell_mpi_benchmark> ${MPIEXEC_POSTFLAGS})
        endforeach()
    endif()
    add_test(NAME bar_demo COMMAND bar_demo)
    add_test(NAME truss_demo COMMAND truss_demo)
    add_executable(finite_element_benchmarks tests/finite_elements.cpp)
    target_link_libraries(finite_element_benchmarks PRIVATE feappv_elements)
    foreach(element IN ITEMS beam2_finite beam3_simo beam3_spatial_moment shell_q4_finite)
        add_test(NAME ${element} COMMAND finite_element_benchmarks ${element})
    endforeach()

    set(FEAPPV_REFERENCE_EXECUTABLE "" CACHE FILEPATH "Original Fortran FEAPpv executable")
    if(FEAPPV_REFERENCE_EXECUTABLE)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME feappv_reference COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/compare_feappv.py"
            "${FEAPPV_REFERENCE_EXECUTABLE}" $<TARGET_FILE:finite_element_benchmarks>)
    endif()
endif()
