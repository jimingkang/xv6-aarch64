import sys, shutil, os, re
R = 'doc/readme/'
os.makedirs('build/md-bak4', exist_ok=True)
FILES = ('readme_vfs.md', 'xv6 启动文件系统追踪.md', 'readme_ext2.md',
         'readme_raw_xv6_partition.md', 'readme_bug_fix.md', 'readme_kernel_lock.md')
for f in FILES:
    shutil.copy(R + f, 'build/md-bak4/' + f)
CODE = ('kernel/drivers/block/rootdev.c', 'kernel/drivers/mmc/host/sdhost.c',
        'kernel/fs/fat/fat32.c', 'kernel/drivers/net/wireless/mt7601u.c')
for f in CODE:
    shutil.copy(f, 'build/md-bak4/' + os.path.basename(f))

def rep(s, a, b, cnt=1):
    n = s.count(a)
    if n != cnt:
        sys.exit('count %d != %d for: %s' % (n, cnt, a[:90]))
    return s.replace(a, b)

def block(s, start, end, new):
    i = s.index(start)
    j = s.index(end, i)
    return s[:i] + new + s[j:]

# ======================= readme_vfs.md =======================
p = R + 'readme_vfs.md'; s = open(p).read()

s = rep(s, '| 1 | xv6 根分区（ROOTDEV） | xv6fs / `xv6_iops`，logged | 1 |',
           '| 4 | mmcblk0p2 上的 xv6fs 根（ROOT_DEV） | xv6fs / `xv6_iops`，logged | 1 |')
s = rep(s, '(dev 1, ino 1) -xv6_lookup-> mnt -> ext2 (dev 1, ino 26)',
           '(dev 4, ino 1) -xv6_lookup-> mnt -> ext2 (dev 4, ino 26)')
s = rep(s, '(dev 1) proc -> follow_mount', '(dev 4) proc -> follow_mount')

s = block(s, 'main\n  -> sd_driver_init\n  -> rootdev_init\n', '       -> fs_mount\n            mtab: covered /root inode',
'''main -> do_initcalls
  [device] sd_driver_init -> sdhost_probe
       -> add_disk(2, "mmcblk0", ...)        整卡成为块设备 2
       -> blk_add_partitions                 块层读 MBR
            -> add_partition(mmcblk0pN)      分区成为块设备 2+N，回调 part_rw
  [late]   rootdev_init                      不登记设备，只挑选
       -> fat32mount(p1)                     读 cmdline_xv6.txt
       -> name_to_bdev("/dev/mmcblk0p2")
       -> is_xv6fs                           读分区内 block 1，检查 FSMAGIC/FSSIZE
       -> ROOT_DEV = 4

first process: prepare_namespace
  -> 建立临时 ram0 rootfs
  -> kmknod("/dev/root", BLOCKDEV, ROOT_DEV) 次设备号 4
  -> mount_root("/dev/root", "/root")
       -> vfs_get_sb("xv6fs", "/dev/root", VFS_MOUNT_ROOTLOG)
            -> get_sb_bdev -> lookup_bdev    从节点读回 dev 4
            -> xv6fs_fill_super -> fsinit(4)
                 -> fs_readsuper
                      bread(dev, 1)          buffer cache 读取 superblock
                      sbs[dev] = on-disk superblock
                      fs_super_register(... xv6_iops, ROOTINO=1)
                 -> initlog                  日志恢复
''')

s = rep(s, 'SB1[dev 1 VFS super_block\\nxv6fs rootino 1]', 'SB1[dev 4 VFS super_block\\nxv6fs rootino 1]')
s = rep(s, 'V->>I: iget_path(dev=1, ino=1)', 'V->>I: iget_path(dev=4, ino=1)')
s = rep(s, '`(dev=1, ino=1)`：`dev=1` 是 raw xv6 根块设备，', '`(dev=4, ino=1)`：`dev=4` 是根分区 mmcblk0p2 这个块设备，')
s = rep(s, '`itable[2] = (dev=1, ino=1)`', '`itable[2] = (dev=4, ino=1)`')
s = re.sub(r'dev1(?!\d)', 'dev4', s)

