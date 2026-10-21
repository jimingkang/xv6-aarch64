# libc.so 的动态链接与加载

本文从头到尾讲一个 xv6 用户程序如何用上共享的 `/lib/libc.so`：
1. 怎么构建；
2. ELF 文件里记录了哪些动态链接信息；
3. 一次 `strlen()` 调用怎样经过 PLT 和 GOT 跳进 libc；
4. 内核与 `/lib/ld.so` 怎样把它们装进内存、填好地址；
5. 多个进程怎样共用同一份 libc 物理页；
6. 出问题时怎么查。

文中的反汇编和 `readelf` 输出取自用 Ubuntu `aarch64-linux-gnu` 工具链构建的 QEMU 版本，代码在分支 `wip-cow-mmap-ldso`。在板子上用 Homebrew 的 `aarch64-elf` 工具链构建时，地址和大小会略有不同，结构完全一样。

相关文档：`readme_process_trap.md` 第二部分（写时复制、页缓存、mmap），书第 4.7 节和第 8.5 节。

---

## 0. 全景

```
          构建期                                   运行期
 ┌──────────────────────┐        exec("/bin/echo")
 │ ulib.c printf.c ...  │──-fPIC──► *.pic.o ──ld -shared──► /lib/libc.so (ET_DYN)
 │ echo.c               │──────────► echo.o ──ld -pie -dynamic-linker /lib/ld.so──► /bin/echo (ET_DYN, PT_INTERP)
 │ ldso.c ldso_start.S  │──-fPIC───► ld -shared -Bsymbolic -z text ──► /lib/ld.so
 └──────────────────────┘

 exec ─► 内核: 主程序装到 VA 0；读 PT_INTERP；ld.so 装在主程序之后；x0=argc x1=argv x2=auxv；从 ld.so 入口开始
      ─► ld.so: 自重定位 → 由 AT_PHDR 找到主程序的 PT_DYNAMIC → DT_NEEDED "libc.so"
               → open("/lib/libc.so") + mmap 映射到 0x10_0000_1000 一带
               → 重定位 libc 和主程序（填 GOT）→ 调用 AT_ENTRY(main)，再 exit(返回值)
      ─► main: bl strlen@plt → ldr x17, GOT[strlen] → br x17 → libc 的 strlen
```

## 1. 构建

```make
# libc 的每个源文件编成位置无关对象
$U/%.pic.o: $U/%.c
	$(CC) $(CFLAGS) -fPIC -c -o $@ $<

# 共享库：ET_DYN，SONAME = libc.so；-z text 保证没有文本重定位，代码页才能共享
$U/libc.so: $(LIBC_PIC_OBJS)
	$(LD) $(LDFLAGS) -shared --no-undefined --hash-style=sysv -z text -soname libc.so -o $@ $^

# 用户程序：PIE，并在 PT_INTERP 里写上动态链接器的路径
USER_LD_MODE = -pie -dynamic-linker /lib/ld.so --hash-style=sysv
_%: %.o $(ULIB) $(USER_LINK_STAMP)
	$(LD) $(LDFLAGS) $(USER_LD_MODE) -e main -o $@ ...

# 动态链接器本身：独立、全部符号 hidden、不允许文本重定位
$U/ld.so: $U/ldso_start.o $U/ldso.o
	$(LD) $(LDFLAGS) -shared -Bsymbolic --no-undefined -z text --hash-style=sysv \
	  -e _start -soname ld.so -o $@ $^
```

- **`--hash-style=sysv`**：生成 `DT_HASH`。内核加载器和 ld.so 都用 SysV 哈希查符号，不支持 GNU hash。
- **`-e main`**：程序入口就是 `main`。ld.so 以 `(argc, argv)` 调用它，然后用返回值调用 `exit`；所以 xv6 程序既可以 `exit()`，也可以直接 `return`。
- **模式戳 `.dynamic-ldso`**：链接方式改变时，旧二进制必须重新链接。换一个戳的名字就能强制所有程序重链。
- **静态回退**：`make DYNAMIC_USER=0` 退回静态 `libc.a`，生成 `ET_EXEC` 程序（`-N -Ttext 0`）。
- **安装**：`mkfs fs.img -L user/libc.so -L user/ld.so ...` 把两个共享对象放进 `/lib`；ext2 根镜像（`make ext2root-image`）也会写入这两个文件。

## 2. ELF 里的动态链接信息（以 `/bin/echo` 为例）

### 2.1 程序头

