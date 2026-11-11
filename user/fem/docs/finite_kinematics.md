# 梁与壳的几何非线性

## 内力与一致切线

记单元自由度为 $q$，离散应变为 $e(q)$，常量弹性矩阵为 $C$，参考积分权重为 $w$。每个积分点的势能为 $w e^T C e/2$。令 $B=\partial e/\partial q$，$s=Ce$，则

```text
f_int = Σ w Bᵀ s
K_t   = Σ w [Bᵀ C B + Σ_a s_a ∂²e_a/∂q²]
```

第一项是材料切线，第二项来自应变随构形的二阶变化。梁的默认实现显式计算内力和切线，并保留二阶自动微分版本作为对照。有限变形壳用二阶自动微分对离散势能取梯度和 Hessian，剪切插值和稳定项均参与求导。中心差分用于验证。模型沿用 $R=f_{ext}-f_{int}$、$K_t\Delta q=R$ 和上一收敛载荷步作为初值的 Newton 循环。这里的外载是固定广义节点载荷。

## 两节点平面梁

在初始梁局部坐标中，令 $g=(1+\Delta u/L,\Delta v/L)$ 是当前轴线的导数，
$\theta_m=(\theta_1+\theta_2)/2$。截面轴 $a=(\cos\theta_m,\sin\theta_m)$，
横向轴 $b=(-\sin\theta_m,\cos\theta_m)$。三项应变为

```text
ε = a·g − 1           γ = b·g           κ = (θ₂ − θ₁)/L
U = (L/2)(EA ε² + kGA γ² + EI κ²)
```

平移和转角均线性插值，使用一个中点积分。应变的一阶与二阶方向变化如下，
`δ` 和 `Δ` 为独立方向，$g$、$\theta_m$ 和 $\kappa$ 本身均是自由度的仿射函数。

```text
δε = a·δg + (b·g) δθm
δγ = b·δg − (a·g) δθm
δκ = (δθ₂ − δθ₁)/L

Δδε = (b·Δg) δθm + (b·δg) Δθm − (a·g) Δθm δθm
Δδγ = −(a·Δg) δθm − (a·δg) Δθm − (b·g) Δθm δθm
Δδκ = 0
```

