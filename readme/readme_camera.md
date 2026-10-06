# 树莓派摄像头驱动：OV5647 + Unicam CSI-2

硬件：Raspberry Pi 3B+，Camera Module v1.3（OmniVision OV5647），接在板载 CAMERA 排线口（CSI1）。

路线：由 ARM 直接驱动，不经过 GPU 固件的摄像头栈（`start_x.elf`、MMAL/VCHIQ）。
内核自己完成 I2C 配置传感器、开电源与时钟、用 Unicam 接收 MIPI CSI-2 数据并 DMA 到内存；
用户态 `camshot` 把原始 Bayer 数据转换成 BMP。这与 Linux 主线的
`bcm2835-unicam` + `ov5647` 驱动是同一种结构，本驱动的寄存器配置也都取自这两个 Linux 驱动。

当前状态：已经编译通过，并在 QEMU 上验证了“没有摄像头”时的流程；图像转换部分在主机上用
合成 RAW10 帧验证过。**真机采集还没有测试过**，第 10 节给出了按顺序排查的步骤。

## 0. 文件

| 文件 | 内容 | 对应的 Linux 代码 |
|---|---|---|
| `kernel/mbox.[ch]` | 新增。共享的固件 property mailbox（带锁）：电源域、扩展 GPIO、时钟频率 | `raspberrypi-firmware.c` |
| `kernel/i2c.[ch]` | 新增。BSC I2C 主机，轮询方式；BSC0 切到 GPIO 44/45 | `i2c-bcm2835.c` |
| `kernel/camsensor.h` | 新增。传感器接口：`struct camsensor`、`sensor_ops`、`cam_mode`、`camsensor_bus` | V4L2 subdev（`v4l2_subdev_ops`） |
| `kernel/ov5647.c` | 新增。`sensor_ops` 的一个实现：上电、读芯片 ID、640x480 RAW10 寄存器表、开关数据流、测试图 | `drivers/media/i2c/ov5647.c` |
| `kernel/unicam.c` | 新增。桥接驱动：CSI1 接收器、CAM1 时钟、帧状态机、中断、`/dev/video0`；只通过 `sensor_ops` 访问传感器 | `bcm2835-unicam.c` |
| `kernel/camera.h` | 新增。内核与用户程序共用的帧头格式和常量 | — |
| `user/camshot.c` | 新增。拍照并转换成 BMP | — |
| `kernel/memlayout.h`、`vm.c` | 摄像头 DMA 区 `CAMDMA`（非缓存映射）；`CSI1_IRQ` | — |
| `kernel/bcm2837.c`、`trap.c` | CSI1 中断的识别、分发、开关 | — |
| `kernel/file.h`、`user/init.c` | 主设备号 `CAMERA = 5`，`mknod /dev/video0` | — |
| `kernel/main.c` | 启动时先 `ov5647_init()` 注册传感器，再 `camera_init()` 探测；检查内核镜像没有越过 `EARLYTOP` | — |
| `Makefile` | 新目标文件和 `_camshot` | — |
| `user/user.h` | 补上 include guard（见第 12 节） | — |

## 1. 结构

```text
user:   camshot ── open/read("/dev/video0") ──┐
                                              │ struct cam_frame_hdr + RAW10
kernel: unicam.c  /dev/video0 读一次 = 拍一帧 ◄┘     （桥接驱动）
          ├─ mbox.c    电源域 UNICAM1
          ├─ CM_CAM1   CSI 的 "lp" 时钟 100 MHz
          ├─ sensor_ops ──► ov5647.c ── i2c.c ── BSC0 (GPIO 44/45) ── 传感器 0x36
          │   (camsensor.h)      └─ mbox.c：摄像头供电 GPIO
          └─ Unicam CSI1 ◄══ 2 lane MIPI CSI-2 ══ 传感器
                  │ DMA（总线地址 0xC0000000 | phys）
                  ▼
               CAMDMA：物理 0x07200000，非缓存映射
```

## 1.1 传感器接口：sensor_ops

`unicam.c` 不认识任何具体传感器，不写传感器寄存器，也不包含 `ov5647_*` 调用；
`ov5647.c` 也不碰 Unicam。两者之间只有 `kernel/camsensor.h` 这一层接口，
仿照 Linux V4L2 子设备，只保留 xv6 用得到的部分：

| Linux | 本工程 | 作用 |
|---|---|---|
| `struct v4l2_subdev` | `struct camsensor` | 一个传感器实例：名称、ops、模式表、总线参数、预热帧数、私有状态 |
| `core_ops.s_power` + runtime PM | `ops->power_on / power_off` | 供电、校验芯片 ID、进入 LP-11；断电 |
| `video_ops.s_stream` | `ops->stream_on(mode) / stream_off` | 按模式写寄存器并开始出流；停止 |
| `v4l2_ctrl_handler`（先缓存，`s_stream` 时由 `__v4l2_ctrl_handler_setup` 写入） | `ops->set_ctrl(id, val)`，下次 `stream_on` 时生效 | 目前只有 `CAM_CTRL_TEST_PATTERN` |
| `pad_ops.enum_frame_size` / `get_fmt` | `modes[]`：`struct cam_mode` | 宽、高、每行字节数、CSI-2 数据类型、Bayer 顺序（`CAM_FMT_*`）、传感器私有的寄存器表 |
| 设备树 endpoint：`data-lanes`、`clock-noncontinuous` | `struct camsensor_bus` | lane 数、是否非连续时钟、虚拟通道 |
| `v4l2_async_register_subdev` + notifier | `camsensor_register()` + `camera_init()` 依次探测 | 桥接驱动发现传感器 |

