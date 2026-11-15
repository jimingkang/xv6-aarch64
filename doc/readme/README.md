# 开发者文档索引

本目录保存按目标平台整理的开发者文档。面向 FriendlyARM Tiny210
（Samsung S5PV210）的文档从 [tiny210/README.md](tiny210/README.md) 开始。

排版后的 Tiny210 ARM32 专题书位于
[Tiny210 LaTeX 书稿](../tiny210-book/README.md)，可直接阅读
[编译后的 PDF](../tiny210-book/main.pdf)。

Tiny210 专题包括：

- [启动与内存初始化](tiny210/boot.md)
- [异常、中断与系统时钟](tiny210/interrupts.md)
- [LCD/FIMD 与 framebuffer 控制台](tiny210/lcd.md)
- [MT7601U USB Wi-Fi、WPA2 与网络接入](tiny210/wifi.md)
- [根文件系统、bcache 与日志](tiny210/filesystem.md)
- [设备模型、板载 K1--K8 输入子系统、initcall 与驱动开发](tiny210/drivers.md)
- [上板调试记录](tiny210/bringup-notes.md)

这些文档描述的是当前 `tiny210` 分支实际编译进镜像的代码。烧录命令和
SD 卡布局仍以 [ports/tiny210/README.md](../../ports/tiny210/README.md) 为操作手册。

