# xv6 内置裁剪版 C 编译器

`tcc` 是面向本项目的教学型 AArch64 C 编译器。它在 xv6 用户态运行，并直接生成 xv6 `exec()` 能加载的 ELF64 文件；运行时不需要汇编器、链接器或宿主机工具链。

## 使用

```text
$ edit hello.c
int main() {
  int answer = 6 * 7;
  printf("hello from xv6 C\n");
  puts("compiler works");
  return answer - 42;
}
<Ctrl-D>

$ tcc hello.c -o hello
tcc: wrote hello (... bytes code, ... bytes data)
$ hello
hello from xv6 C
compiler works
```

省略 `-o` 时输出文件名是 `a.out`：

```text
$ tcc hello.c
$ a.out
```

## 当前支持的语法

- `int main()` 或 `int main(void)` 外形的入口函数
- 整数局部变量声明及初始化
- 变量赋值
- 十进制整数、括号、一元正负号
- `+`、`-`、`*`、`/`
- `return` 整数表达式
- `printf("literal")`，当前不解析格式参数
- `puts("literal")`，自动添加换行
- `//` 和 `/* ... */` 注释
- 字符串中的 `\n`、`\r`、`\t`、`\\` 和 `\"`

它暂不支持预处理器、头文件、函数定义/调用、指针、数组、结构体、条件语句、循环、浮点数以及完整 libc。这是有意保留的小型教学子集，不等同于完整 TinyCC。

## 生成文件的结构

编译器直接写出一个 ELF64 文件：

```text
ELF header
  -> AArch64, entry VA 0
Program header
  -> 单个 RWX PT_LOAD，file offset 0x78，load VA 0
AArch64 machine code
String literal pool
```

生成代码使用 `x29` 指向 256 字节局部变量区，表达式结果放在 `x0`。输出和退出通过 xv6 ABI 直接执行 `svc #0`：`SYS_write=16`、`SYS_exit=2`。字符串地址使用 AArch64 `ADR` 指令，因此生成程序无需重定位。

实现位于 `user/tcc.c`，程序通过 Makefile 的 `UPROGS` 自动链接并打包进 `fs.img`。