```c
struct sensor_ops {
  int  (*power_on)(struct camsensor *s);
  void (*power_off)(struct camsensor *s);
  int  (*stream_on)(struct camsensor *s, const struct cam_mode *mode);
  int  (*stream_off)(struct camsensor *s);
  int  (*set_ctrl)(struct camsensor *s, int id, int value);   // 可以为 0
};
```

桥接驱动从传感器拿到并用于配置 Unicam 的信息：

| 来自 | 字段 | 写到 Unicam 的哪里 |
|---|---|---|
| `bus.data_lanes` | 1 或 2 | lane 时钟门控 `0x7e802004`；`DAT1` 是否使能 |
| `bus.noncontinuous_clk` | 0/1 | `CLK`、`DATn` 是否加 `TRE`/`HSE`（连续时钟需要） |
| `bus.vc` | 0～3 | `IDI0[7:6]` |
| `mode->csi_dt` | 如 `0x2b` RAW10 | `IDI0[5:0]` |
| `mode->bytesperline` | 必须是 16 的倍数 | `IBLS` |
| `bytesperline × height` | 不超过 `CAM_MAX_FRAME_BYTES`（512 KB） | `IBEA0`（真缓冲的结束地址） |
| `warmup_frames` | OV5647 为 30 | 状态机 `WARMUP` 阶段丢弃的帧数 |
| `mode->format` | `CAM_FMT_*` | 不写硬件，原样放进帧头交给用户态 |

### 锁与调用约定

所有 `sensor_ops` 回调都在进程上下文中调用，调用时桥接驱动持有拍照睡眠锁 `cam.busy`，
所以回调里可以忙等，也可以睡眠，不会和另一次拍照并发。`camwrite()` 修改控制值时同样先拿这把锁，
保证不会在 `stream_on` 读取控制值的过程中改动它。

### 增加一个新传感器（例：IMX219，Camera v2）

只需要新增一个文件，`unicam.c` 不用改：

```c
// kernel/imx219.c（示意）
static const struct cam_mode imx219_modes[] = {
  { .name = "640x480 RAW10", .width = 640, .height = 480,
    .bytesperline = 800, .csi_dt = CSI2_DT_RAW10,
    .format = CAM_FMT_SRGGB10P,          // IMX219 是 RGGB
    .priv = &imx219_vga_regs },
};
static const struct sensor_ops imx219_ops = {
  .power_on = imx219_power_on,           // 扩展 GPIO 5 上电，I2C 地址 0x10，ID 0x0219
  .power_off = imx219_power_off,
  .stream_on = imx219_stream_on,
  .stream_off = imx219_stream_off,
  .set_ctrl = imx219_set_ctrl,
};
static struct camsensor imx219_sensor = {
  .name = "IMX219", .ops = &imx219_ops,
  .modes = imx219_modes, .nmodes = NELEM(imx219_modes),
  .bus = { .data_lanes = 2, .noncontinuous_clk = 1, .vc = 0 },
  .warmup_frames = 30,
};
void imx219_init(void) { camsensor_register(&imx219_sensor); }
```

然后在 `main()` 里 `camera_init()` 之前调用 `imx219_init()`，`Makefile` 里加上 `imx219.o`。
启动时 `camera_init()` 按注册顺序逐个调用 `power_on`，第一个读到正确芯片 ID 的传感器被绑定。
用户态 `camshot` 从帧头读取宽、高和 Bayer 顺序，所以不需要修改。

## 2. 板级连线（Pi 3B+）

| 项目 | 值 | 来源（Raspberry Pi Linux 设备树/驱动） |
|---|---|---|
| 传感器 I2C | BSC0（`0x7e205000`）走 GPIO 44/45，ALT1，地址 0x36 | `bcm283x-rpi-i2c0mux_0_44.dtsi`、`bcm283x.dtsi`、`overlays/ov5647.dtsi` |
| 摄像头供电 | 固件扩展 GPIO 5（`CAM_GPIO0`），mailbox 编号 128+5 | `bcm2710-rpi-3-b-plus.dts`：`cam1_reg` |
| 传感器时钟 | 摄像头板上的 25 MHz 晶振，不需要 SoC 输出时钟 | overlay：`cam1_clk` fixed-clock 25 MHz |
| 接收器 | Unicam CSI1：寄存器 `0x7e801000`，lane 时钟门控 `0x7e802004` | `bcm270x.dtsi`：`csi1` |
| 中断 | `interrupts = <2 7>`，即 GPU IRQ 39 | `bcm270x.dtsi` |
| 电源域 | `RPI_POWER_DOMAIN_UNICAM1` = 13，固件里的编号是 **14** | `raspberrypi-power.c`：`dom->domain = xlate_index + 1` |
| lp 时钟 | 时钟管理器 `CM_CAM1CTL/DIV`（`0x7e101048/4c`），100 MHz | `clk-bcm2835.c`；unicam：`clk_set_rate(..., 100 MHz)` |
| VPU 时钟 | Unicam 要求至少 250 MHz | unicam：`UNICAM_MIN_VPU_CLOCK_RATE`；本工程 `config.txt` 已有 `core_freq=250` |
| CSI-2 链路 | 2 条数据 lane，非连续时钟 | overlay：`data-lanes = <1 2>; clock-noncontinuous` |

电源域编号是一个容易出错的地方：设备树里的 `UNICAM1` 是 13，但发给固件时要加 1。

ARM 物理地址 = 总线地址 `0x7e......` 换成 `0x3f......`。这些寄存器都在内核已经映射的 16 MB
外设窗口里。