```
$ readelf -hlW user/_echo
  Type:          DYN (Position-Independent Executable file)
  Entry point:   0x320                                  ← main
  PHDR      0x000040  vaddr 0x40     0x188   R          ← 程序头表自身，ld.so 由它算出主程序基址
  INTERP    0x0001c8  vaddr 0x1c8    0x00b   R          ← "/lib/ld.so\0"
  LOAD      0x000000  vaddr 0x0      0x3ec   R E        ← 代码：ELF 头、PLT、.text、只读数据
  LOAD      0x000ec0  vaddr 0x1ec0   0x158   RW         ← .dynamic、GOT
  DYNAMIC   0x000ec0  vaddr 0x1ec0   0x120   RW
```

### 2.2 动态段 `.dynamic`

| 标签 | 值 | 含义 |
|---|---|---|
| `DT_NEEDED` | "libc.so" | 依赖的库（字符串表里的偏移） |
| `DT_HASH` | 0x1d8 | SysV 哈希表：nbucket、nchain、bucket[]、chain[] |
| `DT_SYMTAB` / `DT_STRTAB` / `DT_STRSZ` / `DT_SYMENT` | 0x200 / 0x260 / 27 / 24 | 动态符号表和它的字符串表 |
| `DT_PLTGOT` | 0x1fe8 | `.got.plt` 的地址 |
| `DT_JMPREL` / `DT_PLTRELSZ` / `DT_PLTREL` | 0x280 / 72 / RELA | PLT 的重定位表（3 条 × 24 字节） |
| `DT_FLAGS_1` | PIE | |

### 2.3 动态符号与重定位

```
$ readelf --dyn-syms -rW user/_echo
.dynsym: 1: write  FUNC GLOBAL UND      ← 未定义：要到 libc 里找
         2: exit   FUNC GLOBAL UND
         3: strlen FUNC GLOBAL UND
.rela.plt:
  Offset   Type                  Symbol
  0x2000   R_AARCH64_JUMP_SLOT   write     ← 把 write 的真实地址写到 VA 0x2000
  0x2008   R_AARCH64_JUMP_SLOT   exit
  0x2010   R_AARCH64_JUMP_SLOT   strlen
```

### 2.4 libc.so 的布局

```
$ readelf -SW user/libc.so
 .hash 0x158 | .dynsym 0x7d0 | .dynstr 0x1be0 | .rela.plt 0x25b8 (71 条)
 .plt 0x2c60 | .text 0x30f0 | .rodata 0x8e80                   ← 第一个 LOAD（R-X）
 .dynamic 0xcee0 | .got 0xcfe0 | .got.plt 0xcfe8 | .bss 0xd238  ← 第二个 LOAD（RW）
符号：write = 0x3214, strlen = 0x4260, exit = 0x4118, printf = 0x4fa4, __syscall = 0x4a3c
```

libc 的 71 条重定位**全部**是 `JUMP_SLOT`，目标都在 RW 段的 `.got.plt` 里，代码段一个字节都不用改。这就是代码页能在进程间共享的前提（第 6 节）。

## 3. 一次库函数调用：PLT 与 GOT

`echo` 的 `main` 调用 `strlen` 时，编译出来是一条普通的 `bl`，但目标不是 libc，而是本程序里一段 16 字节的桩代码（PLT 项）：

```
0000000000000320 <main>:
 ...
 368:  bl   310 <strlen@plt>          ← 调用 PLT 项，而不是 libc

0000000000000310 <strlen@plt>:
 310:  adrp x16, 2000                 ← GOT 所在页（PC 相对，与装载地址无关）
 314:  ldr  x17, [x16, #16]           ← 读 GOT[0x2010]：strlen 的真实地址
 318:  add  x16, x16, #0x10
 31c:  br   x17                       ← 跳过去
```

GOT（`.got.plt`）是数据，位于可写段。构建出来时它的内容是：

```
$ objdump -s -j .got.plt user/_echo
 1fe8 00000000 00000000 00000000 00000000   ← 保留项（延迟绑定用，此处不用）
 1ff8 00000000 00000000 d0020000 00000000
 2008 d0020000 00000000 d0020000 00000000   ← 0x2000/0x2008/0x2010 都指向 0x2d0（PLT0）
```

3 个槽的初值都是 `0x2d0`，即 PLT0，也就是 glibc 那样做"延迟绑定"时的解析入口。这里**不做延迟绑定**：PLT0 要用的保留槽是 0，真跳过去会崩溃。所以必须在 `main` 运行之前把这些槽改写成真实地址，这一步叫**急切绑定（eager binding）**。

