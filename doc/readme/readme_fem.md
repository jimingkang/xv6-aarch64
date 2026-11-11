# fem: a C++ finite-element code in xv6 user space

`user/fem` is the FEAPpv C++ port (bars, trusses, beams, 2D solids, plates
and shells, Newton solver, sparse LU, and an MPI/PETSc backend) built as xv6
programs. The sources are unchanged except for two lines (see below). They
need things xv6 did not have, so this directory set adds them:

| Directory | What it is |
| --- | --- |
| `user/cxx` | header-only subset of the C++17 standard library, plus a small runtime |
| `user/libm` | musl 1.2.5 libm (the functions fem and the C++ headers use) |
| `user/fmtfp.c` | `%e/%f/%g` formatting (`printf` and `std::ostream`) |
| `user/sparse` | sparse direct solver: reverse Cuthill-McKee plus band LU with partial pivoting |
| `user/eigen` | the part of Eigen that fem uses (`SparseMatrix`, `Triplet`, `SparseLU`, `VectorXd`, `Map`) |
| `user/petsc` | a small PETSc (`Vec`, `Mat`, `KSP`, `PC`) over the xv6 MPI subset |
| `user/mpi.c` | the MPI subset, now with the PMPI profiling layer and more calls |

## Programs

| xv6 program | fem source | How to run |
| --- | --- | --- |
| `bar_demo` | `main/bar.cpp` | `bar_demo` |
| `truss_demo` | `main/truss.cpp` | `truss_demo` |
| `element_benchmarks` | `tests/elements.cpp` | `element_benchmarks bar1` (also `truss2 beam2 solid_t3 solid_q4 plate_q4 shell_q4`) |
| `finite_element_benchmarks` | `tests/finite_elements.cpp` | `finite_element_benchmarks shell_q4_finite` (also `beam2_finite beam3_simo beam3_spatial_moment`) |
| `plate_locking_benchmark` | `tests/plate_locking.cpp` | `plate_locking_benchmark`, `plate_locking_benchmark --distorted` |
| `shell_mpi_benchmark` | `tests/shell_mpi.cpp` + `tests/shell_mpi_profile.cpp` | `mpirun -np 4 shell_mpi_benchmark` (`-shell_benchmark`, `-shell_refined` for larger meshes) |
| `bar_mpi_trace` | `tests/bar_mpi_trace.cpp` | `mpirun -np 2 bar_mpi_trace` (two-element bar, prints every rank's data, see below) |

`tests/shell_parallel.cpp` (OpenMP) is not built: there is no OpenMP
runtime. `Model::set_element_threads(n)` with `n > 1` therefore reports
that OpenMP is missing, as it does in a host build without OpenMP.

## Build

The Makefile compiles C++ with `$(TOOLPREFIX)g++`. No C++ library from the
toolchain is used (`-nostdinc -nostdinc++`, only libgcc), so the Homebrew
`aarch64-elf-gcc` built `--without-headers` works. Each program links
`libfem.a`, `libpetsc.a`, `libcxxrt.a`, `libm.a`, `libc.so` and libgcc as a
PIE for `ld-xv6.so`. Debug info is stripped from the programs and from
`libc.so`, because xv6fs files are limited to `MAXFILE` (268 KB).

Flags that matter:

* `-fno-exceptions -fno-rtti -fno-threadsafe-statics`: there is no unwinder
  or C++ ABI library.
* `-fPIE`: `ld-xv6.so` handles RELATIVE, ABS64, GLOB_DAT and JUMP_SLOT
  relocations only, so no copy relocations.
* `-include __xv6_cxx_config.h`: maps the exception keywords (next section).

## C++ runtime (`user/cxx`)

* **Start-up.** `ld-xv6.so` calls the entry as `entry(argc, argv)` and runs
  no constructors. C++ programs use `-e _xv6_cxx_start` (`cxxstart.c`). It
  moves to a 4 MB stack taken from the heap, because exec gives 16 KB
  (`USTACKPAGES`) and the second-order element code keeps far larger arrays
  on the stack. Then it runs `.init_array`, `main`, the `__cxa_atexit` list,
  and flushes the streams. The heap stack survives `fork`, so MPI ranks keep
  it.
* **Exceptions.** `throw E(...)` prints `exception: <what()>` and exits with
  status 1. In an MPI job it calls `MPI_Abort` instead, so no rank is left
  waiting. `try { }` runs its body, and `catch (T name)` blocks are compiled
  but never run. That matches fem, whose handlers only report and exit.
  `catch (...)` cannot be expressed with the macro, so the two
  `catch (...)` in `program/model.cpp` and `program/petsc_model.cpp` read
  `catch (const std::exception& xv6_error_)`. Those are the only edits to
  the fem sources. `std::exception_ptr` is always null.
* **Library.** `vector`, `array`, `string`, `string_view`, `algorithm`,
  `numeric`, `functional` (`std::function`, `hash`, comparators),
  `unordered_map`, `memory` (`unique_ptr`), `chrono` (`steady_clock` on
  `clock_us`), `cmath` (musl), `limits`, `type_traits`, `utility`, `iterator`,
  `stdexcept`, and `iostream`/`iomanip`/`sstream` (output only). `cout` is
  buffered. `cerr` is unbuffered and flushes `cout` first. `vector<bool>`
  stores one byte per element. `thread_local` is empty, which is fine
  because ranks are processes.

## Sparse solver and Eigen subset

`Eigen::SparseLU` in `user/eigen` factors with `xv6sparse::BandLU`
(`user/sparse/bandlu.hpp`): reverse Cuthill-McKee on the pattern of
`A + A^T`, then LAPACK-style band LU with partial pivoting (L bandwidth
`kl`, U bandwidth `kl + ku`). It is exact, but slower and more memory hungry
than Eigen's supernodal LU on large meshes. For the 32x16 shell it is about
3 300 unknowns with a band of about 100.

## PETSc subset (`user/petsc`)

The calls fem makes, plus their obvious neighbours:

* **Sys:** `PetscInitialize` (calls `MPI_Init`, which strips `-np N`),
  `PetscFinalize`, `PetscOptionsGetBool/Int/Real/String/HasName` (from
  `argv`), `PetscPrintf`, `PetscCall`, `PetscCallAbort`, `PetscCallMPIAbort`.
* **Vec:** `VecCreateMPI/Seq/Ghost`, `VecDuplicate`, `VecGetArray*`,
  `VecGetOwnershipRange`, `VecSet/Copy/Scale/AXPY/AYPX/WAXPY/Dot/Norm`,
  `VecSetValues` + assembly, `VecSetPreallocationCOO`/`VecSetValuesCOO`,
  `VecGhostUpdateBegin/End`, `VecGhostGetLocalForm`,
  `VecScatterCreateToAll/ToZero`.
* **Mat:** square parallel AIJ only. `MatCreate`, `MatSetSizes`,
  `MatSetType(MATAIJ)`, `MatSetPreallocationCOO`/`MatSetValuesCOO`
  (entries may belong to other ranks, and duplicates are summed),
  `MatCreateVecs`, `MatMult`, `MatGetDiagonal`.
* **KSP:** `KSPGMRES` (left preconditioned, classical Gram-Schmidt run twice,
  restart), `KSPCG`, `KSPRICHARDSON`, `KSPPREONLY`. It supports
  `KSPSetTolerances`, `KSPGMRESSetRestart`, `KSPSetFromOptions`
  (`-ksp_type -ksp_rtol -ksp_atol -ksp_max_it -ksp_gmres_restart
  -ksp_monitor -ksp_converged_reason`), and PETSc's convergence test and
  reason codes.
* **PC:** `PCASM` (restricted additive Schwarz with any overlap), `PCBJACOBI`
  (= ASM with overlap 0), `PCJACOBI`, `PCLU` (one rank), `PCNONE`
  (`-pc_type`, `-pc_asm_overlap`).

Differences from PETSc:

* ASM/BJACOBI subdomains are solved exactly with the band LU. PETSc uses
  ILU(0) by default. The default PC is BJACOBI (or LU on one rank).
* One communicator (`PETSC_COMM_WORLD`), real double scalars, 32-bit
  `PetscInt`, and no viewers beyond a plain `VecView`.
* Communication uses a `Plan`: each rank lists the entries it needs, the
  owners learn this once (`MPI_Alltoall` plus point-to-point), and then
  values move with `MPI_Isend`/`MPI_Irecv` between neighbours only.
  `VecGhostUpdateBegin` does all the work and `...End` is empty.

## MPI additions

* **Profiling layer.** Each function is defined as `PMPI_x`, and `MPI_x` is
  an alias. The library calls only `PMPI_x`. A program that defines its own
  `MPI_x` (as `tests/shell_mpi_profile.cpp` does) wraps every call it
  makes, and the wrapper reaches the library through `PMPI_x`. `mpi.h` has
  `extern "C"` guards.
* **New calls:** `Allgatherv`, `Gatherv`, `Alltoallv`; persistent requests
  (`Send_init`, `Recv_init`, `Start`, `Startall`, `Request_free`);
  `Waitsome`; `Issend`/`Ssend` (same as the buffered `Isend`/`Send`);
  `Iallreduce` and `Ibarrier`, which complete inside the call and return a
  finished request. `Ineighbor_alltoallv` returns `MPI_ERR_COMM`, because
  there are no topology communicators.

## Tests

All runs are in QEMU raspi3b (TCG). Every check that fem itself makes
passes, and the output matches a host build (x86-64, glibc, real Eigen)
line by line, except where the floating-point evaluation differs:

* `bar_demo`, `truss_demo`: same Newton histories and results. The truss
  finite-difference tangent check prints 3.1e-8 here and 5.6e-9 on the host
  (FMA contraction and musl versus glibc libm). It is within the
  program's limit.
* `element_benchmarks` (7 cases), `finite_element_benchmarks` (4 cases),
  `plate_locking_benchmark` (both meshes): all pass. Across the 370 result
  lines the largest relative difference from the host is 5.8e-11. Only
  Newton residuals at round-off level (1e-15 to 1e-10) and quantities that
  are themselves round-off (finite-difference errors, a "maximum component
  error" of 3e-16 here and 7e-12 there) differ more.
* `mpirun -np 1/2/4 shell_mpi_benchmark`: the PETSc solve matches the
  Eigen solve on rank 0 (`u_error` 4.5e-16, 5.0e-16, 6.4e-16 and
  `reaction_error` about 1e-11, against the program's limits of 1e-8 and
  1e-7). Ghost slots are 0, 24 and 72, and the tip deflection is -0.230292986591
  in every run. The wall time for the 4x2 mesh is 7.6 s, 4.2 s and 2.3 s.
* `mpitest -np 4` still passes after the PMPI rename.
* `mpirun -np 2 bar_mpi_trace`: two bar elements (EA/L = 2 and 3), node 0
  fixed, unit force at node 2, one element per rank. Without options it adds
  `-fem_trace -ksp_trace -pc_type none -ksp_converged_reason`.
  `-fem_trace` (program/petsc_model.cpp) prints, rank by rank, the COO
  `rows`/`columns`/`matrix_values`, the owned rows of K after
  `MatSetValuesCOO`, ghosts and state slots, `rhs_rows`/`rhs_values`, the
  ghosted state of each Newton evaluation, the KSP correction and the
  reaction reduction. `-ksp_trace` (user/petsc) prints each GMRES step: the
  basis vector, the off-process entries the matrix-vector product received,
  the local dot products of both Gram-Schmidt passes with their reduced
  values, and the true residual with its local and reduced squares. Result
  u = (0, 1/2, 5/6), reaction -1, as by hand; the numbers match a host run
  with real PETSc 3.25.4 except that the final true residual is 5.6e-16
  here (0 there). Full log: `doc/readme/bar_mpi_trace_xv6.log`.
