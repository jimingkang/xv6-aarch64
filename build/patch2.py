import re, sys, shutil, os
B = 'xv6-book/chapters/'
os.makedirs('build/tex-bak2', exist_ok=True)
for f in ('06-filesystem.tex', '09-optional.tex'):
    shutil.copy(B + f, 'build/tex-bak2/' + f)

def rep(s, a, b, cnt=1):
    n = s.count(a)
    if n != cnt:
        sys.exit('count %d != %d for: %s' % (n, cnt, a[:90]))
    return s.replace(a, b)

def between(s, a, b, new, keep_b=True, start=0):
    i = s.index(a, start)
    j = s.index(b, i + len(a))
    return s[:i] + new + (s[j:] if keep_b else s[j + len(b):])

p = B + '06-filesystem.tex'; s = open(p).read()

# ---------- 6.5 根文件系统放在哪里 ----------
s = between(s, '启动时 \\code{rootdev\\_init()} 读 SD 卡第 0 扇区', '\\paragraph{内核命令行与 \\code{root=}.}', r'''启动时 SD 驱动的 \code{sdhost\_probe()} 用 \code{add\_disk()} 把整张卡登记为块设备 2（\code{mmcblk0}），块层随即读第 0 扇区的 MBR，把每个主分区登记为 \code{mmcblk0pN}（块设备 $2+N$）。\code{rootdev\_init()} 自己不登记任何设备，只在这些块设备里选出根文件系统所在的那一个，也就是 Linux 所说的 ROOT\_DEV（表~\ref{tab:rootfs}）。\code{bio.c} 通过 \code{blk\_rw()} 派发 I/O：分区的回调 \code{part\_rw()} 加上分区起点，再交给整卡的 \code{mmc\_rw()}。

''')
s = between(s, '\\paragraph{根块设备不是 FAT32 的一部分.}', '% -----------------------------------------------------------------------', r'''\paragraph{根块设备不是 FAT32 的一部分.} \file{rootdev.c} 不解释任何文件系统格式。它只用 \code{is\_xv6fs()} 读候选分区的第 1 块、核对 xv6 superblock 的魔数，再把 FAT32 启动分区交给 \file{fat32.c} 挂起来，供 \code{/boot} 和固件加载使用。根分区就是块设备本身，例如 \code{mmcblk0p2} 是块设备 4，没有另外包一层“根设备”。它的块里装的始终是 xv6 自己的 superblock、inode 和目录（\ref{sec:xv6fs} 节），读写经 \code{part\_rw()} 和 \code{mmc\_rw()}，不调用任何 FAT32 函数。

\begin{table}[htbp]
  \centering
  \caption{根文件系统的存放方式}
  \label{tab:rootfs}
  \small
  \begin{tabularx}{\linewidth}{@{}lXl@{}}
    \toprule
    方式 & 识别条件与地址换算 & 适用场景 \\
    \midrule
    原始分区 & MBR 中某个主分区的第 1 块是 xv6 superblock；ROOT\_DEV 就是这个分区（如 \code{mmcblk0p2}，块设备 4）；扇区 $=$ 分区起点 $+$ 块号$\times 2$ & 真机 \\
    ext2 分区 & \code{rootfstype=ext2} 时 \code{root=} 指定的分区（如 \code{mmcblk0p3}，块设备 5），挂载时由 \code{ext2\_fill\_super()} 校验（\ref{sec:ext2-root} 节） & 真机 \\
    裸镜像  & 没有 MBR；ROOT\_DEV 是整卡 \code{mmcblk0}（块设备 2）；扇区 $=$ 块号$\times 2$ & QEMU \\
    \bottomrule
  \end{tabularx}
\end{table}

早期还支持把镜像存成 FAT32 根目录里的 \file{FS.IMG} 文件，经 \code{fat32mapsector()} 查块到扇区的映射；这种模式连同那两个映射函数已经删除。

\paragraph{原始分区模式}是真机上的做法：SD 卡分成 4 个区，xv6 文件系统独占一个分区，与 FAT32
启动分区互不干扰。识别时只认 superblock 魔数，MBR 类型只影响自动探测的先后（0x7f 优先；macOS 只能建 \code{0x83}
类型的分区，与 ext2 相同）。启动日志（节选）：

\begin{txtcode}
rootdev: bootfs mmcblk0p1 lba=2048 mounted as FAT32
Kernel command line: root=/dev/mmcblk0p2 rootfstype=xv6fs
rootdev: root=/dev/mmcblk0p2 -> mmcblk0p2
rootdev: xv6 filesystem on mmcblk0p2 start=133120 sectors=81920 size=40 MiB
rootdev: ROOT_DEV = 4 (mmcblk0p2)
blkdev: cache self-test ok (mmcblk0 sector 0: 1 device read, MBR signature, second read hit)
\end{txtcode}

''')