ld.so 处理 `R_AARCH64_JUMP_SLOT strlen @ 0x2010`：
1. 在全局作用域（程序 → libc）查到 libc 的 `strlen`，`st_value = 0x4260`；
2. libc 被 mmap 到基址 `0x10_0000_1000`（第 5 节），所以写入 `*(0x2010) = 0x10_0000_1000 + 0x4260 = 0x10_0000_5260`。

之后 `bl strlen@plt` 就总是经过两次跳转到达 libc。代价是每次库调用多一次 `ldr` 和一次 `br`，换来的是：代码里没有任何绝对地址，代码页可以原样共享。

**libc 内部也走 PLT。** libc 以 `-shared` 链接，没有加 `-Bsymbolic`，它内部对导出函数的调用同样经过自己的 PLT：

```
0000000000004b14 <putc>:
 4b28:  bl  2e20 <write@plt>          ← libc 调用自己的 write 也走 PLT/GOT
```

所以 libc 有 71 条 `JUMP_SLOT`。好处是程序可以**覆盖（interpose）**libc 的函数：符号查找先找程序再找 libc，如果程序自己定义了 `malloc`，libc 内部对 `malloc` 的调用也会被解析到程序的版本。

## 4. 两条装载路径

| | A. PT_INTERP → `/lib/ld.so`（默认） | B. 内核内置加载器（兼容路径） |
|---|---|---|
| 适用 | 带 `PT_INTERP` 的程序（`-dynamic-linker /lib/ld.so` 链接） | `ET_DYN`、有 `DT_NEEDED` 但没有 `PT_INTERP`（旧的 `--no-dynamic-linker` 链接） |
| libc 放在哪 | mmap 区（`0x10_0000_0000` 起），由 ld.so 映射 | 紧跟主程序之后，`PGROUNDUP(sz)` |
| 谁做重定位 | 用户态 ld.so | 内核 `relocateimage()` |
| 依赖 | 多个库，到 `/lib` 下搜索 | 只能是一个 `/lib/libc.so` |
| 日志 | 无 | `exec: dynamic /bin/x, libc.so base=…, text shared` |

### 4.1 路径 A：内核这一半（`kernel/fs/exec.c`）

1. 校验 ELF 头（`ET_EXEC` 或 `ET_DYN`，AArch64），`loadimage()` 把主程序的 PT_LOAD 装到 VA 0，并记下 `PT_DYNAMIC`、`PT_INTERP`、`PT_PHDR`。
2. 读出解释器路径（必须以 NUL 结尾、长度不超过 128）。
3. 打开 `/lib/ld.so`，`ET_DYN`，装在 `interpbase = PGROUNDUP(sz)`。它的只读页用 `shlib_map()` 从页缓存映射，所有进程共用。
4. 建用户栈：参数字符串、argv 数组，再压辅助向量：

```
x0 = argc   x1 = argv   x2 = auxv（{tag, value} 对，以 AT_NULL 结束）
AT_PHDR    = 主程序程序头地址（0x40）   AT_PHENT = 56   AT_PHNUM = 程序头个数
AT_PAGESZ  = 4096                       AT_BASE  = interpbase   AT_ENTRY = 0x320（main）
```

5. `elr = interpbase + ld.so 的 e_entry`，`eret` 后从 ld.so 的 `_start` 开始执行。

### 4.2 路径 A：ld.so 这一半（`user/ldso.c`）

```
_start (ldso_start.S): 清 x29/x30，bl ldso_main(argc, argv, auxv)
ldso_main:
  1. 解析 auxv 到 at[]
  2. 自重定位：遍历自己的 _DYNAMIC，处理 RELATIVE（实际构建出的 ld.so 一条都没有）
  3. 主程序：从 AT_PHDR 找 PT_PHDR → mainbase = AT_PHDR - phdr.vaddr（= 0）
             → PT_DYNAMIC → parse_dynamic(): NEEDED/HASH/STRTAB/SYMTAB/RELA/JMPREL
  4. 广度优先装载 DT_NEEDED：load_library("libc.so")
  5. 先库后程序做重定位：relocate_table(rela)、relocate_table(jmprel)
  6. status = ((int(*)(int,char**))AT_ENTRY)(argc, argv);  exit(status)
```

`load_library()` 用 mmap 装库：

```
fd = open("/lib/libc.so")
eh = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0)        ← 读 ELF 头和程序头
span = 所有 PT_LOAD 覆盖的页范围（libc：0 .. 0xe000）
base = mmap(0, span, R|X, MAP_PRIVATE, fd, 0) - minv      ← 一次映射整个镜像，决定基址
对其余 PT_LOAD（libc 的 RW 段：vaddr 0xcee0, off 0xbee0）：
  mmap(base+0xc000, 0x2000, R|W, MAP_PRIVATE|MAP_FIXED, fd, 0xb000)   ← 两页：0xc000 与 0xd000
  .bss：把最后一个文件页的尾部清零；还不够的部分用 MAP_ANONYMOUS|MAP_FIXED 补上
munmap(eh); close(fd); parse_dynamic(); 库带 DT_TEXTREL 则拒绝
```

