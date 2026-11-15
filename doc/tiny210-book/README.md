# Tiny210 / ARM32 LaTeX 书稿

这是独立于 Raspberry Pi 3/AArch64 `xv6-book` 的 Tiny210 专题书稿。正文来源于
`doc/readme/tiny210`，章节按 Tiny210 实际启动和驱动依赖顺序组织。

优先使用 Tectonic：

```sh
cd doc/tiny210-book
make
```

安装完整 TeX Live/MacTeX 后也可以使用：

```sh
latexmk main.tex
```

生成文件为 `main.pdf`。修改 Markdown 后先重新生成章节，再编译：

```sh
make chapters
make
```

`chapters/09-references.tex` 是人工维护的参考资料章节，不会被转换脚本覆盖。

`chapters/05a-wifi-arch-hw.tex`、`05b-usb-stack.tex`、`05c-wifi-protocol.tex` 也是人工维护的，
不在 `tools/convert.sh` 里，`make chapters` 不会覆盖它们。它们是 MT7601U Wi-Fi 驱动的架构详解
（硬件连接、EHCI 数据传输与中断处理、802.11/WPA2 实现），结合了 rpi3-lock 分支 xv6-book 的
MT7601U 章节，内容和 `doc/readme/tiny210/wifi-architecture.md` 同步。为避免与本书的 `box`/`arr`/`darr`/`lab`
样式冲突，这三章用的 TikZ 样式名都带 `w` 前缀（定义在 05a 开头）。