# ---------- 表：嵌入式 Linux 与 xv6 ----------
s = rep(s, "\\code{name\\_to\\_dev\\_t()} & \\code{root\\_param\\_partition()}：\\code{/dev/mmcblk0pN}、\\code{PARTUUID=} \\\\",
        "\\code{name\\_to\\_dev\\_t()} & \\code{name\\_to\\_bdev()}：\\code{/dev/mmcblk0pN}、\\code{mmcblk0pN}、\\code{PARTUUID=} \\\\")
s = rep(s, "\\code{ROOT\\_DEV} & \\code{ROOTDEV}（块设备 1），指向选中的分区 \\\\",
        "\\code{ROOT\\_DEV} & \\code{rootdev\\_dev()}：选中分区自己的块设备号（xv6fs 根 4，ext2 根 5） \\\\\n    \\code{register\\_filesystem()} & 同名：xv6fs、ext2、procfs、sysfs、fat32、netfs 挂进 \\code{file\\_systems} 链表 \\\\")
s = rep(s, "\\code{kmknod(\"/dev/root\", T\\_DEVICE, BLOCKDEV, ROOTDEV)} \\\\",
        "\\code{kmknod(\"/dev/root\", T\\_DEVICE, BLOCKDEV, rootdev\\_dev())} \\\\")
s = rep(s, "\\code{mount\\_root()} & \\code{mount\\_root()}：\\code{fsinit(ROOTDEV)} 读超级块、恢复日志，挂到 \\code{/root} \\\\",
        "\\code{mount\\_root()} & \\code{mount\\_root()}：\\code{vfs\\_get\\_sb(rootfstype, \"/dev/root\")} 经 \\code{get\\_sb}/\\code{fill\\_super} 读超级块、恢复日志，再 \\code{fs\\_mount()} 挂到 \\code{/root} \\\\")
s = re.sub(r'退回自动探测（0x7f、魔数、[^）]*）', '退回自动探测（0x7f 优先，再看 superblock 魔数）', s, count=1)

