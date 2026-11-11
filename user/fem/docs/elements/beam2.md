# 两节点平面梁柱

`beam2` 和 `beam2_finite` 都位于 [elements/frame/beam2.cpp](../../elements/frame/beam2.cpp)。
各节点按 $(u_x,u_y,r_z)$ 输入，初始弦向量确定局部轴线。

- `beam2(chord, EA, EI)` 采用小位移 Euler–Bernoulli 梁。轴向位移线性插值，
  横向位移用三次 Hermite 插值，由曲率构造弯曲刚度。两点 Gauss 积分得到局部刚度，
  初始轴线方向将局部平移投影到全局自由度。
- `beam2_finite(chord, EA, kGA, EI, u)` 采用平移和转角均线性插值的有限转动可剪切梁，
  中点处计算轴向、剪切和弯曲应变，按 FEAPpv 的截面力和几何项显式组装内力及一致切线。
  `beam2_finite_ad` 通过同一离散能量的梯度和 Hessian 提供独立对照。
  推导见[梁壳几何非线性](../finite_kinematics.md)。

`beam2` 测试对竖直悬臂同时施加轴力和横向力，核对末端位移、转角及支座反力。
`beam2_finite` 测试核对刚体运动和切线差分，并用 16 个单元、20 个载荷步求解大位移悬臂。
同一算例可用 [FEAPpv 对照脚本](../../tests/compare_feappv.py) 与原版有限变形梁及 Simo–Reissner 平面连续体参考解比较。
