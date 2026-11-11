# 四节点 Reissner–Mindlin 板

`plate_q4(nodes, E, nu, t)` 位于 [elements/shells/plate_q4.cpp](../../elements/shells/plate_q4.cpp)。
平面节点按逆时针排列，每节点自由度为 $(w,r_x,r_y)$，转角遵循右手规则。
令 $\beta=(r_y,-r_x)$，工程剪切向量为 $\nabla w+\beta$，
曲率向量由 $\beta$ 的两项法向导数和交叉导数和组成。

弯曲刚度使用 $t^3 C_{ps}/12$，其中 $C_{ps}$ 为平面应力本构。
横向剪切刚度为 $(5/6)GtI_2$。MITC 在四条边的中点取协变剪切分量，
插值到积分点后映射到物理平面。弯曲和剪切能量均用 $2\times2$ Gauss 积分。

`plate_q4` 测试为单位方板夹持一边、对边施加均布端弯矩的圆柱纯弯曲。
核对自由边挠度、转角及约束端反力矩。FEAPpv 的 `plate2q` 采用带内部转角泡函数的混合剪切形式。

## 薄板锁死对照

同一个 Q4 接口可用 `PlateShear::full_integration` 选择高斯点直接剪切插值，
默认保留 MITC4。两种公式的弯曲和剪切能量均用四个高斯点积分。
[简支方板基准](../../tests/plate_locking.cpp)使用正弦面载荷，在五种厚度和五组网格上
对比中心挠度，并用 Mindlin 连续解检查网格加密。
运行 `ctest --test-dir build -R '^plate_locking$' --output-on-failure` 可验证，
运行 `build/plate_locking_benchmark` 可导出数据。Kirchhoff 误差曲线与完整推导见
[LaTeX 讲义](../../output/pdf/feappv_cpp_illustrated.tex)。

畸变复验用 `build/plate_locking_benchmark --distorted`，保留边界和中心节点，
只扰动其他内部节点。输出 `DIST` 挠度和最小归一化 Jacobian，以及 `MESH` 节点。
回归项为 `plate_locking_distorted`。第 8 节讲义给出厚度与网格收敛曲线。