# devtmpfs block7 table: no sdroot, ino 8.. shift down by one
old_rows = '''| `[6] / 192` | `video0 : ino7` | block2 / slot7 |
| `[7] / 224` | `sdroot : ino8` | block2 / slot8 |
| `[8] / 256` | `mmcblk0 : ino9` | block2 / slot9 |
| `[9] / 288` | `mmcblk0p1 : ino10` | block2 / slot10 |
| `[10] / 320` | `mmcblk0p2 : ino11` | block2 / slot11 |
| `[11] / 352` | `mmcblk0p3 : ino12` | block2 / slot12 |
| `[12] / 384` | `ram0 : ino13` | block2 / slot13 |
| `[13] / 416` | `ram1 : ino14` | block2 / slot14 |'''
new_rows = '''| `[6] / 192` | `video0 : ino7` | block2 / slot7 |
| `[7] / 224` | `mmcblk0 : ino8` | block2 / slot8 |
| `[8] / 256` | `mmcblk0p1 : ino9` | block2 / slot9 |
| `[9] / 288` | `mmcblk0p2 : ino10` | block2 / slot10 |
| `[10] / 320` | `mmcblk0p3 : ino11` | block2 / slot11 |
| `[11] / 352` | `ram0 : ino12` | block2 / slot12 |
| `[12] / 384` | `ram1 : ino13` | block2 / slot13 |'''
s = rep(s, old_rows, new_rows)
s = rep(s, '分配 ino14；创建完成后\n它位于下一个 32 字节槽 `off416`。日志打印的 `devtmpfs: initialized on ram1, 12 nodes`',
           '分配 ino13；创建完成后\n它位于下一个 32 字节槽 `off384`。日志打印的 `devtmpfs: initialized on ram1, 11 nodes`')

# FAT32 probing section
s = rep(s, '''主核在 `kernel/init/main.c` 中先调用 `fat32init()` 初始化 FAT32 锁，再调用 `rootdev_init()`。
后者在 `kernel/drivers/block/rootdev.c` 中读取 MBR，先找带 xv6 超级块的原始根分区，再对 0x0b/0x0c 分区调用
`fat32mount()`。''', '''`do_initcalls()` 先在 fs 级调用 `fat32init()` 初始化 FAT32 锁，再在 late 级调用 `rootdev_init()`。
MBR 早在 device 级 SD 驱动 `add_disk()` 时由块层扫描好；`rootdev_init()`（`kernel/drivers/block/rootdev.c`）
只遍历已登记的 `mmcblk0pN`，对第一个 0x0b/0x0c 分区调用 `fat32mount()`。''')
s = block(s, 'main()\n  -> fat32init()                     初始化 fat32_lock\n', '```\n\n#### 2. init 创建挂载点并读取 fstab',
'''main()
  -> do_initcalls()
       [fs]     fat32init()             初始化 fat32_lock
       [device] sd_driver_init()        add_disk(mmcblk0)，块层扫描 MBR
       [late]   rootdev_init()          kernel/drivers/block/rootdev.c
            -> fat32mount(p->start, p->nsect)   kernel/fs/fat/fat32.c
                 -> 读取 BPB
                 -> 保存 FAT/data/root 布局
                 -> diskmap.fat = 1
            -> cmdline_init()           读 cmdline_xv6.txt，选 ROOT_DEV
       [late 6] vfsinit()               登记 procfs、sysfs、fat32、netfs
''')

# rootdev.c / blkdev.c / fat32.c table
s = rep(s, "| `kernel/drivers/block/rootdev.c` | `rootdev_init()` 扫描 MBR，为每个有效主分区调用 `blkdev_register()` 注册为 `mmcblk0pN`；`setup_raw_root()` 用块 1 超级块识别 xv6 根分区；`root_blk_rw()` 是根设备的 blkdev 回调，把 xv6 块号换算成 SD 扇区；`rootdev_update_begin()` 供 `/dev/sdroot` 在线更新使用 |",
           "| `kernel/drivers/block/rootdev.c` | `rootdev_init()` 在块层已登记的 `mmcblk0pN` 里挂起 FAT32 启动分区，按 `root=`/`rootfstype=` 选出 ROOT_DEV；`is_xv6fs()` 用块 1 超级块识别 xv6 分区；`rootdev_xv6_part()` 把 p2 交给 ext2 的外置日志。它自己不登记块设备，也没有读写回调 |")