为什么能这样映射：GNU ld 保证 `p_vaddr ≡ p_offset (mod 页大小)`，所以每一页虚拟地址都对应文件里的一个整页偏移。

### 4.3 路径 B：内核内置加载器

主程序装完后：
1. `parsedynamic()` 读取 DT_*，要求 `DT_NEEDED` 恰好一个且就是 `"libc.so"`；
2. 打开 `/lib/libc.so`，`libbase = PGROUNDUP(sz)`；
3. `shlib_textsize()` 求出可共享的只读部分，`shlib_map()` 从页缓存映射它；其余页由 `loadimage()` 分配私有副本；
4. `relocateimage()` 先重定位 libc，再重定位主程序。写入代码区的重定位会被拒绝；
5. `elr = main` 的地址。

这条路径主要用来兼容旧方式链接的程序。

## 5. 一次 exec 后的地址空间（QEMU，`/bin/sh`）

```
0x0000_0000 ┌───────────────────────┐
            │ sh 代码 + 数据（私有） │  主程序：exec 装入，ld.so 改写它的 GOT
            ├───────────────────────┤  PGROUNDUP(sz)
            │ ld.so 代码（共享）     │  页缓存页，只读可执行
            │ ld.so 数据（私有）     │
            ├───────────────────────┤
            │ 保护页 + 4 页用户栈    │
            ├───────────────────────┤  sz
            │ sbrk 堆 ...            │
            ⋮                       ⋮
0x10_0000_1000 ┌────────────────────┐
            │ libc 代码 12 页（共享）│  页缓存页，R-X，每页 "shared by 4 mappings"
            │ libc 0xb000 页（共享） │  同一文件页：代码尾 R-X + .data 开头 R+COW
            │ libc GOT 页（私有）    │  ld.so 写过 → 已经 COW 出私有副本
            └────────────────────┘
```

可以用 `vmmap` 查看：每一页显示 `user RO X`、`cow`、`shared by N mappings`。

## 6. 为什么只有一份 libc 物理页

1. **libc 的代码不需要改**：PIC 代码通过 `adrp`/`add` 等 PC 相对指令访问数据，库函数调用经过 PLT，从 GOT 读地址；所有重定位都在 RW 段。
2. **页缓存（`kernel/pcache.c`）**：以"文件 + 页号"为键缓存物理页，缓存本身持有一份引用。mmap 一个文件页时，只是给这个物理页的引用计数加一，再映射进页表。
3. **写时复制**：RW 段以只读 + `PTE_COW` 映射。ld.so 改写 GOT 时触发写保护异常，内核只复制被写的那一页。
4. **ld.so 自己**的代码页同样来自页缓存。

QEMU 中实测，init、sh、vmmap 三个进程的 libc 代码页是同一组物理地址；`0xb000` 那一页同时被映射成代码尾和数据头，被引用 7 次；GOT 页各有私有副本。

## 7. 支持的重定位类型

| 类型 | 编号 | 计算 | 出现在 |
|---|---|---|---|
| `R_AARCH64_NONE` | 0 | 跳过 | |
| `R_AARCH64_ABS64` | 257 | S + A | 数据中的绝对指针 |
| `R_AARCH64_GLOB_DAT` | 1025 | S + A | `.got`：全局变量的地址 |
| `R_AARCH64_JUMP_SLOT` | 1026 | S + A | `.got.plt`：函数地址（急切绑定） |
| `R_AARCH64_RELATIVE` | 1027 | B + A | 本模块内部的指针（PIE 程序的指针表） |

S 是符号地址，A 是 addend，B 是模块基址。符号查找顺序：本模块的 `STB_LOCAL` 定义，然后全局作用域（程序 → 各库，按装载顺序）。弱符号找不到时按 0 处理，强符号找不到则报错退出。

**不支持**：`DT_REL`（只支持 RELA）、`R_AARCH64_COPY`、TLS 类重定位、`IFUNC`、GNU hash、延迟绑定。

## 8. 调试

### 8.1 常用命令（在主机上）

