# 两节点一维轴杆

`bar1(L, EA)` 位于 [elements/solid1d/bar1.cpp](../../elements/solid1d/bar1.cpp)。
节点自由度依次为轴向位移 $[u_1,u_2]$。要求正的参考长度和轴向刚度。

小应变为 $(u_2-u_1)/L$，轴力为 $N=EA(u_2-u_1)/L$，内力为 $[-N,N]$。
切线为 $EA/L$ 乘以对角项为 1、非对角项为 −1 的二阶矩阵。
用 `linear` 将刚度矩阵包装成供模型使用的内力和切线回调。

`bar1` 测试采用 $L=2$、$EA=1000$、左端给定位移 0.1、右端力 5。
检查右端位移 0.11 和左端反力 −5。另有 `bar_demo` 用
[三次弹性轴杆](../../elements/solid1d/cubic_bar.hpp)演示分步加载。
该材料采用 $\sigma=E\varepsilon+\beta\varepsilon^3$，切线模量为 $E+3\beta\varepsilon^2$。
演示检查末步位移 0.4 m 和支座反力 −4 N。
