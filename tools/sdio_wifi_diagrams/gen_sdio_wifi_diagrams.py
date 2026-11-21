#!/usr/bin/env python3
"""生成 doc/readme/rpi3/sdio_wifi_illustrated.md 用到的 7 张 SVG 图。

用法（仓库根目录）：
    python3 tools/sdio_wifi_diagrams/gen_sdio_wifi_diagrams.py [输出目录]
    node tools/sdio_wifi_diagrams/render_png.js doc/readme/rpi3/images/sdio-wifi-*.svg   # 可选：转 PNG（需要 playwright）
默认输出目录 doc/readme/rpi3/images。图里的函数名、寄存器、常量都取自
arasan_sdio.c / sdio.c / brcmfmac.c / net.c，改代码后请同步改这里。
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fig_arch_hw import fig_arch, fig_hw      # noqa: E402
from fig_proto import fig_proto               # noqa: E402
from fig_calls import fig_calls               # noqa: E402
from fig_seq import fig_seq_init, fig_seq_connect, fig_seq_rxtx  # noqa: E402

FIGS = [('sdio-wifi-arch.svg', fig_arch), ('sdio-wifi-hw.svg', fig_hw), ('sdio-wifi-proto.svg', fig_proto),
        ('sdio-wifi-callchain.svg', fig_calls), ('sdio-wifi-seq-init.svg', fig_seq_init),
        ('sdio-wifi-seq-connect.svg', fig_seq_connect), ('sdio-wifi-seq-rxtx.svg', fig_seq_rxtx)]

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else 'doc/readme/rpi3/images'
    os.makedirs(out, exist_ok=True)
    for name, fn in FIGS:
        fn(os.path.join(out, name))
        print(os.path.join(out, name))
