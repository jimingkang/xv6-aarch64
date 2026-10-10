# 从裸机到操作系统：基于 xv6 的简单实现

ElegantBook 模板的 LaTeX 项目，内容整理自 xv6-aarch64 仓库的 readme 文档与源码。

## 导入 texpage.com

1. 新建项目 → 上传/导入 zip，选择 `xv6-book.zip`。
2. 项目设置：**编译器选 XeLaTeX**，主文件 `main.tex`，参考文献工具选 **Biber**。
3. 编译。若平台没有自动运行 biber，参考文献会显示为空，按“XeLaTeX → Biber → XeLaTeX → XeLaTeX”手动编译一次即可。

## 文件

- `main.tex`：导言区（封面信息、代码样式、TikZ 样式）与章节组织
- `chapters/01-preface.tex` … `08-userland.tex`：八章正文
- `elegantbook.cls`：ElegantBook 模板（v4.7）
- `reference.bib`：参考文献
- `figure/cover.jpg`、`figure/logo.png`：封面与标志
- `latexmkrc`：本地 `latexmk` 编译设置

## 写作约定

- 代码环境：`ccode`（C）、`asmcode`（AArch64 汇编）、`shcode`（终端）、`makecode`（Makefile）、`txtcode`（纯文本/示意）
- 行内：`\code{}` 代码、`\file{}` 文件路径、`\reg{}` 系统寄存器（下划线需写成 `\_`）

## 常见错误

`Critical Package ctex Error: CTeX fontset 'fandol' is unavailable in current mode`

说明平台用的是 **pdfLaTeX**。Fandol 中文字体只能在 XeLaTeX/LuaLaTeX 下使用，请在项目设置里把编译器改成 **XeLaTeX** 后重新编译。
（`main.tex` 开头有检查：误用 pdfLaTeX 时会直接提示改用 XeLaTeX。）