```sh
aarch64-elf-readelf -lW user/_sh          # 程序头：有没有 INTERP/PHDR，段布局
aarch64-elf-readelf -dW user/_sh          # 动态段：NEEDED、HASH、JMPREL…
aarch64-elf-readelf -rW user/libc.so      # 重定位：类型是否都受支持、目标在哪个段
aarch64-elf-readelf -dW user/libc.so | grep TEXTREL   # 库必须没有
aarch64-elf-objdump -d user/_sh | grep '@plt>'        # 哪些调用走 PLT
aarch64-elf-objdump -s -j .got.plt user/_sh           # GOT 初值
```

### 8.2 错误信息对照

| 信息 | 来自 | 原因 |
|---|---|---|
| `exec: /bin/x: cannot open interpreter /lib/ld.so` | 内核 | 根文件系统里没有 ld.so，fs.img 或 ext2 镜像没装 |
| `ld.so: cannot open /lib/libc.so` | ld.so | 缺库，或 `DT_NEEDED` 写错 |
| `ld.so: undefined symbol foo` | ld.so | 程序用了 libc 没有导出的函数 |
| `ld.so: unsupported relocation type in X` | ld.so | 出现了第 7 节表格以外的类型（例如 `R_AARCH64_COPY`） |
| `ld.so: text relocations are not supported in /lib/X` | ld.so | 库带 `DT_TEXTREL`：有对象没用 `-fPIC` 编译 |
| `ld.so: misaligned segment in X` | ld.so | 段的 vaddr 和 offset 模 4096 不相等 |
| `exec: unresolved dynamic symbol foo` | 内核（路径 B） | 同上，发生在内核加载器里 |
| `exec: relocation into shared text at …` | 内核（路径 B） | 要写进共享代码区的重定位 |
| `usertrap(): … ec=0x0 … elr=0x0` 紧跟 `init exiting` | 内核 | 程序是 ET_DYN，但内核不会动态加载（例如旧版 exec.c），GOT 保持 0，跳到了地址 0 |

### 8.3 判断走的是哪条路径

- 内核日志出现 `exec: dynamic /bin/x, libc.so base=…`：走的是路径 B。
- 没有这行但程序正常运行：走的是路径 A（ld.so）。`vmmap` 里 libc 在 `0x10_0000_xxxx`。
- 静态构建（`DYNAMIC_USER=0`）：程序是 `ET_EXEC`，两条路径都不经过。

## 9. 常见问题

**为什么不同程序里 libc 的基址不一样？**
- 路径 B：基址是"主程序末尾向上取整到页"，比如 login 是 0x2000，sh 是 0x3000。
- 路径 A：基址由 mmap 区当时的空闲位置决定，通常每次都是 `0x10_0000_1000`（第 0 页被 ld.so 读 ELF 头时暂用，之后释放）。

两种情况都不影响共享，因为代码页内容与装载地址无关。

**程序可以有文本重定位，库却不行？**
用户程序以 `-fno-pie` 编译、`-pie` 链接，只读段里的指针表需要 RELATIVE 重定位，所以带 `DT_TEXTREL`（usertests 就是这样）。程序是 exec 装进来的私有可写副本，改了没关系；库的代码页是所有进程共用的只读页，不能改。

**为什么不做延迟绑定？**
延迟绑定需要 PLT0 解析入口和运行时的符号查找服务（glibc 的 `_dl_runtime_resolve`）。xv6 的程序都很小，启动时一次性绑定更简单：一个 `echo` 只需要 3 条，libc 也只有 71 条。

**ld.so 自己怎么做到不需要重定位？**
`-fPIC -fvisibility=hidden` 加 `-Bsymbolic`：所有对自身代码和数据的访问都是 PC 相对的，链接器不需要留下任何 GOT 项。唯一的例外是带指针初值的全局变量，ld.so 刻意避免了这种写法，所以 `readelf -r user/ld.so` 显示没有重定位。

## 10. 与 Linux 的对比

| | xv6-aarch64 | Linux / glibc |
|---|---|---|
| 解释器 | `/lib/ld.so`，约 450 行 C | `ld-linux-aarch64.so.1` |
| 参数传递 | x0/x1/x2 = argc/argv/auxv | 全部在栈上（argc、argv、envp、auxv） |
| auxv | PHDR/PHENT/PHNUM/PAGESZ/BASE/ENTRY | 还有 HWCAP、RANDOM、EXECFN、SYSINFO_EHDR… |
| 绑定 | 急切绑定 | 默认延迟（`-z now` 时急切） |
| 共享方式 | 页缓存 + mmap MAP_PRIVATE + COW | 相同 |
| 地址随机化 | 无 | ASLR |
| 其他 | 无 dlopen、TLS、IFUNC、版本符号 | 都有 |