# ---------- xv6fs 根调用链 ----------
s = between(s, '\\paragraph{xv6fs 根文件系统.}', '\\paragraph{\\code{/dev/root} 是什么.}', r'''\paragraph{xv6fs 根文件系统.} \file{cmdline\_xv6.txt} 写成 \code{root=/dev/mmcblk0p2 rootfstype=xv6fs}（这也是默认情况）：

\begin{txtcode}
main()                                 kernel/init/main.c：只在 CPU 0 上运行，此时
                                       还没有进程，只能用自旋锁，不能睡眠
  binit()                              初始化块缓存：64 个缓冲区，每个一页
  iinit()                              初始化内存 inode 表（itable，50 项）
    loginit()                          提前建立日志锁：阶段二在根文件系统的
                                       日志就绪之前，就要往 RAM 盘里写东西
    fs_mountinit()                     挂载表的锁，表为空；
                                       register_filesystem(&xv6fs_type)
  do_initcalls()                       按（级别，序号）调用各模块的 initcall
    [subsys] blkdev_init()             建立块设备表；登记主设备号 BLOCKDEV，
                                       以后块设备才能以 /dev/mmcblk0p2 这样
                                       的文件出现
    [device] sdhost_probe()            初始化 SD 卡
      add_disk(2, "mmcblk0", 5, mmc_rw)
                                       整张卡登记为块设备 2
        blk_add_partitions()           块层自己读第 0 扇区的 MBR
          add_partition()              主分区 N 登记为块设备 2+N（mmcblk0pN），
                                       回调 part_rw()：加上分区起点后交给 mmc_rw()
    [late]   rootdev_init()            ======== 阶段一 ========
      fat32mount(p1)                   挂上 FAT32 启动分区
      cmdline_init()                   从启动分区读 /cmdline_xv6.txt
      check_rootfstype()               rootfstype=xv6fs：走本图这一支
      name_to_bdev("/dev/mmcblk0p2")
                                  (a)  名字换成块设备 4，相当于 Linux 的
                                       name_to_dev_t()
      is_xv6fs(mmcblk0p2)         (b)  读该分区第 1 块，核对 xv6 超级块的
                                       魔数和 FSSIZE；ROOT_DEV = 4
    [late 5] ext2init()                只登记 ext2 类型，挂载时才读盘
    [late 6] vfsinit()                 登记 procfs、sysfs、fat32、netfs
  userinit()                           造出第一个进程，当前目录暂时为空
  scheduler()                          调度第一个进程，它从 forkret() 开始运行
forkret()                              kernel/core/proc.c：从这里起可以睡眠，
                                       可以使用块缓存
  prepare_namespace()                  ======== 阶段二 ========
    1. ramfs_create(0, 7, "ram0")      分配 32 KiB 内存当作块设备 7，在里面
                                       格式化出空的 xv6 文件系统：rootfs，
                                       再用 xv6fs 的 fill_super 读回超级块
       fs_set_root(rootfs 根目录)      "/" 从此指向 rootfs 的 (7,1)
       kmknod(...)                     在 rootfs 里建 /dev、/dev/console、/root
    2. devtmpfs_init()                 同样建出块设备 8（ram1），按已注册的
                                       驱动和块设备表逐个建设备节点；此时
                                       devtmpfs 还没有挂到任何目录上
    3. kmknod("/dev/root", BLOCKDEV, 4)
                                  (c)  在 rootfs 里建设备节点 /dev/root：
                                       主设备号 7（BLOCKDEV），次设备号 4
       mount_root("/dev/root", "/root")
         vfs_get_sb("xv6fs", "/dev/root", VFS_MOUNT_ROOTLOG)
           get_fs_type("xv6fs")        沿 file_systems 链表找到 xv6fs_type
           get_sb_bdev()
             lookup_bdev("/dev/root")
                                  (d)  namei 到节点，读出次设备号 4
             getsuper(4) == 0          设备 4 还没有超级块
             xv6fs_fill_super(4)       块大小设为 1 KiB
               fsinit(4)               fs_readsuper()：bread(4, 1) -> blk_rw()
                                       -> part_rw() -> mmc_rw()，读 p2 的超级块，
                                       登记 supers[4]：xv6fs、xv6_iops、根 inode 1；
                                       initlog(4)：恢复日志
         fs_mount((7,4), 4)            mtab.m[0]：rootfs 的 /root 被设备 4 的
                                       根目录盖住
       kchdir("/root")                 当前目录进入根文件系统的根目录 (4,1)
       devtmpfs_mount("dev")           devtmpfs 挂到根文件系统自己的 /dev 上，
                                       此时它的路径是 /root/dev
    4. fs_move_mount(".", "/")         MS_MOVE：根文件系统从 /root 挪到 rootfs
                                       的根目录上，devtmpfs 的路径随之变为 /dev
       fs_set_root(当前目录)           chroot(".")："/" 改指根文件系统的
                                       根目录，rootfs 被压在下面
  blkdev_selftest()                    读两次整卡第 0 扇区，检查块缓存命中
  usertrapret()                        回到用户态（6.6.11）
initcode：exec("/init")                ======== 阶段三 ========
                                       从根文件系统里找到 /init 并执行
  /init：mount_fstab()                 按 /etc/fstab 挂 /proc、/sys、/boot、
                                       /mnt/ext2、/mnt/net
\end{txtcode}

''')