## 3. 启动时探测

`main()` → `ov5647_init()` → `camera_init()`：

1. `ov5647_init()` 用 `camsensor_register()` 登记 OV5647 驱动，此时还不访问硬件；
2. `camera_init()` 注册字符设备 `CAMERA`（5）→ `/dev/video0`，不论是否找到摄像头；
3. 对每个登记过的传感器，先检查它的默认模式 Unicam 能否接收（lane 数、每行字节数、帧大小），
   再调用 `power_on`。OV5647 的做法是：固件扩展 GPIO 5 先拉低 20 ms、再置 1 → 等待
   100 ms → GPIO 44/45 以约 25 kHz 软件 SCCB 读取 `0x300a/0x300b`，应为 `0x56 0x47`；
4. 第一个成功的传感器被绑定，打印
   ```text
   camera: OV5647 on CSI1 (2 lanes) -> /dev/video0, 640x480 RAW10
   ```
   然后调用 `power_off`，等到 `read()` 时再上电；
5. 都不成功时打印 `camera: no sensor found (N driver(s) tried)`，`/dev/video0` 的
   `read()` 返回 -1。

BSC0 也可以接到 GPIO 0/1（ALT0，HAT ID EEPROM）。为了避免两组引脚同时接在同一个控制器上，
如果 GPIO 0/1 当前是 ALT0，就先把它们切成输入。

驱动仍保留与 Linux `i2c-bcm2835` 相同的 BSC0 组合事务实现：写 16 位寄存器地址，不发送 STOP，
紧接 repeated START 后读取数据。BCM2835 BSC 有状态机硬件限制，写 FIFO 不能预填；驱动先启动
空写事务，等 `TXW` 后填充，并在最后一个写字节后立即切换到读阶段。`i2c_write_read()` 实现了
这条路径。不过当前这块 OV5647 模块已经通过实机确认 BSC0 始终失败而 GPIO SCCB 可以工作，
所以 OV5647 v8 不再先执行 BSC 探测，而是从第一次 ID 读取起直接使用软件 SCCB。BSC 路径保留
给控制器诊断和以后其它传感器使用。探测 `0x36` 仍失败时还可以检查地址 `0x10`，
用来提示是否实际连接了 IMX219 Camera v2。IMX219 探测同样同时尝试 repeated-START 与分离
STOP/START，并读取寄存器 `0x0000` 的 16 位芯片 ID；有效值为 `0x0219`。随后还检查 `0x1a`
（IMX477/IMX708/IMX296 等常用地址），最后明确打印三个候选地址的 ACK/NACK 摘要。

若三个硬件 BSC 探测都 NACK，驱动会再做一次独立的软件 I2C 诊断。它临时将 GPIO44/45 从
ALT1 改成开漏 GPIO，以约 100 kHz bit-bang 发送地址字节并读取第九个时钟的 ACK，随后恢复
ALT1。这个测试不读写传感器寄存器：若 BSC 与 bit-bang 都 NACK，故障位于控制器之外（排线、
插槽、真实供电或模块）；若 bit-bang ACK 而 BSC NACK，传感器、地址、供电、上拉和排线均已
得到验证，故障只在 BSC0 的复用或时序路径。官方 `bcm283x.dtsi` 的 `i2c0_gpio44` 明确规定
GPIO44/45 使用 ALT1，所以不能把它改成 ALT0。

早期 bring-up 版本在出现“bit-bang ACK、BSC NACK”时，会恢复 ALT1 后做 BSC 恢复扫描：依次使用
100/50/25/10 kHz 重新读取 `0x300a/0x300b`。每次重配同时按 Linux `i2c-bcm2835` 的比例更新
`BSC_DIV` 与 `BSC_DEL`，并在写入地址、长度和 FIFO 后用 `dsb sy` 保证寄存器配置在 START
之前对控制器可见。第一个成功的速率会保留给后面的整个传感器寄存器表。成功日志为：

```text
ov5647: BSC recovered at 25000 Hz, chip-id=5647
```

失败时每档会打印 `status/now/div`。若四档都保持 `ERR=0x100`，就已经排除了总线速度和普通
MMIO 写入顺序问题，下一步应检查 BSC0 控制器路由/固件占用，而不应再更换传感器或排线。

早期 bit-bang 诊断在第九个 ACK 时钟也调用了普通的 `bb_release()`。该函数用于数据位时会等待
SDA 回到高电平，但 ACK 的定义恰好是从设备保持 SDA 为低，因此曾在 ACK 前错误等待 100 µs，
造成同一个模块有时显示 ACK、有时显示 NACK。现在 ACK 周期只把 SDA 切成输入，在 SCL 高电平
中间直接采样；而且无论这次诊断结果如何，BSC 的四档恢复扫描都会执行。

当前驱动直接使用完整的软件 SCCB 事务读取 `0x300a/0x300b`。读到 `0x5647` 后打印
`using GPIO software SCCB; BSC0 bypassed`，此后 OV5647 的读、写及模式寄存器表全部走 GPIO44/45
软件 SCCB。软件事务采用 OV5647 可接受的 STOP/START 分离读法；速度较硬件控制器低，但启动时
写约百个短寄存器仍可接受，并且图像本身通过 CSI-2/Unicam DMA 传输，不受软件 I2C 速度影响。
软件路径开始事务时会直接关闭 BSC、清 FIFO 和 sticky 状态，再把 GPIO44/45 切换为开漏 GPIO；
它不会调用 BSC 的 TA 等待恢复，因为触发后备路径的前提正是硬件状态机已经出现异常 TA/CLKT。
此外，BSC 的失败事务可能让 SCCB 从设备停在一个字节中间。软件事务开始时先检查 SDA/SCL；
只有 SDA 被从设备保持为低时才产生最多 9 个 SCL 恢复脉冲，随后发送 STOP。总线已经空闲时
不再无条件插入 9 个额外时钟。首次芯片 ID 读取最多重试三次，避免一次残留事务让启动探测偶发失败。
OV5647 公共表中的 `0x0103=1` 会触发软件复位，期间 SCCB 暂时不应答。GPIO SCCB 路径在该项后
明确等待 10 ms；普通寄存器写入也最多重试三次，防止复位后的第一项 `0x3034` 偶发 NACK。

