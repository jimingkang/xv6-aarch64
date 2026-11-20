# 单机单进程 MUMPS 实测

2026-11-19，在本机使用 1 个 MPI 进程；OMP、OpenBLAS、Accelerate 线程数均设为 1。未测试单机多进程或多线程 MUMPS。本次无需修改 FEM 求解代码，`program/petsc_model.cpp` 的 `KSPSetFromOptions` 已允许切换线性求解器。

## 配置与接口

现有 PETSc 未包含 MUMPS，因此在 `/tmp/fem-mumps/petsc` 隔离构建，未替换原安装。PETSc 版本头为 3.25.4、release=0，pkg-config 标识 3.25.99；MUMPS 5.9.1，Open MPI 5.0.9，Apple Clang C++，GNU Fortran 16，OpenBLAS、ScaLAPACK、METIS。

```bash
export PETSC_DIR=/tmp/fem-mumps/petsc PETSC_ARCH=arch-mumps
python3 ./configure --with-cc=mpicc --with-cxx=mpicxx --with-fc=mpifort \
  --with-debugging=0 --with-shared-libraries=1 \
  --with-blaslapack-dir=/opt/homebrew/opt/openblas \
  --with-scalapack-dir=/opt/homebrew/opt/scalapack \
  --with-metis-dir=/opt/homebrew/opt/metis --download-mumps=1
make all -j8
```

FEM 从项目目录构建；必须先完成 PETSc 构建，以免 CMake 找到其他安装的同名库。

```bash
PKG_CONFIG_PATH=/tmp/fem-mumps/petsc/arch-mumps/lib/pkgconfig \
cmake -S . -B /tmp/fem-mumps/fem-build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=mpicxx -DFEAPPV_ENABLE_PETSC=ON \
  -DFEAPPV_ENABLE_LIBMESH=OFF \
  -Dpkgcfg_lib_PETSC_petsc=/tmp/fem-mumps/petsc/arch-mumps/lib/libpetsc.dylib
cmake --build /tmp/fem-mumps/fem-build --target shell_mpi_benchmark -j6
bash output/mumps_single_node/run.sh
```

`/tmp` 构建是临时目录，清理后需要重新构建；日志和试验脚本保存在项目中。

```bash
mpiexec -n 1 /tmp/fem-mumps/fem-build/shell_mpi_benchmark \
  -shell_benchmark -ksp_type preonly -pc_type lu \
  -pc_factor_mat_solver_type mumps
```

以上参数使每次 Newton 线性系统 `K Δu = r` 使用 MUMPS 稀疏 LU。`preonly` 直接应用 LU 求解，不执行 GMRES 正交化。其 KSP 返回的每次求解“1 次迭代”不能与 GMRES 的 Krylov 迭代数直接比较。参考：[PETSc MUMPS 接口](https://petsc.org/release/manualpages/Mat/MATSOLVERMUMPS/)。

## 正确性

四单元杆：单元刚度系数为 2、3、4、5，左端固定、右端单位力，初始试算位移为 `(0,1/8,1/4,3/8,1/2)`。手算最终位移为 `(0,1/2,5/6,13/12,77/60)`。MUMPS 与手算最大位移差 `2.22045e-16`，支座反力 `-1`，线性真实残差二范数 `3.33067e-16`。`bar_mumps.log` 的 `KSPView` 确认 factorization package 为 mumps。校验代码见 `output/mumps_single_node/bar_smoke.cpp`。

壳为现有 `tests/shell_case.hpp` 中 MITC4 有限转动悬臂，长 2、宽 1、厚 .04，E=1e6，ν=.3，自由端总横向力 -.5，分 4 个载荷步。每种配置运行 3 次，与同一程序中的 Eigen 参考解比较每个载荷步的全部位移、反力分量。

| 网格 | 求解器 | 最大位移差 | 最大反力差 | 最终端点挠度 |
|---|---|---:|---:|---:|
|16×8，864 个自由未知量|MUMPS|3.797e-16|3.781e-12|−0.235362749000|
|16×8|GMRES/ASM|1.905e-13|7.024e-12|−0.235362749000|
|32×16，3264 个自由未知量|MUMPS|8.938e-15|1.943e-12|−0.235775722769|
|32×16|GMRES/ASM|1.179e-13|4.175e-12|−0.235775722769|

## 耗时

使用 `tests/shell_mpi.cpp` 既有统计：按完整总耗时选择三次运行的中位样本，以下各分项均来自该样本。单位为秒，统计涵盖所有载荷步和 Newton 迭代。GMRES 对照固定为 ASM 重叠 2 层、局部 ILU(2)、restart=200，使用同一新 PETSc 和 FEM 构建。

|网格|方法|单元计算|建立/分解|KSPSolve|线性合计|完整总耗时|
|---|---|---:|---:|---:|---:|---:|
|16×8|MUMPS|1.273126|0.049704|0.002068|0.051772|1.345380|
|16×8|GMRES/ASM|1.268878|0.033482|0.019965|0.053451|1.342593|
|32×16|MUMPS|5.124042|0.244713|0.007987|0.252706|5.460369|
|32×16|GMRES/ASM|5.103231|0.139199|0.339296|0.478502|5.675641|

MUMPS 建立阶段包括分析、数值分解及 PETSc 设置；没有把符号分析与数值分解再次分开。KSPSolve 对 MUMPS 主要是三角求解及可能的内部精化，对 GMRES 则包含整个迭代过程。原网格共有 19 次线性求解，GMRES 累计 454 步；加密网格共有 18 次线性求解，GMRES 累计 1537 步。MUMPS 分别执行 19、18 次直接求解。

加密网格线性阶段加速约 **1.89 倍**，完整总耗时加速约 **1.039 倍**，减少约 **3.8%**。原网格总时间差约 0.2%，不足以认定哪种方法更快。单元计算占总时间约 94%–95%，限制了单独更换线性求解器能带来的整体收益。

所有 Newton 线性求解中最大的真实残差二范数：原网格 MUMPS `2.301e-12`、GMRES `3.421e-11`；加密网格 MUMPS `5.006e-12`、GMRES `1.021e-10`。接近 Newton 收敛时右端项很小，最大相对残差可能受绝对停止阈值影响；日志中的相对数不能直接当作最终非线性误差。

这次结果支持把 MUMPS 用作本机正确性参考及中小规模问题的备选直接求解器；尚未测量峰值内存，不能外推到大网格的内存成本或单机多进程性能。

原始输出位于 `output/mumps_single_node/`，包括 `bar_mumps.log`、两个网格各两种求解器的日志和 `run.sh`。