s = between(s, '\\paragraph{\\code{/dev/root} 是什么.}', '\\paragraph{ext2 根文件系统.}', r'''\paragraph{\code{/dev/root} 是什么.} 第 3 步里的 \code{mount\_root("/dev/root", "/root")} 很容易被读成“把 \code{/dev/root} 挂到 \code{/root}”，好像挂上去的是 \code{/dev/root} 这个东西本身。其实 \code{/dev/root} 只是 rootfs 里的一个\emph{设备节点}：一个名字加一对设备号（主设备号 \code{BLOCKDEV}，次设备号是块设备表里的编号），它本身既不是文件系统，也不含数据。\code{mount\_root()} 的两个参数，一个说“到哪里找设备”，一个说“挂在哪个目录”，真正的工作由它下面三步完成：\code{lookup\_bdev("/dev/root")} 把名字换成设备号 4；\code{xv6fs\_fill\_super(4)} 从设备 4 读出文件系统的超级块并恢复日志；\code{fs\_mount((7,4), 4)} 才把\emph{这个文件系统的根目录}盖到 rootfs 的 \code{/root} 目录上。命令行 \code{mount /dev/root /root} 也是同样的意思：挂上去的是节点所指设备上的文件系统，而不是节点。

\paragraph{xv6fs 根的 (a)--(d).} 阶段一的 (a) 把 \code{root=} 换成块设备 4（\code{mmcblk0p2}），(b) \code{is\_xv6fs()} 确认那里是 xv6 文件系统，记下 ROOT\_DEV $=$ 4。设备 4 与 p2 的联系更早就有了：SD 驱动探测时，块层的 \code{add\_partition()} 把 p2 登记为块设备 4，回调 \code{part\_rw()} 让“设备 4 的扇区 $s$”等于“卡上扇区 p2 起点 $+\,s$”。阶段二的 (c) 建出节点 \code{/dev/root}，次设备号写 4；(d) \code{lookup\_bdev()} 从节点读出 4，\code{fill\_super} 的每次 \code{bread(4, \ldots)} 都经 \code{part\_rw()} 落到 p2 上。所以设备 4 与 p2 的联系在驱动探测时建立，\code{/dev/root} 这个名字与设备 4 的联系在阶段二的 (c) 才建立。

''')

s = between(s, '注意 xv6 的“设备 1”', '可以把这三个阶段和图', r'''\code{/dev/root} 不是一个单独的设备，它就是 Linux 的 \code{ROOT\_DEV}：“这次启动用作根文件系统的那个块设备”，xv6fs 根时是 4，ext2 根时是 5。早先的实现另外登记过一个“设备 1（root）”包住 p2，读写回调 \code{root\_blk\_rw()} 负责块号换算，\code{/dev/sdroot} 也经它更新根镜像；现在这一层已经删掉，ext2 的外置日志通过 \code{rootdev\_xv6\_part()} 直接取得 p2 的块设备。

''')