s = rep(s, "| `kernel/drivers/block/blkdev.c` | 通用块设备表（NBLKDEV=10），`blk_rw()` 取代原来的 `rootdev_rw()`，bio.c 的所有读写都走这里 |",
           "| `kernel/drivers/block/blkdev.c` | 通用块设备表（NBLKDEV=10）；`add_disk()` 登记整盘，`blk_add_partitions()`/`msdos_partition()` 读 MBR，`add_partition()` 登记分区；`blk_rw()` 派发 bio.c 的所有读写 |")
s = rep(s, "`fat32mount()` 解析 BPB，目录、长文件名、簇分配、读写；`fat32mapsector()` 把 FS.IMG 文件的扇区号换成卡上扇区号（旧卡兼容） |",
           "`fat32mount()` 解析 BPB，目录、长文件名、簇分配、读写 |")
s = block(s, '''       -> bd->rw(bd, block*spb, data, spb)    设备回调
            ROOTDEV  -> root_blk_rw()''', '```\n\n启动日志对应',
'''       -> bd->rw(bd, block*spb, data, spb)    设备回调，扇区号相对设备
            mmcblk0pN -> part_rw()            加上分区起点 bd->start
                           -> mmc_rw()        交给整卡，sdsector
            mmcblk0   -> mmc_rw()
            ram0/ram1 -> ram_rw()             内存 RAM 盘
''')
s = block(s, 'rootdev: raw xv6 root p2 lba=133120 sectors=81920 size=40 MiB\n', '```\n',
'''rootdev: partitions of mmcblk0
rootdev:   mmcblk0p1 type=0xc (FAT32-LBA) start=2048 sectors=131072 size=64 MiB partuuid=...
rootdev:   mmcblk0p2 type=0x7f (xv6 raw filesystem) start=133120 sectors=81920 size=40 MiB partuuid=...
rootdev: bootfs mmcblk0p1 lba=2048 mounted as FAT32
rootdev: root=/dev/mmcblk0p2 -> mmcblk0p2
rootdev: xv6 filesystem on mmcblk0p2 start=133120 sectors=81920 size=40 MiB
rootdev: ROOT_DEV = 4 (mmcblk0p2)
rootdev: no partitions; root is the bare xv6 image on mmcblk0      (QEMU 裸镜像)
blkdev: dev 2 mmcblk0 (disk, 5 minors) sectors=... block=512
blkdev: dev 3 mmcblk0p1 (partition 1 of mmcblk0) start=2048 sectors=131072 block=512
blkdev: dev 4 mmcblk0p2 (partition 2 of mmcblk0) start=133120 sectors=81920 block=1024
blkdev: cache self-test ok (mmcblk0 sector 0: 1 device read, MBR signature, second read hit)
''')

# struct blkdev
s = block(s, '#define NBLKDEV 10\nstruct blkdev {\n', '};\n',
'''#define NBLKDEV 10
struct blkdev {
  int used;
  int dev;            // 设备号（BLKDEV_MMC=2, BLKDEV_MMC_PART(n)=2+n, ram0=7, ram1=8）
  char name[16];      // "mmcblk0", "mmcblk0p2", "ram0", "ram1" ...
  uint32 start;       // 分区在硬件上的起始扇区（整盘 = 0）
  uint32 nsect;       // 扇区数（0 = 未知，不做越界检查）
  uint32 bsize;       // 块缓存的块大小（512/1024/4096 B）
  blk_rw_fn rw;       // 设备 I/O 回调，扇区号相对本设备
  void *priv;         // 驱动私有数据（RAM 盘的页）
  struct blkdev *disk;  // 分区：所属整盘；整盘为 0
  int partno;         // 整盘 0，分区 N
  int minors;         // 整盘预留的设备号个数 dev .. dev+minors-1
  uchar ptype;        // 分区：MBR 类型（0x0c FAT32，0x83 Linux，0x7f xv6）
  ...
''')