诊断还通过 `GET_GPIO_CONFIG` 回读 CAM_GPIO0 的方向与极性。标准 Pi 3 应显示
`direction=output polarity=normal state=1`。三个常见地址均 NACK 时，bit-bang 会继续扫描所有
合法的 7-bit 地址 `0x08..0x77`；扫描只发送地址字节与 STOP，不写任何传感器寄存器。若
`0x36` 已经 ACK、但 ID 寄存器读取失败，则不会继续扫描，而是立即断电并交给外层执行干净的
电源周期。

电源配置现在与 Linux `gpio-raspberrypi-exp.c` 一致：先读取并保留固件 GPIO 的 polarity，再设置
CAM_GPIO0；低—高电源边沿后通过 `GET_GPIO_STATE` 回读。I2C 初始化还会显式设置 `BSC_DEL` 的
FEDL/REDL，避免继承固件使用 BSC0 后留下的边沿采样配置。

失败诊断示例：

```text
ov5647: no reply at i2c0 0x36 (GPIO44/45), BSC raw=90000131 flags=131 \
now=... div=2500 fsel=55 levels=3
```

- `flags & 0x100` 是 `ERR/NACK`：控制器发出了事务，但从设备未应答；
- `flags & 0x200` 是时钟拉伸超时；
- `fsel=55` 表示 GPIO44/45 都是 ALT1；
- `levels=3` 表示 SDA、SCL 均为高，物理总线处于空闲状态；若不是 3，优先检查摄像头供电、排线
  方向和接触；
- `now & 1` 是 BSC 的 `TA`（transfer active）。错误清理后它必须为 0。真 BCM2837 与 QEMU
  在这一点不同：若 TA 尚为 1 就撤掉 `I2CEN`，真机状态机可能被冻结。驱动先保持 `I2CEN`、
  清 FIFO并等待 NACK+STOP 使 TA 清零，随后才关闭控制器并清 sticky 状态位；
- 旧版内核的 `%x` 会把最高位为 1 的寄存器值按有符号数输出，所以 `0x90000131` 曾显示为
  `-6ffffecf`；现在 `%x` 已按标准改成无符号十六进制。

若真机同时满足下面四项：

1. `CAM_GPIO0 direction=output polarity=normal state=1`；
2. BSC 在 100/50/25/10 kHz 都返回 `ERR/NACK`；
3. 修正 ACK 时序后的 GPIO bit-bang 在 `0x36/0x10/0x1a` 都 NACK；
4. bit-bang 全地址扫描找不到任何设备；

则不再是 BSC 寄存器、分频或地址问题，而是摄像头没有在电气上响应。Pi 3B 和 3B+ 的官方设备树
都包含 `bcm283x-rpi-i2c0mux_0_44.dtsi`，确认标准 CSI1 插座连接的是 GPIO44/45；两款板的
`cam1_reg` 也都是 expander GPIO 5、active-high。此时应断电后重新插紧排线并按连接器内部金属弹片
确认触点方向，检查模块电源，或用 Raspberry Pi OS 加 `dtoverlay=ov5647` 后在同一套硬件上验证。
如果 Linux 同样探测不到 `0x36`，就可以确认是排线、插座或模块问题，而不是 xv6 驱动。

## 4. 拍一帧的完整流程

`read(/dev/video0)` → `cam_capture()`：

| 步骤 | 做什么 |
|---|---|
| 1 | `mbox_set_domain(14, on)`：打开 UNICAM1 电源域 |
| 2 | CAM1 时钟：源 PLLD_PER（500 MHz）÷ 5 = 100 MHz；如果 BUSY 位不置起，就改用 19.2 MHz 晶振并打印提示 |
| 3 | `sensor->power_on`：传感器上电，校验 ID，输出使能，进入 LP-11（stream off） |
| 4 | 清零帧缓冲，状态置为 `WARMUP` |
| 5 | `unicam_start(&sensor->bus, mode)`：按 Linux `unicam_start_rx()` 配置接收器，DMA 指针先指向 dummy |
| 6 | 打开 CSI1 中断 |
| 7 | `sensor->stream_on(mode)`：OV5647 依次写公共寄存器表 → 640x480 模式表 → HTS/VTS/曝光/增益 → VC → 缓存的测试图 → 退出软件待机 → MIPI 开始发送 |
| 8 | 等待状态变成 `DONE`：每个 xv6 tick（100 ms）醒来一次，中断也会唤醒；最多 4 s |
| 9 | `sensor->stream_off` → 关中断 → `unicam_stop()`（Linux `unicam_disable()`）→ `sensor->power_off` → 关时钟 → 关电源域 |
| 10 | 把帧头和数据拷给用户 |

顺序与 Linux 相同：开始时先启动接收器再让传感器出流，结束时先停传感器再停接收器。
真机 bring-up 阶段会分别打印 sensor powered、receiver enabled、三组 OV5647 寄存器表以及
sensor streaming；因此并发的 Wi-Fi/SDIO 错误日志不会再掩盖摄像头实际停在哪个阶段。