# ---------- 阶段一 ----------
s = between(s, '\\subsection{阶段一：\\code{main()} 中登记块设备、选出 ROOTDEV}', '\\subsection{阶段二：', r'''\subsection{阶段一：登记块设备、选出 ROOT\_DEV}
\label{sec:ns-stage1}

阶段一要回答“根文件系统在哪个块设备上”，为此先要有一张块设备表。原来 \code{bio.c} 的 \code{bread}/\code{bwrite} 直接调用 \code{rootdev\_rw()}，只能访问一个设备；现在 \file{blkdev.c} 仿照 Linux 的 gendisk 维护 \code{blk.dev[NBLKDEV]}，每项描述一个设备：

\begin{ccode}
// include/kernel/blkdev.h（节选）
struct blkdev {
  int      dev;      // BLKDEV_MMC=2, BLKDEV_MMC_PART(n)=2+n, BLKDEV_RAM(n)=7+n
  char     name[16]; // "mmcblk0", "mmcblk0p2", "ram0" ...
  uint32   start;    // 在硬件上的起始扇区（整盘为 0）
  uint32   nsect;    // 扇区数（0 = 未知，不做越界检查）
  uint32   bsize;    // 块缓存的块大小（512 / 1024 / 4096 B）
  blk_rw_fn rw;      // I/O 回调：整盘 mmc_rw，分区 part_rw，RAM 盘 ram_rw
  void     *priv;    // 驱动私有数据（RAM 盘的页）
  struct blkdev *disk;   // 分区所属的整盘；整盘为 0
  int      partno;   // 分区号，整盘为 0
  uchar    ptype;    // 分区的 MBR 类型（0x0c FAT32、0x83 Linux、0x7f xv6）
  char     uuid[12]; // PARTUUID "ssssssss-pp"
};
\end{ccode}

这张表由三个 initcall 先后填好，\code{main()} 里只剩一句 \code{do\_initcalls()}：

\begin{enumerate}
  \item \code{blkdev\_init()}（\code{subsys} 级）初始化设备表，并注册主设备号 \code{BLOCKDEV}，让块设备以后能以 \code{/dev/mmcblk0p2} 这样的文件出现；
  \item SD 驱动的 \code{sdhost\_probe()}（\code{device} 级）初始化卡，调用 \code{add\_disk(2, "mmcblk0", 5, \ldots, mmc\_rw)}；块层的 \code{blk\_add\_partitions()} 读 MBR，\code{add\_partition()} 把主分区 $N$ 登记为块设备 $2+N$，回调是 \code{part\_rw()}。和 Linux 一样，驱动只登记整盘，分区由块层自己扫描；
  \item \code{rootdev\_init()}（\code{late} 级）列出分区，挂上 FAT32 启动分区，由 \code{cmdline\_init()} 读出 \file{cmdline\_xv6.txt}（\ref{sec:rootfs} 节）；\code{check\_rootfstype()} 读 \code{rootfstype=}，\code{name\_to\_bdev()} 把 \code{root=} 换成块设备，\code{is\_xv6fs()} 确认那里是 xv6 文件系统，没有 \code{root=} 或分区不对时退回自动探测；最后记下 ROOT\_DEV，打印 \code{rootdev: ROOT\_DEV = 4 (mmcblk0p2)}。它不再登记任何设备。
\end{enumerate}

从此 \code{bio.c} 的读写入口是 \code{blk\_rw(b, write)}：按 \code{b->dev} 查表，把块号换成扇区号，调用该设备的回调。为了容纳 ext2 的 4\,KiB 块，\code{buf.data} 从静态数组改为 \code{kalloc()} 分配的页，\code{NBUF} 从 30 扩到 64；新增的 \code{buf.error} 让非根设备的 I/O 失败只置位而不 panic，根设备（\code{rootdev\_dev()}）失败仍 panic。

阶段一结束时，ROOT\_DEV 已经选定，但根分区的超级块还没有被读过，\code{/} 也还不存在。随后 \code{ext2init()} 只把 ext2 登记进文件系统类型链表（\code{register\_filesystem()}），不读盘，\code{vfsinit()} 登记其余几种文件系统并初始化 netfs，\code{userinit()} 造出第一个进程，此时它的当前目录还是空的。

\begin{note}
  块设备文件（\code{/dev/mmcblk0p2} 等）经过 buffer cache，用户态只读；\code{lseek(fd, 0, SEEK\_END)} 返回分区大小。它们的节点由阶段二的 devtmpfs 建出来。
\end{note}

''')