s = rep(s, '`mount_root()` 核对分区后 `ext2_register_super()`，挂到 `/root`',
           '`mount_root()` 经 `vfs_get_sb("ext2")` 调用 `ext2_fill_super()`，最后 `ext2_register_super()`，挂到 `/root`')
s = rep(s, 'xv6 分区（p2）仍被注册为 ROOTDEV（设备 1），供 `/dev/sdroot` 和 ext2 的外置日志使用；没有也不 panic。',
           '`rootdev_init()` 仍然找出 xv6 分区 p2（`rootdev_xv6_part()`），ext2 的外置日志在它尾部；没有也不 panic，只是关闭日志。')
s = rep(s, 'rootdev: root=/dev/mmcblk0p2 -> mmcblk0p2 lba=133120 sectors=81920 size=40 MiB\n',
           'rootdev: root=/dev/mmcblk0p2 -> mmcblk0p2\nrootdev: xv6 filesystem on mmcblk0p2 start=133120 sectors=81920 size=40 MiB\nrootdev: ROOT_DEV = 4 (mmcblk0p2)\n')
s = rep(s, '`forkret()` 里的 `fsinit(ROOTDEV)` 改为 `prepare_namespace()`',
           '`forkret()` 里原来的 `fsinit(ROOTDEV)` 改为 `prepare_namespace()`')
s = block(s, '  ③ kmknod("/dev/root", BLOCKDEV, ROOTDEV)\n', '     kchdir("/root")\n',
'''  ③ kmknod("/dev/root", BLOCKDEV, ROOT_DEV)  xv6fs 根时次设备号 4，ext2 根时 5
     mount_root("/dev/root", "/root")
       vfs_get_sb(rootfstype, "/dev/root", VFS_MOUNT_ROOTLOG)
         xv6fs_fill_super -> fsinit(dev)  读磁盘超级块，日志恢复
       fs_mount(/root, dev)               把磁盘根挂在 /root
''')
open(p, 'w').write(s)

# ======================= 启动文件系统追踪 =======================
p = R + 'xv6 启动文件系统追踪.md'; s = open(p).read()
s = rep(s, '`/` 是 dev 1（mmcblk0p2 上的 xv6fs）', '`/` 是 dev 4（mmcblk0p2 上的 xv6fs）')
s = rep(s, '虚线箭头上的 `1:54`', '虚线箭头上的 `4:54`')
s = s.replace('根文件系统（dev 1）', '根文件系统（dev 4）')
s = rep(s, 'xv6_read_inode: dev=1 ino=10', 'xv6_read_inode: dev=4 ino=10')
s = rep(s, '第 3 步建出的 /dev/root，次设备号 1', '第 3 步建出的 /dev/root，次设备号 4')
s = rep(s, '`kmknod("/dev/root", BLOCKDEV, 1)`', '`kmknod("/dev/root", BLOCKDEV, 4)`', 2)
s = rep(s, "| #5 | 320–383 | ino 5 | `/dev/root` | 3 T_DEVICE | 7 BLOCKDEV | 1 | 1 | 0 | 0 |",
           "| #5 | 320–383 | ino 5 | `/dev/root` | 3 T_DEVICE | 7 BLOCKDEV | 4 | 1 | 0 | 0 |")
s = rep(s, '`kmknod("/dev/root", T_DEVICE, BLOCKDEV, 1)` 建，major 7 = 块设备，minor 1 = 块设备表里的 dev 1。',
           '`kmknod("/dev/root", T_DEVICE, BLOCKDEV, 4)` 建，major 7 = 块设备，minor 4 = 块设备表里的 dev 4（mmcblk0p2）。')