首次真机出流时发现，启用尚未验证的 CSI1 legacy IRQ 后 CPU0 可能陷入电平中断，连负责更新
`ticks` 的 Generic Timer 都得不到执行，于是原本基于 `ticks` 的 4 秒超时也永远不会到达。
bring-up 版本因此屏蔽 CSI1 IRQ，使用 `CNTVCT_EL0` 作为独立的 4 秒期限，并每 1 ms 轮询一次
Unicam 状态。这样无论 CSI-2 是否收到帧，都会成功返回或打印 FS/FE/STA/IBWP 诊断；得到成功的
轮询帧以后再单独验证 legacy IRQ 路由。

当日志显示

```text
unicam: frame captured by polling; CSI1 IRQ 39 intentionally masked
```

表示完整的 sensor → CSI-2 → Unicam → DMA 路径已经成功，只是 bring-up 版本主动没有打开
legacy IRQ 39；它不表示硬件中断已经打开却丢失。IRQ 路由验证应作为下一阶段单独进行，并始终
保留 `CNTVCT_EL0` 的有界轮询作为兜底，避免错误的电平中断再次饿死 CPU0。

## 5. Unicam 配置要点

全部来自 `unicam_start_rx()`，这里只列出与本场景相关的取值：

| 寄存器 | 值 | 说明 |
|---|---|---|
| lane 时钟门控 `0x7e802004` | `0x5a000015` | 密码 `0x5a` + CLK、DAT0、DAT1 各 `01` |
| `ANA` | 先 `AR=1, CTATADJ=7, PTATADJ=7`，1.5 ms 后清 `AR` | 模拟部分复位 |
| `CTRL` | `CPR` 脉冲复位；CSI-2 模式；`PFT=0xf`；`OET=128` | |
| `PRI` | `PP=0xe, NP=8, PT=2, PE=1` | AXI 访问优先级 |
| `CLT` / `DLT` | `term_en=2, settle=6` | 以 lp 时钟为单位 |
| `CMP0` | `PCE GI CPH PCDT=1` | 包比较；Linux 注释：没有它会丢帧结束事件 |
| `CLK` / `DAT0` / `DAT1` | `EN | LPE` | 非连续时钟：不打开 HS 终端 |
| `IBLS` | 800 | 每行字节数（640×10/8，必须是 16 的倍数） |
| `IPIPE` | 0 | 不解包，保持 CSI-2 RAW10 打包格式 |
| `IDI0` | `0x2b` | VC 0，数据类型 RAW10 |
| `ICTL` | `FSIE FEIE IBOB`，再 `LIP=1`、`TFC=1` | 帧开始/结束中断；装载指针；与下一个帧开始同步 |

## 6. 帧状态机

```text
WARMUP ──(第 30 个帧开始，或 1.5 s)──► ARMED ──(下一个帧开始)──► CAPTURE ──(帧结束)──► DONE
  │ DMA 指向 dummy（大小 0）            │ 写入真缓冲地址            │ 写回 dummy
  │ 传感器 AEC/AGC 在收敛               │ 下一帧开始时生效          │ 当前帧继续写进真缓冲
```

关键在于：写进 `IBSA0/IBEA0` 的新地址**要到下一个帧开始才生效**（Linux 也是在帧开始时安排
“下一帧”的缓冲区）。所以在真缓冲那一帧开始后立刻写回 dummy，缓冲区里就恰好留下一整帧，
不会被后面的帧覆盖。

dummy 的结束地址等于起始地址（大小 0），这是照搬 Linux 的做法：Linux 注释说，循环缓冲模式
有一个会导致越界写的硬件缺陷，所以 dummy 要按 0 字节来编程。

### 中断与轮询

状态机 `unicam_service()` 由两条路径驱动，都在 `cam.lock` 保护下运行：

- **CSI1 中断**（CPU0，GPU IRQ 39）：每个帧开始/结束都会触发，响应最及时；
- **读者轮询**：`read()` 每个 tick（100 ms）自己调用一次。

只靠轮询也能拿到完整的一帧：100 ms 内的多个帧开始事件会合并成一次，但 dummy 总是在“某一帧
正在写入真缓冲”的时候写入，而这一帧会在切换生效前写完。

中断号 39 来自设备树，没有在真机上验证过。如果它实际属于别的外设，处理函数永远清不掉它，
CPU0 就会陷在中断里出不来。所以中断只在拍照期间打开，并且连续 100 次没有发现 Unicam 状态时
会自动关掉它（打印 `unicam: IRQ 39 has no Unicam status; polling instead`），之后只靠轮询。

## 7. DMA 内存

| 项目 | 取值 |
|---|---|
| 物理地址 | `CAMDMA_PA = 0x07200000`，大小 1 MB |
| 帧缓冲 | `CAMDMA` 开头，最大 `CAM_MAX_FRAME_BYTES` = 512 KB（OV5647 640x480 用 384000 字节） |
| dummy | `CAMDMA_PA + CAM_MAX_FRAME_BYTES`（编译期断言保证在 1 MB 之内） |
| 映射 | `PTE_NORMAL_NC`：非缓存的普通内存 |
| 总线地址 | `0xC0000000 | phys`，与 `dwc2.c` 的 `DWC2_DMA_BUS` 相同；VideoCore 侧不经过 L2 |

这样安排的原因：