# ---------- 阶段二 ----------
s = rep(s, '3. kmknod("/dev/root", BLOCKDEV, ROOTDEV)\\\\ \\quad mount\\_root(): fsinit(ROOTDEV) + fs\\_mount(/root)',
        '3. kmknod("/dev/root", BLOCKDEV, ROOT\\_DEV)\\\\ \\quad mount\\_root(): vfs\\_get\\_sb() + fs\\_mount(/root)')
s = between(s, '\\paragraph{第 3 步：挂上根文件系统.}', '\\code{kchdir("/root")} 之后', r'''\paragraph{第 3 步：挂上根文件系统.} 先在 rootfs 里建设备节点 \code{/dev/root}（主设备号 \code{BLOCKDEV}，次设备号是 ROOT\_DEV：xv6fs 根为 4，ext2 根为 5；\ref{sec:ns-callchain} 节调用链中的 (c)）。\code{mount\_root()} 按 \code{rootdev\_fstype()} 取类型名，调用 \code{vfs\_get\_sb("xv6fs", "/dev/root", VFS\_MOUNT\_ROOTLOG)}：\code{get\_fs\_type()} 在类型链表里找到 \code{xv6fs\_type}，它的 \code{get\_sb\_bdev()} 用 \code{lookup\_bdev()} 解析节点，从 inode 的次设备号得到 4 (d)。设备 4 还没有超级块，于是调用 \code{xv6fs\_fill\_super(4)}：把块大小设为 1\,KiB，\code{fs\_readsuper()} 读块 1、检查魔数，并为设备 4 登记 \code{super\_block}（类型 xv6fs、函数表 \code{xv6\_iops}、根 inode 1，\ref{sec:vfs} 节）；带 \code{VFS\_MOUNT\_ROOTLOG} 时再由 \code{initlog()} 恢复日志。这一步对应 Linux 的 \code{fill\_super}，读操作经 \code{blk\_rw()} 和 \code{part\_rw()} 落到 p2 上。最后 \code{fs\_mount()} 把设备 4 上文件系统的根目录挂到 rootfs 的 \code{/root} 上，启动日志打印：

\begin{txtcode}
VFS: Mounted root (xv6fs filesystem) on device 4 (/dev/root = mmcblk0p2).
\end{txtcode}

''')

# ---------- 机制 ----------
s = rep(s, '\\code{fsinit(dev)} 在它之后再做日志恢复。',
        '只有根文件系统的 \\code{xv6fs\\_fill\\_super()} 带 \\code{VFS\\_MOUNT\\_ROOTLOG}，经 \\code{fsinit(dev)} 在它之后再做日志恢复；rootfs 和 devtmpfs 不带这个标志。')
s = rep(s, '会在 \\code{root} 这一级从 rootfs 跳进 ROOTDEV。', '会在 \\code{root} 这一级从 rootfs 跳进根文件系统（ROOT\\_DEV）。')
s = rep(s, 'namei("/bin/sh")    rootdir() = (dev 1, inode 1)', 'namei("/bin/sh")    rootdir() = (dev 4, inode 1)')
s = rep(s, 'namei("/dev/tty")   rootdir() = (dev 1, inode 1)', 'namei("/dev/tty")   rootdir() = (dev 4, inode 1)')

# ---------- fs-trace：日志来自旧内核 ----------
s = rep(s, '日志共 4408 行。', '日志共 4408 行。这份日志取自 \\code{/dev/root} 还经过“设备 1”包装的内核；现在同一个 p2 直接以块设备 4（\\code{mmcblk0p2}）挂载，下文的 dev 1 对应今天的 dev 4。')

# ---------- VFS 设备号 ----------
s = rep(s, '（xv6fs 根 1、rootfs 7、devtmpfs 8、ext2 分区 3～6 之一）', '（xv6fs 根是它所在的分区，通常为 4；rootfs 7、devtmpfs 8；ext2 分区 3～6 之一）')
s = rep(s, '\\code{fs\\_readsuper()} 登记 xv6fs，', '\\code{fs\\_readsuper()} 登记 xv6fs（由 \\code{xv6fs\\_fill\\_super()} 调用），')

