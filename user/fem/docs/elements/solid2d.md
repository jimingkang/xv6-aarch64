# 二维连续体 T3 与 Q4

`solid_t3` 和 `solid_q4` 位于 [elements/solid2d/continuum.cpp](../../elements/solid2d/continuum.cpp)。
节点沿周边逆时针排列，各节点自由度为 $(u_x,u_y)$。两者均为小应变线弹性，
通过 `Plane::stress` 或 `Plane::strain` 选择本构。应变使用工程剪应变顺序
$(\varepsilon_{xx},\varepsilon_{yy},\gamma_{xy})$。

- **T3** 采用线性三角形形函数，单元内 $B$ 为常量，以面积乘厚度积分。
- **Q4** 采用双线性等参形函数，参考 Jacobian 把形函数导数映射到物理坐标，
  使用 $2\times2$ Gauss 积分。单元要求整个母单元内 Jacobian 行列式为正。

两者均计算 $K=\int B^T C B\,t\,dA$。平面应力令面外应力为零，
平面应变令面外应变为零。平面应变的位移型离散在近不可压缩时可能出现体积锁定。

`solid_t3` 和 `solid_q4` 分别在含偏移内部节点的多单元网格上执行常应变补丁测试，
各自覆盖两种平面本构。检查内部节点位移以及右边界的法向和切向合力。