- **必须物理连续**：Unicam 只接受一段起止地址。`kalloc()` 按页分配，不能保证连续；
- **不放进内核 BSS**：`kinit2()` 把物理 2 MB 以上的内存全部交给分配器。内核镜像现在到 1.42 MB，
  再加 375 KB 的静态缓冲就离 2 MB 很近了，以后内核一旦越过 2 MB，分配器就会悄悄复用内核内存。
  所以帧缓冲放在 `PHYSTOP`（112 MB）之上、ramdisk 之后，`kalloc` 不会碰到这里；
- **非缓存映射**：DMA 前后都不需要做 cache 维护，也就不会读到旧的 cache 行；
- `main()` 新增了一个检查：内核镜像结束地址如果超过 `EARLYTOP`，就通过早期串口报错并停机，
  而不是继续运行、出现难以排查的内存损坏。

## 8. OV5647 配置

使用 Linux 的默认模式：640x480，2x2 binning + skipping（从 2560x1920），RAW10，像素率 55 MHz，
HTS 1852，VTS 504，约 59 帧/秒，Bayer 排列 BGGR。

与 Linux 的不同之处：

| 项目 | Linux | 本驱动 | 原因 |
|---|---|---|---|
| HTS/VTS、曝光、增益 | 由 V4L2 control 写入 | 直接写入同样的默认值 | 没有 V4L2 |
| AEC/AGC（`0x3503`） | 默认手动（`0x03`），由 libcamera 的 IPA 计算 | **打开传感器自带的自动曝光/增益**（`0x00`） | xv6 没有 ISP，也没有用户态的曝光控制回路 |
| AWB（`0x5001`） | 默认关 | 关 | RAW 输出，由用户态 `camshot` 做白平衡 |

预热帧数 `warmup_frames = 30`（约 0.5 s，写在 `struct camsensor` 里），给自动曝光留出收敛时间。

测试图（`0x503d`）：`write(fd, "pattern=1", 9)` 选彩条，2 是彩色方块，3 是随机数据，0 关闭。
桥接驱动把它转成 `set_ctrl(CAM_CTRL_TEST_PATTERN, N)`，由 OV5647 驱动检查取值范围并缓存，
下一次 `stream_on` 时写入。
它由传感器内部产生，不受对焦和光线影响，正好用来单独验证 CSI-2 链路和转换程序。

## 9. 用户接口

`/dev/video0`：

- `read(fd, buf, n)`：拍一帧；`n` 至少为帧头加一帧的大小，按 `CAM_READ_MAX`（524328）
  分配总是够用。返回值是 `struct cam_frame_hdr`（40 字节：magic、宽、高、每行字节数、格式、
  数据长度、序号、时间戳）加上打包的 RAW10 数据；
- 帧头里的 `format` 同时给出 Bayer 顺序：`CAM_FMT_SBGGR10P`（OV5647）、`SGBRG10P`、
  `SGRBG10P`、`SRGGB10P`（IMX219）；
- `write(fd, "pattern=N", 9)`：选择之后拍照使用的测试图；传感器不支持时返回 -1；
- 同一时间只允许一次拍照（睡眠锁），每次 `read()` 都会完整地上电、预热、拍照、断电，
  大约需要 1 秒。

RAW10 打包格式：每 4 个像素占 5 个字节，前 4 个字节是 4 个像素的高 8 位，第 5 个字节依次放
它们的低 2 位。

### camshot

```text
camshot [-o OUT.bmp] [-r OUT.raw] [-i IN.raw] [-t PATTERN] [-s] [-w]
  -o  输出 BMP（默认 /boot/camera.bmp）
  -r  另存原始帧（帧头 + RAW10）
  -i  不拍照，转换之前用 -r 保存的原始帧
  -t  测试图：1 彩条，2 彩色方块，3 随机
  -s  半尺寸 320x240：每个 2x2 Bayer 块输出一个像素
  -w  不做白平衡
```

拍摄成功后，`camshot` 会分别打印收到帧、RAW10 解包、白平衡/亮度统计和 BMP 写盘阶段，便于区分
内核采集与用户态转换问题。BMP 数据按 16 行批量写入，而不是每行调用一次 `write()`；640×480
半尺寸输出由 240 次 FAT32 写操作降为 15 次，避免把正常但缓慢的 `/boot` 写盘误判成相机卡死。
白平衡求和与绿色通道亮度直方图也合并为一次全帧扫描，并每 64 行打印进度。
注意当前 xv6 调度器不会仅仅因为任务运行在用户态就把它降到内核线程以下：`camshot` 和
`brcmf0_wq` 默认都是 `SCHED_OTHER/nice 0`。SDIO IRQ 上半部仍可立即抢占用户程序，而 CMD52
事务错误应根据 Arasan 状态、IRQ 和 Wi-Fi 工作项并发单独诊断，不能仅凭日志出现时间归因于
`camshot` 的 CPU 计算。

`camshot -t N` 使用 OV5647 内部生成的标准测试图。测试图已经校准，因此转换器采用固定
1.0 白平衡和固定亮度，不再运行灰世界/直方图统计；这使测试图专门验证 sensor、CSI-2、
Unicam DMA、RAW10 转换和 FAT32 写盘。只有不带 `-t` 的真实画面才运行自动白平衡和亮度统计。
启动时的 `camshot: converter fixed-pattern-v2 fat-progress batch16` 用来确认根文件系统中的用户程序确实
已经更新。