# ---------- 跨文件系统路径解析示例 ----------
i = s.index('namei("/mnt/ext2/d/a.txt")'); j = s.index('\\subsection{文件描述符与系统调用}', i)
s = s[:i] + s[i:j].replace('(dev 1, ', '(dev 4, ') + s[j:]

# ---------- FAT32 调用关系图 ----------
s = rep(s, "    \\node[fn, text width=30mm] (c3) at (128,64) {根块设备 \\file{rootdev.c}\\\\（仅 \\code{FS.IMG} 兼容模式）};\n", "")
s = rep(s, "    \\node[fn, text width=30mm] (i3) at (128,48) {\\code{fat32mapfile}\\\\\\code{fat32mapsector}};\n", "")
s = rep(s, "\\draw[call] (c1) -- (i1); \\draw[call] (c2) -- (i2); \\draw[call] (c3) -- (i3);", "\\draw[call] (c1) -- (i1); \\draw[call] (c2) -- (i2);")
s = rep(s, "    \\draw[call] (i3.east) -- ++(3,0) |- (s1.east);\n", "")
s = rep(s, '三类调用者共用同一组目录与 FAT 操作', '两类调用者共用同一组目录与 FAT 操作')
s = re.sub(r'  \\item \\textbf\{文件扇区映射\}[^\n]*\n', '', s, count=1)
s = rep(s, '分成四层：', '分成三层：')

# ---------- xv6 根文件系统怎样被认出来 ----------
s = between(s, '\\code{rootdev\\_init()} 扫描 MBR 时，对每个既不是 FAT32', '\\subsection{superblock 与磁盘布局}', r'''块层登记完分区以后，\code{rootdev\_init()} 对 \code{root=} 指定的分区（自动探测时是每个非 FAT 主分区，类型 0x7f 的优先）调用 \code{is\_xv6fs()}（\file{kernel/drivers/block/rootdev.c}）：

\begin{ccode}
static int is_xv6fs(struct blkdev *bd) {
  // xv6 的第 1 块是 superblock；块大小 1024，所以它在分区内第 2 个扇区
  if((bd->nsect != 0 && bd->nsect < FSSIZE * SECTORS_PER_BLOCK) ||
     bdev_read(bd, SECTORS_PER_BLOCK, sector) < 0 ||
     le32(sector) != FSMAGIC || le32(sector + 4) != FSSIZE)
    return 0;
  return 1;
}
\end{ccode}

判断只看两样东西：superblock 开头的魔数 \code{0x10203040}，以及紧跟着的 \code{size} 是否等于内核编译时的 \code{FSSIZE}。MBR 里的分区类型只决定自动探测的先后，不作为判断依据——macOS 的 \code{diskutil} 只能把这个分区建成 Linux 的 \code{0x83} 类型，和真正的 ext2 分区一样，靠魔数才能分开。\code{size} 的检查同样重要：如果有人改了 \code{FSSIZE} 重新编译内核却没重新烧 \code{fs.img}，这里会拒绝，而不是让新内核去解释旧格式的磁盘。识别成功后启动日志打印：

\begin{txtcode}
rootdev: xv6 filesystem on mmcblk0p2 start=208896 sectors=819200 size=400 MiB
rootdev: ROOT_DEV = 4 (mmcblk0p2)
\end{txtcode}

此后块缓存对设备 4 的每一次读写都经过 \code{blk\_rw()}：它按 \code{b->dev} 查到 \code{mmcblk0p2}，把块号换成分区内的扇区号（1 块 = 2 扇区），再调用分区的回调：

\begin{ccode}
static int part_rw(struct blkdev *bd, uint32 sector, uchar *data,
                   uint32 count, int write) {   // kernel/drivers/block/blkdev.c
  struct blkdev *disk = bd->disk;               // mmcblk0
  return disk->rw(disk, bd->start - disk->start + sector, data, count, write);
}
\end{ccode}

扇区号 $=$ 分区起点 208896 $+$ 块号 $\times 2$，然后由整卡的 \code{mmc\_rw()} 交给 SDHOST。整条路径不经过 \file{fat32.c} 的任何函数。分区有 400\,MiB，文件系统只用前 32\,MiB；可分配多少块由 superblock 决定，与分区大小无关。

''')
s = rep(s, '\\code{FSMAGIC}；\\code{fsinit()} 和 \\code{setup\\_raw\\_root()} 都检查它', '\\code{FSMAGIC}；\\code{fs\\_readsuper()} 和 \\code{is\\_xv6fs()} 都检查它')