s = rep(s, '14 个 inode 都在块 2；根目录块 7 有 14 项（console、tty、ttyS0、input、video0、sdroot、mmcblk0、\nmmcblk0p1–p3、ram0、ram1，共 448 字节）',
           '13 个 inode 都在块 2；根目录块 7 有 13 项（console、tty、ttyS0、input、video0、mmcblk0、\nmmcblk0p1–p3、ram0、ram1，共 416 字节）')
s = rep(s, '被重新从块 2 读了 12 次', '被重新从块 2 读了 11 次')
if re.search(r'dev ?1(?!\d)|dev=1(?!\d)|sdroot', s):
    sys.exit('trace md still has dev 1/sdroot')
open(p, 'w').write(s)

# ======================= readme_ext2.md =======================
p = R + 'readme_ext2.md'; s = open(p).read()
s = rep(s, '没有原始 xv6 根分区（QEMU 裸镜像或旧的 FS.IMG 模式）时打印',
           '没有 xv6 分区（QEMU 裸镜像）时打印')
s = rep(s, '`/dev/sdroot` 只写前 `FSSIZE` 块，不会覆盖日志区。',
           '运行中的系统不能改写 p2（块设备文件在用户态只读，`/dev/sdroot` 已删除）；在开发机上用\n`make install-rpi3` 重写 p2 时只写 `fs.img` 的 `FSSIZE` 块，不会覆盖日志区。')
open(p, 'w').write(s)

# ======================= readme_raw_xv6_partition.md =======================
p = R + 'readme_raw_xv6_partition.md'; s = open(p).read()
s = block(s, '## 12. 在 xv6 内在线更新 p2 根文件系统\n', '## 13. ',
'''## 12. 在 xv6 内在线更新 p2 根文件系统（已删除）

早先内核注册过受保护的字符设备 `/dev/sdroot`，配合用户命令 `/bin/dd` 把 TFTP 下载到
`/boot/fs.img` 的新镜像写回 p2：打开时冻结根文件系统，逐扇区写入并读回校验，结束后必须断电重启。
它依赖“设备 1（root）”这层包装；`/dev/root` 改为直接指向分区块设备（xv6fs 根是 dev 4
`mmcblk0p2`，ext2 根是 dev 5 `mmcblk0p3`）以后，`/dev/sdroot` 和 `/bin/dd` 一起删掉了，
`include/uapi/device.h` 里只留下 `/* 6 was ROOTUPDATE (/dev/sdroot), removed */`。

现在块设备文件 `/dev/mmcblk0p2` 在用户态是只读的，运行中的系统无法覆盖自己的根分区。
更新 p2 只有一条路：把 SD 卡插到开发机上，执行

```sh
make install-rpi3 RPI3_XV6_DEV=/dev/rdisk4s2
```


''')
open(p, 'w').write(s)

# ======================= initcall 名 =======================
for p, a, b in (
    (R + 'readme_bug_fix.md', '`sdroot` 属于 late initcall', '`rootdev_init()` 属于 late initcall'),
    (R + 'readme_kernel_lock.md', '`sdroot` 挂载 FAT32 bootfs 后', '`rootdev_init()` 挂载 FAT32 bootfs 后'),
    ('kernel/drivers/block/rootdev.c', 'late_initcall("sdroot", rootdev_init);', 'late_initcall("rootdev", rootdev_init);'),
    ('kernel/drivers/mmc/host/sdhost.c', '// The external SD card has to be a block device before sdroot mounts it;',
                                         '// The external SD card has to be a block device before rootdev_init()\n// picks the root from its partitions;'),
    ('kernel/fs/fat/fat32.c', '// mounted later by sdroot.', '// mounted later by rootdev_init().'),
    ('kernel/drivers/net/wireless/mt7601u.c', "// Order 3: needs sdroot's firmware files and dwc2's enumeration.",
                                              "// Order 3: needs the bootfs firmware files (rootdev_init() mounts\n// bootfs) and dwc2's enumeration."),
):
    s = open(p).read(); s = rep(s, a, b); open(p, 'w').write(s)
print('ok')