`/dev/video0` 的字符设备节点会在 sensor probe 之前注册，所以 open 成功不代表启动时已经绑定
传感器。GPIO software SCCB 若在启动探测时偶发丢失 ACK，旧实现会让设备一直 inactive 到下次
重启。现在首次 `read()` 或 `pattern=N` 控制写发现 `cam.sensor == 0` 时，会在 `cam.busy`
sleeplock 下重新探测并绑定；成功后打印 `camera: OV5647 on CSI1 ...`，无需依赖重新启动碰运气。
启动阶段只探测一次，避免无摄像头时拖慢开机；用户首次 `read()` 或控制请求触发的 lazy probe
最多执行三次完整断电/上电周期，每次失败后等待 100 ms。若三次 GPIO bit-bang 扫描仍全部
NACK，且诊断显示 `levels=3`，说明总线空闲但模块没有应答，应检查排线触点、模块电源或传感器。
软件 SCCB 半周期已放宽到约 20 us（总线约 25 kHz），提高克隆模块和较长排线的
setup/hold 裕量。v8 从本次启动的第一次 ID 读取起就使用软件 SCCB，不再用硬件控制器的失败
事务反复扰动传感器；完整芯片 ID 读取也不依赖一次容易抖动的 address-only ACK probe。
一次事务即使收到 ACK，若读出的 ID 是 `0x3631` 等非 `0x5647` 值，也只被视为位采样不稳定；
驱动会拒绝该值并继续完整重试。如果地址 `0x36` 已完成过寄存器事务但三次 ID 仍不稳定，
本轮直接断电并交给外层重试，不再执行会进一步扰动总线的全地址扫描。为了让模块电容充分
放电并等待克隆模块内部时钟稳定，低电平时间由 5 ms 增至 20 ms，上电等待由 50 ms 增至 100 ms。

处理流程全部用整数运算，因为本工程的用户程序以 `-mcpu=cortex-a53+nofp` 编译，没有浮点：

```text
RAW10 解包 → 减黑电平 16 → BGGR 双线性去马赛克（边界镜像，保持 Bayer 颜色）
→ 灰度世界白平衡（R、B 的均值对齐 G，增益限制在 0.5～4）
→ 自动电平（G 的 99% 分位映射到满幅，最多 ×8）→ gamma 2.2 查表 → 24 位 BMP
```

**文件大小限制**：原生 xv6 文件系统每个文件最多 `MAXFILE = 12 + 256` 块，即 268 KB。
640x480 的 BMP（900 KB）和原始帧（375 KB）都超过了这个限制，只能写到 `/boot`
（以读写方式挂载的 FAT32 启动分区，见 `readme_vfs.md`）。320x240 的 BMP（225 KB，`-s`）
放在哪里都可以。

取出图片最简单的办法：关机后把 SD 卡插到 Mac 上，`bootfs` 卷里就有 `camera.bmp`。

覆盖已有的 `/boot/camera.bmp` 时，`open(..., O_TRUNC)` 必须先释放旧 FAT cluster 链。旧实现
逐 cluster 读取并改写两份 FAT，一个 320x240 BMP 可能在真正的像素转换开始前触发五百多次
SD 扇区事务，串口最后只看到 `sensor test pattern; fixed WB/level`，容易误判为转换死循环。
现在 FAT 链按 FAT 扇区成批清零并镜像到所有 FAT 副本；`camshot` 还会分别打印
`preparing output`、`output opened` 和 `converting and writing`，从而明确区分截断、BMP 头写入
以及图像转换阶段。

## 10. 测试

### 10.1 已经验证的

| 内容 | 方法 | 结果 |
|---|---|---|
| 编译 | aarch64-linux-gnu-gcc 11（你电脑上的虚拟机）和 13，`-Werror` | 通过 |
| 没有摄像头时的启动 | QEMU raspi3b | 打印 `camera: no sensor found (1 driver tried)`，不会卡住；`camshot` 正常报错退出 |
| I2C 超时路径 | QEMU 8.2 中 BSC 是未实现设备，寄存器读出全 0 | 20 ms 后超时返回，不会挂死 |
| 图像转换 | 在主机上编译同一份 `camshot.c`，输入合成的 RAW10 帧（四色条、渐变、左上角白块，并叠加偏暖色温） | 颜色和方向正确，白平衡把色温校正回来；全尺寸和 `-s` 都正确 |
| 传感器接口重构 | BGGR、RGGB 640x480 和 GBRG 320x240 三种合成帧；与重构前的版本比较 | 三种 Bayer 顺序结果一致，BGGR 的像素值与重构前完全相同；OV5647 的 97 个寄存器写入不变，Unicam 只有 6 处改成取自 `bus`/`mode`，代入 OV5647 的参数后取值与原来相同 |
| Pi 3 真机 OV5647 探测 | CAM_GPIO0 上电；BSC0 四档恢复；GPIO44/45 软件 SCCB 读取芯片 ID | BSC0 在 100/50/25/10 kHz 均失败；软件 SCCB 稳定读到 `0x5647`，切换到 GPIO 后备路径并成功绑定 `/dev/video0` |

这一轮测试还发现并修复了 `camshot` 的一个释放后使用：它在 `free(frame)` 之后读取了
`hdr->sequence`。

### 10.2 QEMU 无法验证的

QEMU 没有模拟 Unicam、OV5647，也没有模拟 BSC，所以下面这些只能在真机上测：传感器 I2C 通信、
mailbox 电源域和 GPIO、CAM1 时钟、CSI-2 接收、DMA、中断号。

### 10.3 真机排查顺序