# ---------- 目录与路径查找 ----------
s = rep(s, '\\ding{172} \\code{iget(ROOTDEV, 1)}', '\\ding{172} \\code{rootdir()} $=$ (4, 1)')
s = rep(s, "if(*path == '/') ip = iget(ROOTDEV, ROOTINO);        // 绝对路径从 inode 1 开始",
        "if(*path == '/') ip = rootdir();                      // 绝对路径从 fs_root，即 (4, 1) 开始")
s = rep(s, '\\code{video0}、\\code{sdroot} 等设备节点', '\\code{video0}、\\code{mmcblk0p2} 等设备节点')

# ---------- 从路径到 SD 扇区 ----------
s = rep(s, '''bread(ROOTDEV, B)            -> 块缓存未命中，调用 rootdev_rw(b, 0)
root_sector_lba(2B), (2B+1)  -> 扇区 208896 + 2B 和 208896 + 2B + 1
sdsector(lba, buf, 0)        -> SDHOST CMD17，PIO 读 512 字节''',
'''bread(4, B)                  -> 块缓存未命中，调用 blk_rw(b, 0)
blk_rw: mmcblk0p2            -> 分区内扇区 2B，共 2 个扇区
part_rw(p2, 2B, 2)           -> 加上分区起点：卡上扇区 208896 + 2B
mmc_rw(mmcblk0, ...)         -> SDHOST 以 PIO 读 2 × 512 字节''')
s = rep(s, '\\code{end\\_op()} 提交时才真正经 \\code{rootdev\\_rw()} 写到 SD 卡。', '\\code{end\\_op()} 提交时才真正经 \\code{blk\\_rw()} 写到 SD 卡。')

# ---------- 更新根文件系统 ----------
s = between(s, '\\subsection{在系统里更新根文件系统}', '\\section{烧录 SD 卡}', r'''\subsection{更新根文件系统}

根文件系统的格式一旦改动（例如 \code{DIRSIZ} 从 14 改成 30），就必须整体重写 p2。现在的办法只有一个：把 SD 卡插到开发机上，用下一节的 \code{make install-rpi3} 或 \code{dd} 写入新的 \file{fs.img}。

早先内核提供过一个受保护的字符设备 \code{/dev/sdroot}，配合用户态 \code{/bin/dd} 在系统里把 \file{/boot/fs.img} 写回 p2：打开时冻结根文件系统，逐扇区写入并读回校验，结束后必须断电重启。它依赖“设备 1（root）”这层包装，在 \code{/dev/root} 改成直接指向分区（\ref{sec:ns-stage1} 节）时与 \code{/bin/dd} 一起删掉了，\file{include/uapi/device.h} 里只留下一行注释。块设备文件 \code{/dev/mmcblk0p2} 在用户态是只读的，运行中的系统无法覆盖自己的根分区。

''')
open(p, 'w').write(s)

# ---------- 09-optional ----------
p = B + '09-optional.tex'; s = open(p).read()
s = rep(s, '更新根文件系统的 \\code{/dev/sdroot}（第~\\ref{sec:xv6fs} 节）只写前 \\code{FSSIZE} 块，不会碰到日志区。',
        '在开发机上重写 p2 时（第~\\ref{sec:xv6fs} 节）只写 \\file{fs.img} 的前 \\code{FSSIZE} 块，不会碰到日志区。')
open(p, 'w').write(s)
print('ok')