这对应 FEAPpv `frame2d → framf2d → b2modl` 的弹性、可剪切有限变形分支。
初始方向可任意，输入位移和转角均使用全局自由度顺序 `(ux, uy, rz)`。
[FEAPpv frame2d](https://github.com/sanjayg0/feappv/blob/e6897950aaca6166f4893bbe21d644c7af6150d7/elements/frame/frame2d.f)

### 对应 FEAPpv 的显式组装

取中点变量 $z=(g_x,g_y,\kappa,\psi)$，令 $H=\partial z/\partial q$。
因为节点插值和初始局部坐标变换均为线性映射，$H$ 为常量。
设 $\lambda=1+\varepsilon$、$\mathbf F=N\mathbf a+Q\mathbf b$，
其中 $N=EA\varepsilon$、$Q=kGA\gamma$、$M=EI\kappa$。与 $z$ 共轭的广义内力为

$$
p_z=(F_x,F_y,M,g_yF_x-g_xF_y)^T.
$$

$b_\varepsilon=(\cos\psi,\sin\psi,0,\gamma)^T$，
$b_\gamma=(-\sin\psi,\cos\psi,0,-\lambda)^T$。
中点切线 $A$ 的材料项为
$EA\,b_\varepsilon b_\varepsilon^T + kGA\,b_\gamma b_\gamma^T + EI\,e_3 e_3^T$，
其中 $e_3=(0,0,1,0)^T$。按从 1 开始的索引，几何项为

$$
A_{14}^{g}=A_{41}^{g}=-F_y,\qquad
A_{24}^{g}=A_{42}^{g}=F_x,\qquad
A_{44}^{g}=-\mathbf g\cdot\mathbf F.
$$

这三组项对应 `b2modl` 的 `gflag` 分支。最后按 `framf2d`
映射回全局节点自由度，$f_e=L H^T p_z$，$K_e=L H^T A H$。
C++ 返回正内力，模型组装时从外力中减去它。显式入口为 `beam2_finite`，
二阶自动微分入口为 `beam2_finite_ad`，两者采用相同的离散势能。
[显式与自动微分实现](../elements/frame/beam2.cpp)

### 与 Simo–Reissner 几何精确梁的关系

Simo 梁的连续运动学由中心线与独立的截面转动 $R\in SO(3)$ 描述。
对初始直梁，截面应变由 $R^T r'$ 减去参考轴向分量得到，曲率由 $R^T R'$ 提取。
将运动限制在平面、转动限制在固定法向后，这些应变退化为本节的
$(\varepsilon,\gamma,\kappa)$。三维梁还包含第二个横向剪切分量、面外弯曲及扭转，
并需要处理三维有限转动的组合。这里使用的是平面运动学的两节点、中点积分离散。
[原始有限应变梁理论](https://www.sciencedirect.com/science/article/abs/pii/0045782585900507) ·
[Simo 与 Vu-Quoc 的计算方法](https://www2.eecs.berkeley.edu/Pubs/TechRpts/1985/489.html)

同一悬臂的平面连续体参考解用射击法独立求得。竖向死载为 $P$ 时，
全局截面力为 $(0,P)$，故
$r'=\mathbf a(1+P\sin\theta/EA)+\mathbf b(P\cos\theta/kGA)$，
$\theta'=M/EI$，$M'=-P x'$。从自由端 $M(L)=0$ 反向积分，
调整自由端角度，使夹持端满足 $\theta(0)=0$。
将 RK4 积分由 256 步加密到 512 步核查参考解。
[C++ 与原版、连续体对照脚本](../tests/compare_feappv.py)

## 四节点空间平壳

每个节点有三个位移和三个旋转向量分量。参考中面单元共面，参考局部基底为
$(E_1,E_2,E_3)$。当前中面 $x=\sum_i N_i(X_i+u_i)$，
指向矢 $d=\sum_i N_iR(\theta_i)E_3$，其中 $R=\exp([\theta_i]_\times)$。
按参考平面坐标 $X,Y$ 取导数，令 $a_1=x_{,X},a_2=x_{,Y}$，
$d_1=d_{,X},d_2=d_{,Y}$。厚度坐标 $z$ 处的位置为 $x+zd$。
将面内 Green–Lagrange 应变保留到 $z$ 的一次项得到

```text
中面应变   e_m = ( (a₁·a₁−1)/2, (a₂·a₂−1)/2, a₁·a₂ )
曲率项     e_b = ( a₁·d₁, a₂·d₂, a₁·d₂ + a₂·d₁ )
横向剪切   e_s = ( a₁·d, a₂·d )
```

横向剪切先取自然坐标协变分量 $x_{,r}\cdot d,x_{,s}\cdot d$。
各分量分别在对应两条平行边的中点取样，线性插值到高斯点，再用参考 Jacobian
映射为 $X,Y$ 分量。令 $v_j=\sum_i N_iR(\theta_i)E_j$；
弱惩罚使用 $e_d=(a_2\cdot v_1-a_1\cdot v_2)/2$。四项刚度为

```text
C_m = t C_plane_stress              C_b = (t³/12) C_plane_stress
C_s = (5/6) Gt I₂                   C_d = 10⁻³ Gt
```

例如 $\delta(a_1\cdot d_1)=\delta a_1\cdot d_1+a_1\cdot\delta d_1$，
二阶变化还包括 $\delta a_1\cdot\Delta d_1+\Delta a_1\cdot\delta d_1+a_1\cdot\Delta\delta d_1$。
一般地，二阶导数对内积使用乘积法则，并对 Rodrigues 旋转中的三角函数使用链式法则。
因此切线包含构形变化、有限转角及稳定项的贡献。适用范围为平面参考面片、薄壳厚度近似、
线弹性和当前旋转向量参数化。旋转向量通过加性 Newton 更新，跨越参数化奇点的任意多圈运动超出本阶段范围。

## 数值对照

原版为 FEAPpv 5.1.2h，提交
`e6897950aaca6166f4893bbe21d644c7af6150d7`，用 GNU Fortran 编译。
脚本在临时目录生成两个 FEAP 输入文件，运行原版，读取长格式节点位移并比较全部 20 步。
[原版提交](https://github.com/sanjayg0/feappv/tree/e6897950aaca6166f4893bbe21d644c7af6150d7)

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
python3 tests/compare_feappv.py /path/to/original/feappv build/finite_element_benchmarks
```

- **悬臂梁。** 梁长 2 m，16 个单元，$EA=10^5$ N、$EI=10$ N·m²，
  末端施加 10 N 竖向固定方向力。C++ 末步
  $(u_x,u_y,\theta)=(-0.65730452277,1.33992084680,1.12201038519)$。
  全载荷历程三个分量的最大绝对差分别为 $4.82\times10^{-11}$ m、
  $4.76\times10^{-10}$ m、$3.40\times10^{-10}$ rad。
  显式 C++ 与自动微分 C++ 的全历程最大差分别为 $2.22\times10^{-16}$ m、
  $1.11\times10^{-15}$ m 和 $1.11\times10^{-16}$ rad。
  平面连续体参考末步为 $(-0.65797055867,1.34027138510,1.12116519639)$。
  与该参考相比，16 单元的末步绝对误差为 $(6.66\times10^{-4},3.51\times10^{-4},8.45\times10^{-4})$，
  32 单元时降为 $(1.67\times10^{-4},8.79\times10^{-5},2.11\times10^{-4})$。
- **薄圆筒扩径。** 半径 1 m、轴向长度 1 m、厚度 0.01 m，
  $E=10^6$ Pa、$\nu=0.25$，轴向伸长受限。最终均匀径向载荷为
  2816 N/rad，采用参考圆弧分配载荷。FEAPpv 的有限变形轴对称 `shell2d` 给出
  0.2 m。C++ 四节点平面片用循环对称约化成一个等效扇区，
  8、16、32、64 片时全历程最大位移差分别为
  $4.14\times10^{-3}$、$1.03\times10^{-3}$、$2.56\times10^{-4}$、
  $6.39\times10^{-5}$ m。此对照验证轴对称膜响应及面片几何收敛。
  FEAPpv 的空间四节点 `shell3d` 有限变形分支会报错，因此空间壳弯曲另外用解析解验证。
  [原版 shell2d](https://github.com/sanjayg0/feappv/blob/e6897950aaca6166f4893bbe21d644c7af6150d7/elements/shells/shell2d.f) ·
  [原版 shell3d](https://github.com/sanjayg0/feappv/blob/e6897950aaca6166f4893bbe21d644c7af6150d7/elements/shells/shell3d.f#L77-L90)
- **大转动薄壳弯曲。** 8 个面片、单位长和宽、厚度 0.01 m，
  夹持一端、自由端施加纯弯矩。按圆柱弯曲对称性约束每个截面，
  薄壳解析极限为半径 1 m 的圆弧。C++ 端部 $(u_x,u_z,\theta_y)$
  为 $(-0.15878,-0.46100,1.00264)$，解析极限为 $(-0.15853,-0.45970,1)$。
  此算例的绝对残差容限为 $10^{-9}$，以容纳薄壳膜刚度引起的浮点残差。
