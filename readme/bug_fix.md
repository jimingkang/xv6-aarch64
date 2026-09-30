# Raspberry Pi 3 移植故障修复记录

## Pi 3B 启动停在 DWC2 USB HCD power on

### 现象

同一张 SD 卡和 xv6 镜像从原 Raspberry Pi 3B+ 换到一块 Raspberry Pi 3B 后，固件和
内核能够正常完成以下阶段：

```text
Loaded 'kernel8-xv6_wifi.img' to 0x80000
mini-uart: initialized

xv6 kernel is booting
build: bcm43430-sdio-v2

dwc2: firmware USB HCD power on
```

此后没有任何输出。固件日志表明新板使用：

```text
dtb_file 'bcm2710-rpi-3-b.dtb'
```

因此 Mini UART、EL1 入口、早期页表和普通 RAM 已经可用，故障范围可以缩小到 DWC2
USB Host 初始化。

HDMI EDID 错误和下面这条信息与内核停止无关：

```text
Cannot open /dev/tty.usbserial-0001!
```

前者只是没有连接 HDMI 显示器，后者来自 Mac 串口程序；它们发生后 Mini UART 仍能
继续输出 xv6 日志。

### 第一次排查：确认实际启动的内核

最初 SD 卡仍在启动旧内核。固件日志给出的文件大小为：

```text
Loaded 'kernel8-xv6_wifi.img' ... size 0x1e2f8
```

而当时工作区新内核为 `0x1e328`。安装后固件日志变为：

```text
Loaded 'kernel8-xv6_wifi.img' ... size 0x1e328
```

文件大小只能作为初步证据。后续两版内核碰巧具有相同大小，所以最终必须比较哈希：

```sh
shasum -a 256 \
  kernel/kernel8.img \
  /Volumes/bootfs/kernel8-xv6_wifi.img
```

两个 SHA-256 必须完全一致。修复当时生成的测试内核哈希为：

```text
808940d87d56abcb09d626020e983401fece52df140ce32c87f650450077ea50
```

该值只用于辨认这次构建；源代码再次变化后哈希自然会改变。

### 第二次排查：区分 power、delay 和 MMIO 访问

在 `dwc2_init()` 中加入分阶段输出：

```c
printf("dwc2: firmware USB HCD power on\n");
udelay(1000000);
printf("dwc2: probing core at pa=%p\n", V2P(DWC2_BASE));
id = rd(GSNPSID);
printf("dwc2: core id=%x\n", id);
```

新内核仍只打印第一行，没有打印 `probing core`。这证明 CPU 尚未执行 DWC2
`GSNPSID` 的 MMIO 读取，停止位置实际是 `udelay(1000000)`。

同时把 `trapinit()` 和 `trapinithart()` 提前到 `dwc2_driver_init()` 之前。这样以后若
`0x3f980000` 的 MMIO 访问产生同步异常，会打印 ESR、ELR 和 FAR，而不是因为 VBAR
尚未安装而静默停止。

### 根因

DWC2 原微秒延时使用 ARM Generic Timer：

```c
uint64 ticks = (r_cntfrq_el0() * us) / 1000000;
uint64 start = r_cntvct_el0();
while(r_cntvct_el0() - start < ticks)
  yield;
```

`CNTFRQ_EL0`/`CNTVCT_EL0` 的可用状态依赖固件和 armstub 建立的异常级、计时器访问
控制及频率信息。这个实现曾在原测试板工作，但换到使用另一固件/默认 armstub 路径的
Pi 3B 后，大延时没有结束。

不要重新启用项目中旧的极简 `armstub-xv6.bin`。该 stub 只有跳转逻辑，项目当前的
`entry.S` 已负责把 EL3/EL2 入口规范化到 EL1；旧 stub 还曾造成 property mailbox
不响应。正确做法是让外设驱动延时不依赖这组架构计时器状态。

### 修复

DWC2 的 `udelay()` 改用 BCM2837 System Timer 的 `CLO`：

```c
#define SYS_TIMER_CLO \
  (*(volatile uint32 *)(PERIPHERAL_BASE + 0x00003004UL))

static void
udelay(uint32 us)
{
  uint32 start = SYS_TIMER_CLO;
  while((uint32)(SYS_TIMER_CLO - start) < us)
    asm volatile("yield" ::: "memory");
}
```

BCM System Timer `CLO` 是 1 MHz、32 位自由运行计数器：计数增加 1 就是一微秒。使用
无符号减法可自然处理一次 32 位回绕；单次 USB 延时远小于其回绕周期。

这与后续用于进程调度 tick 的 ARM Generic Virtual Timer 是两条独立路径：

- DWC2 初始化延时：BCM System Timer `CLO` MMIO；
- xv6 调度和网络周期 tick：ARM Generic Virtual Timer。

### 更新与验证

代码变更涉及内核，必须更新 SD 卡上的内核镜像：

```sh
cd /Users/jimingkang/Documents/xv6-aarch64
make install-rpi3
sync
```

重新启动后，正常的下一阶段日志应为：

```text
dwc2: firmware USB HCD power on
dwc2: probing core at pa=0x000000003f980000
dwc2: core id=4f54....
dwc2: host hprt=...
```

若停在 `probing core` 之后并打印 `kerneltrap`，根据 ESR/FAR 排查 DWC2 MMIO 映射或
电源域；若打印 `DWC2 root port has no connection`，则控制器访问已成功，应继续排查
板载 USB Hub 的供电、复位和连接状态。
