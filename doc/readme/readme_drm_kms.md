# Raspberry Pi 3 DRM/KMS：固件兼容层与原生 VC4 扫描输出

## 两阶段边界

第一阶段保留 VideoCore 固件 framebuffer，不直接编程 VC4 的 HVS 和 Pixel
Valve。内核新增 Linux 风格的 DRM 对象模型和 `/dev/dri/card0`，把
“谁拥有显示缓冲、当前扫描哪一帧、何时翻页”从 `/dev/fb0` 的裸内存访问中
分离出来。`/dev/fb0` 继续作为启动控制台兼容接口。

第二阶段由 `vc4_native.c` 接管真正的扫描输出：显存从 ARM buddy allocator
取得，HVS display list、HVS FIFO 1、Pixel Valve 2 和 vblank IRQ 都由内核
直接控制。固件 framebuffer 不再提供 DRM 扫描数据。现阶段仍复用启动固件
已经建立的 HDMI 物理链路和 25.175 MHz 像素时钟；这相当于 Linux VC4 中
HVS/CRTC/plane 部分已经原生化，而 HDMI encoder 的时钟、PHY、EDID 热插拔
留给下一步。

这不是 Linux DRM 的二进制 ABI；结构和语义接近 Linux DRM/KMS，但 ioctl
编号与定长返回结构是 xv6 专用的。

## 分层

```text
用户程序 drmtest / 将来的窗口合成器
                 │ open/ioctl/mmap/poll
                 ▼
          /dev/dri/card0
                 │
       drm_drv.c：通用字符设备层
                 │ drm_driver callbacks
                 ▼
 vc4_firmware_kms.c：GEM / FB / CRTC / Connector
                 │
                 ├── mmap ──► 两个 GEM scanout 缓冲
                 │
                 └── PAGE_FLIP（commit_lock 串行化）
                       │
              ┌────────┴─────────┐
              │ Pi 3 原生后端   │ QEMU/探测失败回退
              ▼                  ▼
       vc4_native.c       bcm2708_fb_page_flip()
              │                  │ mailbox
              │                  ▼
              │          firmware framebuffer
              ▼
 ARM DDR ──► HVS display list ──► FIFO 1 ──► Pixel Valve 2 ──► HDMI
              ▲                                  │
              └──────── PAGE_FLIP ───────────────┘ PV2 vblank IRQ 42
```

## 双缓冲布局

第一阶段固件被要求分配 `640 x 960 x 32bpp` 的虚拟 framebuffer。第二阶段
原生后端改为从 buddy allocator 申请一个 4 MiB、物理连续的 order-10 区域；
前 `2 x 1,228,800` 字节分别作为两张画面，其余空间是高阶页分配产生的尾部
余量：

```text
native ARM scanout allocation
base + 0 * frame_bytes  ┌──────────────────────┐
                        │ GEM handle 1 / slot 0│  y = 0..479
base + 1 * frame_bytes  ├──────────────────────┤
                        │ GEM handle 2 / slot 1│  y = 480..959
                        └──────────────────────┘
```

用户态只绘制非显示缓冲。每个 slot 有一份七字 HVS plane display list，内容
包含 XRGB8888 格式、尺寸、pitch 和 `0xc0000000 | PA` 的 VideoCore bus 地址。
`DRM_IOCTL_MODE_PAGE_FLIP` 更新 `SCALER_DISPLIST1`，然后睡眠等待 Pixel Valve 2
的 VFP-start IRQ；中断到来后再核对 `SCALER_DISPLACT1` 已切到目标 list。这样
基地址只在帧边界生效，不会产生 tearing，也不再为每次翻页调用 mailbox。

## 对象与生命周期

- 一个 `drm_device`：`card0`。
- 一个固定连接器、编码器和 CRTC。
- 每次打开创建一个 `drm_file`，其中 handle、framebuffer ID 都属于该文件。
- 当前仅允许一个 DRM master，避免多个进程竞争同一 CRTC scanout。
- 原生后端的两个 dumb buffer 是 buddy 高阶页中的固定页对齐物理范围，`mmap()` 使用
  `PTE_NORMAL_NC` 映射，CPU 写入不会遇到普通缓存与扫描控制器不一致。
- `commit_lock` 是可睡眠 mutex：等待垂直同步期间不会像 spinlock
  那样长时间关闭本 CPU 中断。
- 关闭 DRM 文件时恢复 slot 0，清除 `current_fb`，再释放 master。

## 原生硬件初始化

`vc4_native_kms_init()` 按以下顺序接管硬件：

1. 先通过兼容 fbdev 请求 640×480 模式，让固件只完成 HDMI clock/PHY 链路；
2. 确认 PV2 已运行。QEMU 的 HVS/PV 寄存器为零，因此在这里安全回退；
3. 申请 4 MiB 连续 ARM RAM，清零并 clean data cache；
4. 在 HVS SRAM word 32 和 48 写两份 display list；
5. 将 HVS Composite Output Buffer 的 6144..12287 像素分给 FIFO 1，配置
   `SCALER_DISPCTRL1=640×480`；
6. 直接写 PV2 的 VGA porch/sync/active timing，启动 continuous video；
7. 使能 GPU IRQ 42。每个 VFP-start 中断增加 vblank sequence 并唤醒等待者。

初始化前会保存 HVS/PV2 寄存器。如果接管后 200 ms 内观察不到新帧或 HVS
报告 DMA 错误，驱动恢复旧寄存器、释放连续页，并继续使用第一阶段固件后端。

## 用户接口

头文件为 `include/uapi/drm.h`。第一阶段支持：

- 查询版本、资源、连接器、CRTC；
- 创建和映射两个 dumb GEM buffer；
- 为 GEM buffer 创建/删除 framebuffer；
- 同步 page flip 和等待 vblank；
- `poll(POLLOUT)`；异步 flip event 队列留到后续阶段。

测试命令：

```sh
/bin/drmtest
```

测试程序创建两张渐变画面并交替翻页，串口会输出 framebuffer ID 和 vblank
sequence。屏幕应每秒在两张完整画面间切换，不应出现半帧撕裂。

## 当前边界与后续

- 已完成：原生 GEM scanout、HVS plane/display list、HVS FIFO、PV2 timing、
  vblank IRQ 和原子帧边界翻页。
- 保留：`/dev/fb0` 和 firmware page-flip，供 QEMU 及硬件探测失败时回退。
- 尚未原生化：HDMI encoder 的 CPRMAN pixel clock、HDMI PHY、EDID/DDC、
  hot-plug detect 和音频。这些不影响当前固定 640×480 DRM 扫描输出，但若要
  动态 modeset 或完全关闭 mailbox 依赖，需要作为第三阶段补齐。
