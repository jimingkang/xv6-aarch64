# 三维 Simo–Vu-Quoc 型几何精确梁

[beam3_simo](../../elements/frame/beam3.cpp) 是两节点、初始直梁、线弹性截面合力模型。
中心线与截面转动独立，保留轴伸、双向剪切、扭转和双向弯曲。
每节点自由度按 $(u_x,u_y,u_z,\theta_x,\theta_y,\theta_z)$ 排列，均在全局坐标下输入。
截面刚度在初始材料坐标下输入，顺序为 $(EA,k_2GA,k_3GA)$ 和 $(GJ,EI_2,EI_3)$。
初始弦向量给出第一材料轴。第二轴用输入的横向参考向量正交化确定。
离散采用两节点线性中心线、相对转动的指数插值和中点积分。
[连续理论与计算方法](https://www2.eecs.berkeley.edu/Pubs/TechRpts/1985/489.html)

## 转动插值与 Newton 更新

令 $R_0$ 的列为三个初始材料轴。节点截面朝向为
$R_i=\operatorname{Exp}([\theta_i]_\times)R_0$。
从两个节点取相对转动，令 $\phi=\operatorname{Log}(R_1^TR_2)^\vee$。
以参考弧长 $S\in[0,L]$ 为参数，有

$$
R(S)=R_1\operatorname{Exp}((S/L)[\phi]_\times),\qquad
R_m=R(L/2),\qquad \kappa=\phi/L.
$$

源码用单位四元数计算指数映射、相对旋转的主值对数，以及相对四元数的平方根。
平方根将截面推进到中点。小角度表达式对角度平方展开，以便在零转角处稳定求二阶导数。

现有 Newton 框架加性更新转动坐标 $\theta_i^+=\theta_i+\Delta\theta_i$，
随后重建 $R_i^+$。对实际朝向，这对应严格的乘法更新

$$
R_i^+ =
\left(\operatorname{Exp}([\theta_i+\Delta\theta_i]_\times)
       \operatorname{Exp}(-[\theta_i]_\times)\right)R_i.
$$

无穷小空间转动为 $\eta_i=J_l(\theta_i)\delta\theta_i$。对 $\alpha=\|\theta\|$，
左 Jacobian 为

$$
J_l(\theta)=I+\frac{1-\cos\alpha}{\alpha^2}[\theta]_\times
             +\frac{\alpha-\sin\alpha}{\alpha^3}[\theta]_\times^2.
$$

节点广义力与转动坐标变化功共轭。空间力矩的变换与线性化见下文。

相对转动使用短弧插值。节点间相对角接近 $\pi$ 时主值分支不唯一，
单元会报错。节点转动向量接近 $2\pi$ 的参数化奇点也会报错。
当前采用固定指数坐标图，跨越奇点的多圈转动需要重参数化。

## 内力与一致切线

中心线导数 $g=(x_2-x_1)/L$，中点拉伸和剪切向量为 $\Gamma=R_m^Tg-e_1$，
其中 $e_1=(1,0,0)^T$。取 $C_N=\operatorname{diag}(EA,k_2GA,k_3GA)$、
$C_M=\operatorname{diag}(GJ,EI_2,EI_3)$。中点截面合力为
$N=C_N\Gamma$、$M=C_M\kappa$。

给定一组节点坐标变化，先把相对转动变化从空间坐标转换到左截面的材料坐标，
再由左 Jacobian 线性化对数与中点指数映射：

$$
\begin{aligned}
\delta\phi &= J_l(\phi)^{-1}R_1^T(\eta_2-\eta_1),\\
\eta_m &= \eta_1+\tfrac12 R_1 J_l(\phi/2)\delta\phi,\\
\delta\Gamma &= R_m^T(\delta g-\eta_m\times g),\\
\delta\kappa &= \delta\phi/L .
\end{aligned}
$$

令 $B_\Gamma=\partial\Gamma/\partial q$，$B_\kappa=\partial\kappa/\partial q$。
由中点内虚功 $L(N^T\delta\Gamma+M^T\delta\kappa)$ 得

$$
f_e=L(B_\Gamma^T N+B_\kappa^T M).
$$

再次线性化，得到包含截面转动和几何贡献的一致切线：

$$
K_e=L\left[
B_\Gamma^TC_NB_\Gamma+B_\kappa^TC_MB_\kappa+
\sum_a N_a\nabla_q^2\Gamma_a+\sum_a M_a\nabla_q^2\kappa_a
\right].
$$

实现对同一个离散弹性能 $L(\Gamma^TC_N\Gamma+\kappa^TC_M\kappa)/2$
取梯度和 Hessian。自动微分以实际用于 Newton 更新的 12 个自由度为变量，
保留指数、对数与中点插值的二阶贡献。单元回调返回正内力和切线，
现有 Model 按外力减内力形成残差。

## 空间端力矩与一致载荷切线

令全局固定空间力矩为 $m$。其虚功 $m^T\eta=m^TJ_l(\theta)\delta\theta$，
因此节点广义外力为 $q_m=J_l(\theta)^Tm$。令 $t=\theta^T\theta$、
$a(t)=(1-\cos\sqrt t)/t$、$b(t)=(\sqrt t-\sin\sqrt t)/t^{3/2}$，
以及 $c=\theta\times m$、$d=\theta\times c$。有

$$
q_m=m-ac+bd,\qquad
Dq_m[v]=-2(\theta\cdot v)a'(t)c-a(v\times m)
 +2(\theta\cdot v)b'(t)d+b[v\times c+\theta\times(v\times m)] .
$$

源码显式计算各列。小角度使用同一泰勒多项式的值和导数，使
$\theta=0$ 时 $q_m=m$、$Dq_m=[m]_\times/2$。
沿用 $R=f_{\rm ext}-f_{\rm int}$，Newton 矩阵为
$K_{\rm tot}=K_{\rm int}-Dq_m=-\partial R/\partial q$。
载荷切线一般非对称，现有带部分主元的稠密求解器可以直接求解。
[左 Jacobian 参考](https://arxiv.org/abs/1812.01537)

每个载荷步传入该步的总空间力矩。Newton 在每次试算转角处更新广义力和切线。

~~~cpp
const std::vector<fem::SpatialMoment> moments{
    {{tip_rx, tip_ry, tip_rz}, {4.0, 6.0, -3.0}}
};
const auto reaction = model.solve(u, nodal_load, moments);
const auto system = model.assemble(u, nodal_load, moments);
~~~

反力仍按节点坐标的功共轭分量返回，零转动处与物理空间力矩相同。
随截面转动的材料轴力矩还需要另外线性化 $\delta m$。

## 验证

运行独立测试：

~~~sh
ctest --test-dir build -R '^beam3_simo$' --output-on-failure
~~~

- **单元级。** 在斜置且已变形的单元上，以中心差分逐项检查 12×12 切线。
  另检验有限刚体转动零内力，并比较斜置单元平面子块与显式二维梁的内力及切线。
- **平面悬臂。** 沿用长 2 m、16 单元、20 步和 10 N 竖向端力。
  三维模型只约束夹持端，检验自由节点面外自由度、支座反力与末端平面位移和转角。
  同轴平面转动时，相对对数给出两节点转角之差，中点给出两角平均，恢复二维梁的离散式。
- **空间弯扭。** 长 2 m 的梁施加端力矩 $(4,6,-3)$ N·m，三个转动刚度均为
  10 N·m²。端部转动向量为 $(0.8,1.2,-0.6)$ rad，夹持端反力矩与外力矩相反。
  端力为零，因此无拉伸和剪切。中心线切向绕固定力矩轴旋转，连续解为螺旋线。
  16 单元端点位置为 $(1.469464,-0.204743,-1.116867)$ m，
  连续螺旋线端点为 $(1.469088,-0.204349,-1.116580)$ m。
  数值结果也与该螺旋线切向量的离散中点积分逐项吻合。

- **非共轴空间端力矩。** 长 2 m、12 单元、20 步，转动刚度
  $(GJ,EI_2,EI_3)=(7,13,21)$ N·m²，施加全局端力矩 $(4,6,-3)$ N·m。
  末端转动向量为 $(1.263782,0.680291,-0.506327)$ rad，
  广义端力矩为 $(4.214031,4.288151,-4.765788)$ N·m。
  每步均检查夹持端空间反力矩与外力矩平衡，且平移反力为零。
  末步以 $h=10^{-6}$ 对全部 72 个自由度的装配残差做中心差分，
  与总切线的最大逐项差为 $2.76\times10^{-6}$，采用模型所用的广义坐标单位。
  独立 CTest 名称为 beam3_spatial_moment。