1. **启动日志**里应该有
   `camera: OV5647 on CSI1 (2 lanes) -> /dev/video0, 640x480 RAW10`。
   - `ov5647: no reply at i2c0 0x36`：检查排线方向（Pi 3B+ 上蓝色面朝网口）、两端是否插紧；
   - `ov5647: firmware GPIO 133 (camera power) failed, firmware rev N`：通过 mailbox
     打开摄像头电源失败。下面两行分别是 `SET_GPIO_CONFIG` 和 `SET_GPIO_STATE` 的结果。
     驱动会照样继续做 I2C 探测，以防模块其实已经通电。`N` 是固件的编译时间（Unix 秒），
     在 Mac 上用 `date -r N` 换算：

     | 第一行显示 | 含义 |
     |---|---|
     | `no reply` | mailbox 超时，固件没有回应 |
     | `message rejected`（status 不是 `0x80000000`） | 固件拒绝整条消息 |
     | `tag response ... done 0` | 单个 tag 没置 response bit；Linux 不把它单独当失败，而是继续检查返回结构的第一个字 |
     | `firmware reported an error`，`done 1 len 0` | 标签被应答，但没有返回数据：QEMU 就是这样；真机上同样说明固件没有实现它 |
     | `firmware reported an error`，`len 8`、`first word 133` | 固件认识标签，但认为 GPIO 133 无效（这个固件或板子上没有对应的扩展 GPIO） |

     判断规则与 Linux `gpio-raspberrypi-exp.c` 相同：property 消息整体成功后复制 payload，
     GPIO 操作成功时固件把第一个字写回 0。Linux 的 property 层不检查单个 tag 的 response bit，
     所以 xv6 也不能因为 `done 0` 就在复制 payload 之前提前失败。

     每行末尾的 `stale replies K` 是发请求前从 FIFO 里清掉的旧回复数量。这些回复是工程里其他驱动
     （`arasan_sdio.c`、`dwc2.c`、`sd.c` 各自有私有 mailbox 代码）超时后遗留下来的。
     现在的 mailbox 代码按 Linux `rpi_firmware_property()` 把请求的 `req_resp_size` 设为 0，
     同时写入前清空 FIFO，并且只接受与本次请求地址相同的回复。如果 `K > 0`，说明以前那次“tag not supported”
     很可能是读到了别人的回复。

     **兜底方案**：如果固件确实不处理这个标签，可以让固件在启动时就打开摄像头电源，
     这样就不需要 mailbox 了。从固件仓库取出 `dt-blob.dts`，在 `pins_3bplus` 下把
     `pin@p133` 改成 `function="output"; termination="no_pulling"; startup_state="active";`，
     用 `dtc -I dts -O dtb -o dt-blob.bin dt-blob.dts` 编译，然后放到 boot 分区。
   - `unexpected chip id`：可能不是 v1.3 模块（v2 是 IMX219，地址 0x10）。
2. **`camshot -t 1`**：传感器内部彩条。它能验证电源域、时钟、CSI-2、DMA 和转换全部正常，
   而且与镜头和光线无关。
3. **`camshot`**：真实画面。
4. 失败时，内核打印的诊断行是这样的：
   ```text
   unicam: no frame (state S, FS a, FE b, irqs c, sta X, ibwp Y, lp N Hz)
   ```
   | 现象 | 含义 |
   |---|---|
   | `FS 0 FE 0` | 一个包都没收到：检查电源域、lane 时钟、传感器是否真的开始出流 |
   | 有 FS、`state 1`（WARMUP）超时 | 帧开始事件太少，比如只收到了零星几个 |
   | `sta` 含 `0x80`（CRCE）/`0x4`（SBE）/`0x8`（PBE） | CSI-2 链路误码：检查 `CLT/DLT` 的 settle 值和 lp 时钟频率 |
   | `irqs 0` 但拍照成功 | 中断号不对，目前靠轮询工作，可以把实际中断号补上 |
   | 打印 `CAM1 clock from 19.2 MHz crystal` | PLLD_PER 没有运行，lp 时钟偏低，settle 时间会跟着变 |

## 11. 已知限制与后续工作

- **OV5647 目前只提供 640x480 RAW10 一个模式**：Linux 中的 1296x972 等模式每帧要 1.5 MB 以上，
  超过了 `CAM_MAX_FRAME_BYTES`（512 KB）。扩大 `CAMDMA` 区后，在 `ov5647_modes[]` 里加上即可，
  桥接驱动不用改；存成文件仍受 268 KB 的限制；
- **模式选择**：桥接驱动总是使用 `modes[0]`，还没有让用户选择模式的接口（可以在 `camwrite`
  里加 `mode=N`）；
- **每次拍照都完整地上电和预热**（约 1 秒），没有连续取流的接口，也没有 `poll()`；
- **曝光完全交给传感器**，没有手动曝光/增益接口；
- **I2C 是轮询方式**，每个寄存器传输都要在自旋锁里关中断等待约 0.3 ms，写完整张寄存器表的
  这段时间会抬高软实时任务的延迟（见 `readme_soft_realtime.md` 第 10 节）；
- **mailbox 代码仍有重复**：`arasan_sdio.c`、`dwc2.c`、`sd.c` 还各有一份私有的 mailbox 实现，
  没有使用新的 `mbox.c`。它们只在启动时使用，目前不会和摄像头冲突，以后可以统一过来；
- **`config.txt` 不要打开固件的摄像头支持**（`start_x=1`、`camera_auto_detect=1`），
  否则固件会占用同一个传感器和 I2C。

## 12. 顺带修复

- **`user/user.h` 缺少 include guard**：`user/sync.h` 会再次包含它，导致 `struct thread`
  重复定义，`user/syncdemo.c` 编译失败。修改前当前分支的 `fs.img` 是编不出来的。
- **`main()` 检查内核镜像大小**：内核结束地址超过 `EARLYTOP`（2 MB）时，通过早期串口
  报错并停机，见第 7 节。
