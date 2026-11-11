# FEAPpv C++ 迁移

C++17 静力有限元库。项目按 FEAPpv 的目录职责划分，将单元、本构响应、全局组装和可执行入口分开。当前实现覆盖轴杆、二维桁架、平面梁、二维连续体、板和空间平壳。[FEAPpv 原版源码](https://github.com/sanjayg0/feappv/tree/e6897950aaca6166f4893bbe21d644c7af6150d7)

## 项目结构

| 目录 | C++ 职责 | FEAPpv 中的对应位置 |
| --- | --- | --- |
| [elements/solid1d](elements/solid1d) | 一维线弹性轴杆 | `elements/solid1d` |
| [elements/frame](elements/frame) | 桁架内力和切线、平面梁与三维几何精确梁 | `elements/frame` |
| [elements/solid2d](elements/solid2d) | T3 与 Q4 平面应力、平面应变 | `elements/solid2d` |
| [elements/shells](elements/shells) | MITC 板、线性和有限变形平壳 | `elements/shells` |
| [elements/common](elements/common) | 参考几何、积分和能量求导 | 各单元族共用的计算 |
| [program](program) | 单元组装、边界约束、反力、Newton 和稀疏 LU 求解 | `program/pform.f`、`pmacr1.f`、`psolve.f` |
| [include/feappv](include/feappv) | C++ 公共类型与接口 | `include/` 对应的接口层 |
| [main](main) | 三次弹性杆与大位移两杆演示程序 | `main/` 中的入口职责 |
| [tests](tests) | 独立单元测试和原版数值对照 | C++ 验证层 |

调用顺序为 `main/tests → Model → 单元回调 → 内力与切线`。
`Model` 从全局位移提取局部自由度，并把单元结果装配到自由自由度方程中。
`program/newton.hpp` 负责残差判定和载荷步收敛，线性求解独立于单元。
线性单元返回 $K$，由 `linear(K)` 产生 $(Ku,K)$；非线性单元直接返回
`Response{force, tangent}`。同一模型可以注册两类单元。
原来的 `fem/model.hpp` 和 `fem/elements.hpp` 保留为转发头文件。

## 单元文档与测试

| 单元 | 独立 CTest 名称 | 验证 |
| --- | --- | --- |
| [一维轴杆](docs/elements/bar1.md) | `bar1` | 给定位移、端力和支座反力 |
| [二维桁架](docs/elements/truss2.md) | `truss2` | 有限伸长及端部平衡 |
| [平面梁](docs/elements/beam2.md) | `beam2`、`beam2_finite` | 线性悬臂解析解、有限转动悬臂和切线 |
| [三维 Simo–Vu-Quoc 梁](docs/elements/beam3.md) | `beam3_simo`、`beam3_spatial_moment` | 平面退化、空间弯扭、非共轴端力矩与一致总切线 |
| [二维连续体](docs/elements/solid2d.md) | `solid_t3`、`solid_q4` | 各自的平面应力及平面应变补丁试验 |
| [四节点板](docs/elements/plate_q4.md) | `plate_q4` | 纯弯曲挠度、转角与反力矩 |
| [四节点空间壳](docs/elements/shell_q4.md) | `shell_q4`、`shell_q4_finite` | 倾斜面片、刚体模态、扩径、卷曲及切线 |

[梁壳几何非线性推导和原版对照](docs/finite_kinematics.md)记录了内力、一致切线、原版版本和比较数据。

## 构建与运行

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
ctest --test-dir build -R '^shell_q4_finite$' --output-on-failure
```

CMake 目标 `feappv_elements` 提供库接口。两个演示程序的目标名称为
`bar_demo`、`truss_demo`。如需在 CTest 中加入 Fortran 原版对照，可在配置时
设置 `-DFEAPPV_REFERENCE_EXECUTABLE=/path/to/feappv`，然后运行
`ctest --test-dir build -R '^feappv_reference$' --output-on-failure`。

## C++ 建模示例

按单元约定顺序传入全局自由度编号。

```cpp
#include <feappv/model.hpp>

fem::Model model(2);
model.add_element({0, 1}, fem::linear(fem::bar1(2.0, 1000.0)));
model.prescribe(0);
fem::Vector displacement(2);
const auto imbalance = model.solve(displacement, {0.0, 5.0});
// displacement[1] == 0.01, imbalance[0] == -5.0
```

`solve` 接收当前载荷步的总节点载荷，使用传入位移作为初值。
返回全自由度内力减外力，约束处为支座反力。非线性分析在每个收敛步后用上一步位移作为初值。

## 稀疏装配与 OpenMP

全局矩阵使用 Eigen 的稀疏列存储和 SparseLU，单元仍返回小型稠密矩阵。
CMake 需要可找到的 Eigen 配置包。编译器支持 OpenMP 时，可以用
`Model::set_element_threads(n)` 开启并行单元计算，回调须支持并发调用。
默认用一个线程。全局贡献按单元编号顺序合并，空间力矩切线仍按原符号装配。

~~~sh
cmake -S . -B build-omp -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++
cmake --build build-omp -j
ctest --test-dir build-omp -R '^shell_parallel$' --output-on-failure
build-omp/shell_parallel_benchmark --benchmark | awk '/^SHELL_/'
~~~

在本机 Homebrew 环境可将 `g++` 换为 `g++-16`。OpenMP 构建才生成并行基准。
基准输出单元计算、串行装配、稀疏线性求解与总墙钟时间，单位为秒。

当前范围为静力分析。完整迁移仍包括 FEAP 输入命令、材料历史变量、
几何非线性连续体、三维实体和动力学。

## MPI/PETSc 后端

配置时设置 `-DFEAPPV_ENABLE_PETSC=ON`，需要同一 MPI 发行版的编译及运行库、
实数双精度 PETSc 和可找到 `PETSc.pc` 的 pkg-config 环境。模型、初始位移与载荷
在所有进程上一致。在 `PetscInitialize` 后集体调用 `Model::solve_petsc()`，
并在返回后再结束 PETSc 会话。矩阵、残差与 KSP 增量按行分布，
试算位移使用同一行所有权和按本地单元依赖建立的 ghost 自由度。
每次残差计算前只更新这些远程自由度，收敛后才汇总全量位移和反力。

~~~sh
cmake -S . -B build-mpi -DCMAKE_BUILD_TYPE=Release -DFEAPPV_ENABLE_PETSC=ON
cmake --build build-mpi -j
ctest --test-dir build-mpi -R '^shell_mpi_' --output-on-failure
mpiexec -n 4 build-mpi/shell_mpi_benchmark -shell_benchmark \
  -pc_type asm -pc_asm_overlap 2 -sub_pc_type ilu -sub_pc_factor_levels 2
~~~

该基准沿用 OpenMP 的 MITC4 悬臂。其输出分别记录单元、本地装配、
显式通信和求解耗时。KSP 内部通信计入求解，显式通信计时也包含集合调用内的本地工作。
`MPI_TIMING` 单列 ghost 更新耗时，该时间已计入显式通信。
`MPI_GHOST` 给出所有进程的远程接收槽位总数。

KSP 对照沿用四个载荷步。`-shell_benchmark` 使用 16×8 网格并重复三次，
加上 `-shell_refined` 使用 32×16 网格。每组先用单进程 Eigen 解校验
四个载荷步的所有位移和反力分量。测试程序另外拦截常用 MPI 通信调用，
以测量 KSP 建立和迭代期间进入这些调用的时间，包含调用内的等待。
`MPI_TIMING` 在 ghost 更新耗时之后依次记录建立、迭代、建立阶段 MPI、迭代阶段 MPI，单位为秒。
后两列分别包含在前两列中。

以下 Bash 或 Zsh 循环用相同的 GMRES 容差分别测试独立 ASM/ILU 和以 ASM/ILU 为层内平滑器的 GAMG。
原网格采用重叠 2 层和 ILU(2)。加密网格采用重叠 4 层和 ILU(3)。

~~~sh
export OMP_NUM_THREADS=1 VECLIB_MAXIMUM_THREADS=1 OPENBLAS_NUM_THREADS=1
for mesh in coarse refined; do
  mesh_options=()
  overlap=2
  fill=2
  if [ "$mesh" = refined ]; then
    mesh_options=(-shell_refined)
    overlap=4
    fill=3
  fi
  for n in 1 2 4 8; do
    mpiexec -n "$n" build-mpi/shell_mpi_benchmark -shell_benchmark "${mesh_options[@]}" \
      -pc_type asm -pc_asm_overlap "$overlap" -sub_pc_type ilu -sub_pc_factor_levels "$fill"
    mpiexec -n "$n" build-mpi/shell_mpi_benchmark -shell_benchmark "${mesh_options[@]}" \
      -pc_type gamg -mg_levels_ksp_type richardson -mg_levels_ksp_max_it 1 \
      -mg_levels_pc_type asm -mg_levels_pc_asm_overlap "$overlap" \
      -mg_levels_sub_pc_type ilu -mg_levels_sub_pc_factor_levels "$fill"
  done
done
~~~

### GMRES 与流水线 GMRES

沿用上面的网格循环和独立 ASM/ILU 参数，在每个进程数下运行两次，
分别把以下参数追加到独立 ASM 命令末尾。两种算法使用相同的左预条件和重启长度。

~~~sh
-ksp_type gmres  -ksp_pc_side left -ksp_norm_type preconditioned -ksp_gmres_restart 200
-ksp_type pgmres -ksp_pc_side left -ksp_norm_type preconditioned -ksp_gmres_restart 200 -splitreduction_async true
~~~

在此前四个 KSP 分段时间字段之后，`MPI_TIMING` 追加迭代阶段的归约次数、
其中异步归约次数、归约调用时间、点对点操作数和点对点调用时间，
以及所有线性校正中最大真实相对、绝对残差。次数按所选进程统计。
异步归约和点对点请求的完成时间按请求类型归类，混合与未知请求只计入 MPI 总时间。
每次线性求解后显式重算 `R-K*delta`，这项校验在 KSP 分段计时之外，
计入端到端墙钟。需要逐迭代残差曲线时，可在单独的诊断运行中加 `-ksp_monitor_true_residual`。
