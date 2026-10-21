fs-trace · xv6-aarch64 · 启动日志分析

# xv6 启动文件系统追踪

从一次启动的 `fs-trace` 日志重建出每个文件系统的 superblock、目录项和 inode，以及每次路径查找怎样从 inode 走到目录项、再走到数据块。图里的数值都来自日志；日志没有打印、由上下文推出的值标为“推断”。

[挂载与命名空间](#ns)[各文件系统](#fs)[路径查找图](#walk)[从日志看到的问题](#notes)

## 挂载与命名空间

启动结束时，`/` 是 dev 1（mmcblk0p2 上的 xv6fs），rootfs（dev 7）被压在下面。其余五个文件系统各盖住根文件系统里的一个空目录；虚线箭头上的 `1:54` 这类编号是被盖住的目录 inode。

## 各文件系统：superblock、目录项与 inode

xv6fs 有磁盘 superblock（`fill_super` 读出的块 1）和内存里的 VFS `super_block`；ext2、procfs、fat32、netfs 在日志里只有 VFS `super_block`。块区间按 superblock 字段计算：inode 区 = `ninodes/16+1` 块，位图 = `size/8192+1` 块，其后是数据区。

## 路径查找图：inode → 目录项 → 数据块

选一条路径。每一行左边是当前目录的 inode，右边是它的目录数据块；箭头从 `addrs[i]` 指到磁盘块，再从命中的目录项指到下一级 inode。最后一行是目标文件的 inode 与它的数据块，样式与 ext2 inode 的块映射图相同。

命中的目录项本次新建的目录项查找失败无意义的残留值follow_mount 穿过挂载点

## 从日志看到的问题

## 三个文件系统：rootfs（dev 7）、devtmpfs（dev 8）、根文件系统（dev 1）

三者是三个不同的文件系统：rootfs、devtmpfs 是内核启动时用 `ramfs_mkfs()` 在内存盘 ram0、ram1 上现做的，重启即消失；
根文件系统是 `mkfs` 预先做好、烧在 SD 卡 p2 上的 `fs.img`。它们只是共用同一种磁盘格式（xv6fs）和函数表 `xv6_iops`。

| 字段 | rootfs（dev 7） | devtmpfs（dev 8） | 根文件系统（dev 1） |
|---|---:|---:|---:|
| magic | 10203040 | 10203040 | 10203040 |
| size | 32 | 64 | 32768 |
| nblocks | 27 | 57 | 32718 |
| ninodes | 16 | 48 | 200 |
| nlog | 0 | 0 | 30 |
| logstart | 2 | 2 | 2 |
| inodestart | 2 | 2 | 32 |
| bmapstart | 4 | 6 | 45 |
| 来源 | ram0 | ram1 | mmcblk0p2 |
| 日志行号 | 1 | 215 | 2182 |

块区间（inode 区 = `ninodes/16+1` 块，位图 = `size/8192+1` 块）：

| | boot | super | 日志 | inode 区 | 位图 | 数据区 |
|---|---|---|---|---|---|---|
| rootfs（dev 7） | 0 | 1 | — | 2–3 | 4 | 5–31 |
| devtmpfs（dev 8） | 0 | 1 | — | 2–5 | 6 | 7–63 |
| 根文件系统（dev 1） | 0 | 1 | 2–31 | 32–44 | 45–49 | 50–32767 |

数据区第一块 5、7、50 正好是三个根目录 inode 的 `addrs[0]`。

**rootfs（dev 7）**，inode 都在块 2：

```text
/          ino 1   块 5    目录项：dev -> 2（off 64）  root -> 4（off 96）
  dev/     ino 2   块 6    目录项：console -> 3（off 64）  root -> 5（off 96）
    console  ino 3   T_DEVICE   第 1 步建出的 /dev/console
    root     ino 5   T_DEVICE   第 3 步建出的 /dev/root，次设备号 1
  root/    ino 4   块 7    只有 . 和 ..，后来被根文件系统盖住
```

**devtmpfs（dev 8）**：14 个 inode 都在块 2；根目录块 7 有 14 项（console、tty、ttyS0、input、video0、sdroot、mmcblk0、
mmcblk0p1–p3、ram0、ram1，共 448 字节），`input` 是子目录（ino 5，块 8，只有 event0）。建节点时根目录 8:1 被重新从块 2 读了 12 次：
每建完一个节点引用降到 0，xv6 的 inode 表不保留 ref==0 的项。

**根文件系统（dev 1）**，只列日志读到的：inode 块 32、33、35、36；`/`（块 50，11 项）、`/bin`（块 51，25 项）、`/mnt`（1660）、
`/etc`（1663）、`/usr`（1665）；`/init` 与 `/bin/init` 都是 ino 7（硬链接，数据块 130–134）；`/bin/login` 块 199–202，
`/bin/wifi` 块 533–536，`/etc/fstab`（119 B）块 1670，`/etc/wifi.conf`（33 B）块 1671。ino 54/55/56/58/59 是被 devtmpfs、procfs、
fat32、ext2、netfs 盖住的空目录。

## inode 号、槽号、目录项偏移与 dinode 的 64 字节

以 rootfs（dev 7）为例，回答几个容易混淆的问题。

### 根目录为什么是 ino 1，`/dev` 为什么是 ino 2

`fs.h` 里 `#define ROOTINO 1`。inode 号 0 不用，因为目录项里 `inum == 0` 表示空槽，所以根目录取第一个可用号 1：
`mkfs`、`ramfs_mkfs()` 都把根写在 ino 1，内核用 `iget(dev, ROOTINO)` 找根，xv6fs 的 `super_block.rootino` 也是 1。
`ialloc()` 从 1 往上找第一个空闲 dinode，所以之后的号按创建顺序发：

| 顺序 | 调用 | inode |
|---|---|---|
| 1 | `ramfs_mkfs()` | ino 1 = `/` |
| 2 | `kmknod("/dev")` | ino 2 |
| 3 | `kmknod("/dev/console")` | ino 3 |
| 4 | `kmknod("/root")` | ino 4 |
| 5 | 第 3 步 `kmknod("/dev/root", BLOCKDEV, 1)` | ino 5 |

ext2 不同：inode 1 留给坏块表，根是 ino 2（`EXT2_ROOT_INO`）；procfs、fat32、netfs 的根是 `"/"` 的 31 位 FNV 哈希 705468254。
所以 VFS 的 `isfsroot()` 比较的是各自 `super_block` 里的 `rootino`，而不是写死的 1。

### inode = 编号 + 磁盘上的 dinode + 内存里的 inode

| | 是什么 | 在哪 |
|---|---|---|
| inode 号 `1` | 编号 | 写在目录项里，`iget` 的参数 |
| `struct dinode` | 磁盘上的本体，64 字节 | 块 2 第 1 个槽 |
| `struct inode` | 内存副本，多了 dev、ref、锁、`iop` | `itable[]` 的一格 |

dinode 不存自己的编号，编号由位置推出：

```text
所在块 = inodestart + ino / 16      槽号 = ino % 16      块内字节 = 槽号 × 64
```

`#0` 槽对应 ino 0，不用。inode 区从哪一块开始看 `inodestart`：rootfs、devtmpfs 是块 2（前面只有 boot 和 superblock），
SD 卡上的根文件系统是块 32（块 2–31 是 30 块日志）。日志里 `xv6_read_inode: dev=1 ino=10 disk-block=32 slot-in-block=10` 就是这个公式。

`iget(7, 1)` 只在 `itable` 里记下 dev=7、inum=1、valid=0，不读盘；`ilock` 发现 valid==0 才调用 `xv6_read_inode()`，
按公式读块 2、取偏移 64 处的 64 字节，拷进内存 inode。

### dinode 的 64 字节：`addrs[0]=5` 存在哪里

```c
struct dinode {            // kernel/fs.h
  short type;              // 2 B
  short major;             // 2 B
  short minor;             // 2 B
  short nlink;             // 2 B
  uint  size;              // 4 B
  uint  addrs[NDIRECT+1];  // 13 × 4 = 52 B：addrs[0..11] 直接块，addrs[12] 一级间接块
};                         // 64 B，IPB = 1024 / 64 = 16
```

rootfs 的 ino 1 在块 2 的偏移 64–127（启动结束时 size = 128，即 4 个目录项）。nlink = 3：`ramfs_mkfs()` 做好时为 1（日志第 23 行的 dump 打印于建 `/dev` 之前，那时 size 还是 64），`xv6_create()` 每建一个子目录给父目录 `nlink++`（子目录的 `..` 指向它），建了 `/dev`、`/root`，所以 1+2=3：

| dinode 内偏移 | 块 2 内偏移 | 字段 | 值 | 小端字节 |
|---:|---:|---|---|---|
| 0 | 64 | type | 1（T_DIR） | `01 00` |
| 2 | 66 | major | 0 | `00 00` |
| 4 | 68 | minor | 0 | `00 00` |
| 6 | 70 | nlink | 3 | `03 00` |
| 8 | 72 | size | 128 | `80 00 00 00` |
| **12** | **76** | **addrs[0]** | **5** | **`05 00 00 00`** |
| 16 | 80 | addrs[1] | 0 | `00 00 00 00` |
| … | … | … | 0 | … |
| 60 | 124 | addrs[12] | 0（无间接块） | `00 00 00 00` |

```text
块2+64:  01 00 00 00 00 00 03 00  80 00 00 00 05 00 00 00
块2+80:  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00
块2+96:  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00
块2+112: 00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00
```

`addrs[0]=5` 在块 2 内第 76–79 字节（整个 ram0 的第 2×1024+76 = 2124 字节）。`ilock` 之后它还有一份拷在内存 inode 里，
日志 `root inode loaded` 下面打印的、`bmap` 用的都是内存那一份。

### inode 与数据块：根目录由两部分组成

| | 在哪 | 内容 |
|---|---|---|
| ino 1 | inode 区，块 2 槽 1，64 B | 关于目录的信息：type、nlink、size、`addrs[0]=5` |
| 块 5 | 数据区，1024 B | 目录的内容：目录项表 `.`、`..`、`dev`、`root` |

“inode 1 是根”说的是身份；“块 5 · 目录 /”说的是 ino 1 的 `addrs[0]` 指向的内容块。普通文件也一样，只是目录的内容恰好是“名字 → inode 号”的表。

### `.` 与 `..`；目录项偏移 off 与槽号 # 是两套编号

每个目录创建时先写 `.`（指自己）和 `..`（指父目录）。`/dev` 的块 6：

```text
off 0   .        → ino 2    自己
off 32  ..       → ino 1    父目录 /
off 64  console  → ino 3
off 96  root     → ino 5
```

根目录的 `..` 指回自己（ino 1）。日志里建 `/dev` 时的 `dirlookup: miss name='.'`、`miss name='..'` 就是在写这两项前先确认不存在。

`off N` 是目录项在目录数据块里的位置（每项 32 B，按写入顺序）；`#N` 是 dinode 在 inode 块里的槽号（由 inode 号算出）。
两者靠目录项里写的 inode 号连起来，顺序并不一致：

| 目录项 | 写着的 inode 号 | inode 区位置 |
|---|---|---|
| 块 6 off 0 `.` | 2 | 块 2 `#2` |
| 块 6 off 32 `..` | 1 | 块 2 `#1` |
| 块 6 off 64 `console` | 3 | 块 2 `#3` |
| 块 6 off 96 `root` | 5 | 块 2 `#5` |

### 两个同名的 `root`

| 目录项所在 | 路径 | inode | 类型 | 谁建的 |
|---|---|---|---|---|
| 块 5（`/`）off 96 | `/root` | ino 4 | T_DIR | 第 1 步，挂根文件系统的挂载点 |
| 块 6（`/dev`）off 96 | `/dev/root` | ino 5 | T_DEVICE | 第 3 步 `kmknod("/dev/root", BLOCKDEV, 1)` |

从 ino 1 到 ino 5 要两层：ino 1 → `addrs[0]` → 块 5 → `dev` → ino 2 → `addrs[0]` → 块 6 → `root` → ino 5，
即日志第 2127 行 `namex: begin path='/dev/root'` 的走法。inode 里只有指向数据块的 `addrs[]`，指向 inode 的是目录项。

### rootfs 块 2 的全部 16 个槽（`prepare_namespace()` 结束时）

inode 区不按目录分组，是全文件系统一张平铺的表：dinode 放哪一块只看 inode 号。rootfs 的 `ninodes` = 16，`ialloc()` 只发 ino 1–15，
所以整个 rootfs 的对象（不论在第几层目录）都在块 2；块 3 对应 ino 16–31，不存在，一直为空。层级关系只记在目录项里，dinode 没有“父目录”字段。

| 槽 | 块内偏移 | inode | 路径 | type | major | minor | nlink | size | addrs[0] |
|---|---|---|---|---|---|---|---|---|---|
| #0 | 0–63 | ino 0 | 不用 | 0 | 0 | 0 | 0 | 0 | 0 |
| #1 | 64–127 | ino 1 | `/` | 1 T_DIR | 0 | 0 | 3 | 128 | 5 |
| #2 | 128–191 | ino 2 | `/dev` | 1 T_DIR | 0 | 0 | 1 | 128 | 6 |
| #3 | 192–255 | ino 3 | `/dev/console` | 3 T_DEVICE | 1 CONSOLE | 0 | 1 | 0 | 0 |
| #4 | 256–319 | ino 4 | `/root` | 1 T_DIR | 0 | 0 | 1 | 64 | 7 |
| #5 | 320–383 | ino 5 | `/dev/root` | 3 T_DEVICE | 7 BLOCKDEV | 1 | 1 | 0 | 0 |
| #6–#15 | 384–1023 | ino 6–15 | 空闲（type 0） | 0 | 0 | 0 | 0 | 0 | 0 |

- type = 0 表示空闲，`ialloc()` 找的就是这样的槽。
- 设备节点只有 major/minor：`/dev/console` 由 `kmknod("/dev/console", T_DEVICE, CONSOLE, 0)` 建，major 1 是控制台驱动；
  `/dev/root` 由 `kmknod("/dev/root", T_DEVICE, BLOCKDEV, 1)` 建，major 7 = 块设备，minor 1 = 块设备表里的 dev 1。size 0，没有数据块。
- 目录的 size = 目录项数 × 32；内容块 5、6、7 由各自的 `addrs[0]` 指向。
