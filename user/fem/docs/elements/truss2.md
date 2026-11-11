# 两节点二维大位移桁架

`truss2(chord, EA, u)` 位于 [elements/frame/truss2.cpp](../../elements/frame/truss2.cpp)。
节点自由度为 $(u_x,u_y)$，`chord` 是初始杆向量。令 $D=[-I_2\; I_2]$，
$d=chord+Du$，$L=\|chord\|$，$g=D^T d$，以及 $\varepsilon=(d^Td/L^2-1)/2$。

```text
f_int = (EA/L) ε g
K_t   = (EA/L³) g gᵀ + (EA/L) ε DᵀD
```

该总拉格朗日单元采用线性第二 Piola–Kirchhoff 应力与 Green–Lagrange 应变关系。
杆长在刚体转动下保持不变，公式适用于任意初始杆方向。

`truss2` 测试用轴向力 264 将长 2、$EA=1000$ 的杆拉到伸长比 1.2，
校验位移 0.4 和反力 −264。`truss_demo` 另验证两杆平衡和一致切线。
