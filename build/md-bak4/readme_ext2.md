# xv6 的 ext2 文件系统

本项目把 SD 卡上的传统 ext2 分区接入统一 VFS。它既可以作为 `/dev/root` 挂到临时
rootfs 的 `/root` 后再 `MS_MOVE` 成为 `/`，也可以作为附加文件系统挂到 `/mnt/ext2`：

```text
SD card
├─ p1 FAT32  -> /boot
├─ p2 XV6FS  -> 可选原生根文件系统，同时可提供 xjournal 区域
├─ p3 ext2   -> / 或 /mnt/ext2
└─ p4/SPFS   -> reserved
```

块层先把 MBR 分区注册为 `mmcblk0pN`；挂载 ext2 时，`ext2_fill_super(dev)` 再通过
`0xef53` superblock、feature bits 和根 inode 判断该设备是否兼容。因此 p2 即使也使用
`0x83` 占位，也不会被误认为 ext2。

## VFS 使用

`/etc/fstab` 默认包含：

```text
ext2 /mnt/ext2 ext2 rw 0 0
```

`init` 不会覆盖已经存在的 `/etc/fstab`。从旧镜像升级且原配置仍为 `ro` 时，需要把该行
手动改为 `rw` 后重启；只读挂载仍然受支持。

探测和挂载成功后可以直接使用普通 VFS 命令：

```sh
ls /mnt/ext2
cat /mnt/ext2/hello.txt
touch /mnt/ext2/new.txt
echo hello > /mnt/ext2/new.txt
edit /mnt/ext2/new.txt
umount /mnt/ext2
```

`umount` 只允许卸载非根挂载。存在打开文件、进程 cwd、正在使用的 inode 或子挂载时返回
busy；成功时先阻止新的路径遍历进入该挂载，再写回 clean 状态、flush、关闭外置日志、清空
该设备的 bcache，最后释放 mount/superblock 和 ext2 实例资源。

原来的诊断命令仍然保留：

```sh
ext2ls /
ext2cat /hello.txt
```

## 挂载过程：类型、超级块、挂载表与 inode 缓存

ext2 按 Linux 的方式接入 VFS：启动早期只登记“有 ext2 这种文件系统”，真正打开分区、
读 superblock、恢复外置日志都推迟到挂载那一刻的 `fill_super`。挂载涉及四张表，各管一件事：

| 表 | 定义 | 按什么查 | ext2 根启动后里面的那一项 |
|---|---|---|---|
| `file_systems` 链表 | `kernel/fs/vfs.c:1044` | 类型名 | `ext2_type = { "ext2", ext2_get_sb, ext2_fill_super }` |
| `supers[NSUPER]` 数组（24 项） | `kernel/fs/fs.c:39` | dev 号（就是下标） | `supers[5] = { "ext2", ext2_iops, rootino 2, priv = &ext2_instances[5], ext2_put_super }` |
| `mtab.m[NFSMOUNT]` 数组（8 项） | `kernel/fs/fs.c:932` | 被盖住的 inode `mp`，或 dev | `m[0] = { mp, dev 5, "ext2", source "/dev/root", path }` |
| `itable.inode[NINODE]` 数组（50 项） | `kernel/fs/fs.c:303` | (dev, inum) | `(5,2)` 是 ext2 根目录，`(5,76)` 是 `/dev` 目录 …… |

另外 `ext2_instances[NSUPER]`（`ext2.c:93`）是 ext2 自己的私有实例，同样以 dev 为下标：
分区起点/长度、block size、`blockbuf/ptrbuf`、外置日志、`open[]` 表和每实例的 `lock`。

- **类型表**回答“认不认识 ext2”，是链表，`get_fs_type()` 按名字遍历。
- **supers[]** 回答“dev 5 上是什么文件系统、根 inode 号多少、用哪套 `inode_ops`”。
- **mtab** 只记录覆盖关系：“目录 `mp` 被 dev 号为 `dev` 的文件系统的根目录盖住”。
  它不存 superblock 指针，用的时候 `getsuper(m->dev)` 取 `&supers[dev]`。
- **itable** 是内存里的 inode 表（inode cache），只按 (dev, inum) 认 inode，不记名字。
  名字到 inum 的对应在目录块里，每次由 `dirlookup` 扫描；xv6 没有 dentry cache。

![挂载项如何连到 superblock](../svg/ext2-mount-superblock.svg)

### 1. 登记类型（阶段一）

```text
do_initcalls()
  [late 5] ext2init()                      ext2.c:1208
             清空 ext2_instances[]，初始化每实例 lock
             register_filesystem(&ext2_type)   只登记，不读盘
  [late 6] vfsinit()                       登记 procfs / sysfs / fat32 / netfs
```

日志：`register_filesystem: ext2`，随后 `vfs: one mount table …; xv6fs ext2 procfs …`。

### 2. 作为根：`mount_root("/dev/root", "/root")`

`cmdline.txt` 写 `root=/dev/mmcblk0p3 rootfstype=ext2`。`rootdev_init()` 把 ROOT_DEV 记为
5（`mmcblk0p3`），`rootdev_fstype()` 返回 `ROOTFS_EXT2`。`prepare_namespace()`
（`kernel/init/do_mounts.c:405`）随后：

```text
ramfs_create(0, 7, "ram0")            rootfs，dev 7
fs_dev_root(7) -> fs_set_root, cwd    "/" = (7,1)；不是挂载，mtab 仍为空
kmknod /dev, /dev/console, /root      (7,2) (7,3) (7,4)
devtmpfs_init()                       ram1，dev 8
kmknod("/dev/root", T_DEVICE, BLOCKDEV, 5)        (7,5)，minor = 5
mount_root("/dev/root", "/root")      do_mounts.c:390
  vfs_get_sb("ext2", "/dev/root", VFS_MOUNT_ROOTLOG)       vfs.c:1103
    get_fs_type("ext2")               沿 file_systems 链表找到 ext2_type
    ext2_get_sb()                     ext2.c:1181
      lookup_bdev("/dev/root")        namei -> (7,5)：T_DEVICE, major BLOCKDEV, minor 5 -> dev 5
      getsuper(5) == 0                还没挂过
      ext2_fill_super(5)              ext2.c:1126
        blkdev_get(5)                 p3：start、nsect、rw = part_rw -> mmcblk0
        setup_ext2()                  读并校验 superblock、块组描述符、特性位
        ext2_journal_init()           rootdev_xv6_part() 取 p2，日志从 p2 内 65536 扇区开始；
                                      xj_init() 有已提交事务就重放
        readinode(EXT2_ROOT_INO=2)    根 inode 必须是受支持的目录
        ext2_register_super(5)        fs_super_register -> supers[5]，put_super = ext2_put_super
  klookup("/root")                    (7,4)
  fs_mount((7,4), 5, "ext2", "/dev/root", "/root")         fs.c:1062 -> mtab.m[0]
```

`ext2_fill_super()` 失败时会关闭日志、`binvalidate(dev)`、把 block size 恢复成 512 B 并释放
实例资源，`mount_root()` 随即 panic。

![mount_root 怎样从 /dev/root 找到 ext2 的 fill_super](../svg/ext2-mount-root-lookup.svg)

### 3. 进入、搬移与 chroot

| 步骤 | mtab | itable 中的变化 |
|---|---|---|
| `fs_mount((7,4), 5)` | `m[0] = { mp (7,4), dev 5, path "/root" }` | (7,4) ref 1，被 `m[0].mp` 持有 |
| `kchdir("/root")` | 不变 | `follow_mount`：`mount_covering(7,4)` 命中 `m[0]` → `getsuper(5)` → `fs_dev_root(5)` 取 (5,2)；cwd = (5,2)，(7,1) ref 2→1 |
| `devtmpfs_mount("dev")` | `m[1] = { mp (5,76), dev 8, path "/root/dev" }` | (5,76) ref 1，被 `m[1].mp` 持有 |
| `fs_move_mount(cwd, "/", "/")` | `m[0].mp` 由 (7,4) 改为 (7,1)；path `"/root"`→`"/"`，`"/root/dev"`→`"/dev"` | (7,1) ref 1→2，(7,4) ref 1→0 |
| `fs_set_root(cwd)` | 不变 | `fs_root` = (5,2)，ref 2；(7,1) 只剩 `m[0]` 持有，从路径上已不可达 |

`(5,2)` 第一次被 `ilock()` 时才调用 `ext2_iops.read_inode` 读盘；`iget_path()` 填槽时
`ip->iop = sb->iop`，所以之后对它的 lookup/read 都走 ext2。

![调用链与每一步的 mtab / itable / inode / 目录项](../svg/ext2-mount-callchain.svg)

### 4. `/init` 里的 fstab：`ext2 /mnt/ext2 ext2 rw 0 0`

`mount_fstab()` 依次挂 `/proc`（dev 20）、`/sys`（23）、`/boot`（21）、`/mnt/net`（22），
它们的 `get_sb` 只登记固定 dev 的 superblock。到 ext2 这一行时：

```text
vfsmount("ext2", "/mnt/ext2", "ext2", 0)        vfs.c:1157
  vfs_get_sb("ext2", "ext2") -> ext2_get_sb()
    lookup_bdev("ext2") = -1                    source 不是设备节点
    ext2_default() 已有实例 -> dev 5             （xv6fs 根时：扫描 0x83 分区逐个 fill_super）
    getsuper(5) != 0                            不再 fill_super
  fs_mount(/mnt/ext2, 5)
    mount_of_dev(5) 已是 m[0] -> -1
vfs: cannot mount ext2 at /mnt/ext2 (busy or already mounted)
```

同一个 dev 在 mtab 里只能出现一次，所以 ext2 作根时 `/mnt/ext2` 这一行必然失败，
不影响启动；xv6fs 作根时它才真正把 p3 挂到 `/mnt/ext2`。

### 与 Linux 的对应

| xv6 | Linux |
|---|---|
| `file_systems` 链表、`register_filesystem/get_fs_type` | 同名，`fs/filesystems.c` |
| `ext2_get_sb` + `lookup_bdev` + `ext2_fill_super` | `ext2_mount` → `mount_bdev` → `ext2_fill_super` |
| `supers[dev]`，`getsuper(dev)` | 动态分配的 `super_block`，挂在 `super_blocks` 和 `fs_type->fs_supers` 链表上 |
| `mtab.m[i]`：`mp` inode、`dev`、`path` 字符串 | `struct mount`：`mnt_mountpoint` dentry + `mnt_parent`、`mnt.mnt_sb`、`mnt.mnt_root`；路径由 `d_path` 现算 |
| 线性扫描 `mount_covering()` | `mount_hashtable` + dentry 的 `DCACHE_MOUNTED` |
| `itable`，按 (dev, inum) | icache；另有 dcache 缓存“名字 → inode” |
| `fs_move_mount` 手动改写子挂载 path | `MS_MOVE` 只改 `mnt_parent/mnt_mountpoint` |

## 已实现的读路径

- 1 KiB、2 KiB、4 KiB block；
- 经典 ext2 inode 与 MBR primary `0x83` 分区；
- 普通文件和线性目录；
- direct、single-indirect、double-indirect 和 triple-indirect block；
- 稀疏文件空洞读取；
- 64 位 VFS 文件偏移和 inode 大小；
- VFS `open/read/pread/lseek/fstat/close/readdir`。

## 已实现的写路径

- 在已有目录中创建普通文件；
- `O_TRUNC` 与 `ftruncate` 缩短或稀疏扩展普通文件；
- 顺序写、`pwrite` 和超过 4 GiB 的稀疏文件；
- 分配、清零、释放 direct/single/double/triple-indirect data block，并在
  truncate 时回收空的间接块；
- 更新 block bitmap、inode bitmap、group descriptor 和主 superblock 的空闲计数；
- `mkdir`、空目录/普通文件 `unlink`、目标不存在时的同文件系统 `rename`；
- VFS `open(O_CREATE|O_WRONLY/O_RDWR)`、`write/pwrite`、`fsync/fdatasync` 和普通
  shell 重定向。

每个挂载实例都有独立的 `ext2_fs.lock`，用于串行化该实例的元数据访问，避免多个 CPU 同时
修改 bitmap；不同 ext2 分区不再共享原来的全局 `e2`。写入范围还会
同时检查分区扇区边界、superblock block 数和当前实现支持的最大文件大小。

挂载时还会严格检查磁盘格式：必须有 `INCOMPAT_FILETYPE`，只接受当前实现认识的
compat/ro-compat/incompat 位；只把普通文件和线性目录映射成 VFS inode。符号链接、设备
inode、压缩/extent/inline-data inode 以及带 `EXT2_INDEX_FL` 的 HTree 目录会被拒绝，而不再
错误地当作普通文件解释。

## 外置事务日志（xjournal）

ext2 本身没有 journal。当 SD 卡上存在原始 xv6 根分区（p2）时，内核把 p2 中 xv6 镜像之后的
空闲区（分区偏移 `FSSIZE*2` 扇区起，共 1034 扇区）作为 ext2 的**外置日志**，实现位于
`kernel/fs/ext2/xjournal.c`：

```text
p2: [ xv6 根文件系统 32 MiB | 描述符 9 | 载荷 ≤1024 扇区 | 提交记录 1 | 未用 ]
```

每个修改型 ext2 操作（create、write、truncate、mkdir、unlink、rename、孤儿回收）是一个事务：

- 元数据扇区（superblock、组描述符、位图、inode、目录块、间接块）写入事务缓冲，后续读取先查缓冲；
- 文件数据块直接写回原位（ordered 模式），在提交前到达卡上；
- 提交顺序：描述符+载荷 → flush → 提交记录（提交点）→ flush → 写回原位 → flush → 清提交记录 → flush；
- 操作中途失败时整个事务被丢弃，因此 rename/unlink/mkdir 不再留下半完成状态；
- `ext2init()` 现在只注册 `ext2` 文件系统类型；真正的 `setup_ext2()`、外置日志初始化和
  回放都在 `ext2_fill_super(dev)` 挂载实例时进行，分区范围来自
  `blkdev_get(dev)->start/nsect`；
- 挂载时检查提交记录，校验通过就重放（`xjournal: replayed seq=N (K sectors)`）。没有 commit
  属于正常情况；无效且未提交的 commit sector 可清除，但一旦发现有效 commit，描述符、载荷、
  checksum、home checkpoint 或 flush 任一步失败都会保留记录并使挂载失败，绝不暴露未恢复的
  ext2；
- 超大 truncate/write 超出单事务容量时，在 inode 与已释放块一致的位置分段提交。

日志位置由 `ext2_fill_super()` 通过 `rootdev_xv6_part()` 取得 p2 的块设备实例，日志起点是
该分区内 `FSSIZE*2` 扇区。真机分区表（p2 起点 208896）上应打印 `xjournal: ready ... lba=274432`；
QEMU 测试镜像（p2 起点 133120）上是 `lba=198656`。

没有原始 xv6 根分区（QEMU 裸镜像或旧的 FS.IMG 模式）时打印
`ext2: no raw xv6 root partition; external journal disabled`，退回 bcache 同步写。
`/dev/sdroot` 只写前 `FSSIZE` 块，不会覆盖日志区。

普通 ext2 I/O 不再直接调用 `sdsector()`。`e2_rsector()`/`diskbytes()` 只是 512 B 元数据视图，
底层会定位到所属 ext2 block，再用 `bread()` 读取；非日志写用 `bwrite()`。因此
`sectorbuf` 是解析跨扇区字段的实例私有暂存区，`struct buf` 才是共享块缓存，并不是两份长期
缓存。xjournal 的日志区仍按 512 B sector I/O 保存既有磁盘格式，但 checkpoint home block 时
也使用 `bread()/bwrite()`，不会绕过 bcache。

## fsync、flush 与写屏障

有外置日志时，每个操作返回前已经同步提交；`fsync()`/`fdatasync()` 只需调用 `sdflush()`。
SDHOST 持有全局块设备锁，执行 ARM `dsb sy`，再循环发送 CMD13，直到卡报告
`READY_FOR_DATA=1` 且回到 TRAN 状态。这保证之前的 CMD24 已完成卡内编程，并阻止之后的写
越过该屏障。

原生 xv6 write-ahead log 也在四个位置调用同一个屏障：

```text
log payload -> flush -> commit header -> flush
            -> home blocks -> flush -> clear header -> flush
```

SD 6.x 的可选 Performance Enhancement Cache 需要另外读取扩展寄存器并执行它定义的 Flush Cache。
当前驱动没有启用该可选 cache，因此 CMD13 是当前支持卡型的持久化边界。

## 并发与打开文件语义

- 同一 ext2 实例的操作由睡眠锁 `ext2_fs.lock` 串行化。SD 传输是轮询 PIO，旧的自旋锁会让持锁 CPU 在整个
  操作（含最长 5 s 的 flush 等待）期间关中断；
- VFS 打开普通文件时通过 `ext2open()` 绑定 inode 号，此后读写、fstat、ftruncate 都按 inode
  进行，不再按路径重新查找；
- 文件在打开期间被 unlink 或被 rename 覆盖时成为孤儿：目录项立即消失，旧描述符仍可读写原数据，
  最后一次 close 时释放块和 inode（Unix 语义）。崩溃时尚未回收的孤儿 inode 需要 `e2fsck` 回收；
- inode 表偏移按 64 位计算（大分区上 inode 表可能位于 4 GiB 之后）。

## WAL 断电恢复测试

`/bin/waltest` 是一个小型 REDO WAL 示例，顺序严格为：

```text
写 redo.wal -> fdatasync(WAL) -> 写 data.db -> fsync(data) -> 清 WAL -> fsync(WAL)
```

普通测试：

```sh
waltest init
waltest commit 42
waltest show
waltest fsops       # mkdir/rename/unlink/ftruncate/pread/pwrite
waltest semantics   # rename 覆盖、目录移入自身子树被拒绝、unlink 已打开文件
waltest large       # 5 GiB 稀疏文件，实际走 triple-indirect
waltest jstress     # 无限循环的元数据操作，用于外置日志断电测试
```

真实断电测试不能用正常 `exit` 代替。使用：

```sh
waltest init
waltest prepare 123
# 看到 STOP-AFTER-WAL 后直接断电
# 重新启动并挂载 p3
waltest recover
```

`prepare` 只持久化已校验的 WAL，不写 `data.db`；重启后 `recover` 必须输出
`REDO ... value=123`，随后清空并再次 fsync WAL。

外置日志的断电测试：运行 `waltest jstress`，在任意时刻断电并重启。若断电恰好落在“已提交、
未写回”的窗口，启动日志会出现 `xjournal: replayed ...`。之后在开发机执行
`e2fsck -fn` 应无错误。QEMU 中用同样布局的 SD 镜像随机断电 14 次：3 次落在该窗口（断电后
立即检查 `e2fsck` 报错），重启重放后全部无错误；其余 11 次断电前后都无错误。拆分 `kernel/drivers/block/rootdev.c` 之后又随机断电 4 次：1 次重放
（`xjournal: replayed seq=85 (27 sectors)`），4 次 `e2fsck -fn` 全部无错误。

## mini SQLite 风格页式数据库压力测试

`/bin/minisqlite` 不是完整 SQLite，也不解析 SQL。它是为了验证 ext2/VFS 持久化语义而实现的
小型页式键值数据库：数据库由一个 4 KiB header page 和 256 个 4 KiB 数据页组成，每页
64 个固定大小的槽；整数主键使用开放寻址查找。数据库文件约 1 MiB，随机 key 会落到不同
页，从而覆盖 ext2 的随机 `pread/pwrite`、首次数据块分配、间接块映射和 `fsync`。

每次 `put/delete` 都使用一页 redo WAL，提交顺序与 SQLite WAL 的核心持久化原则相同：

```text
构造完整 WAL record
  -> pwrite(main.wal)
  -> fsync(main.wal)                 WAL 成为恢复依据
  -> pwrite(main.db slot + header)
  -> fsync(main.db)                  数据库页持久化
  -> ftruncate(main.wal, 0)
  -> fsync(main.wal)                 完成 checkpoint
```

常用命令：

```sh
minisqlite init
minisqlite put 10 1234
minisqlite get 10
minisqlite delete 10
minisqlite scan                       # 输出所有行及 page/slot
minisqlite insert 11 5678             # put 的 SQL 风格别名
minisqlite select 11                  # get 的 SQL 风格别名
minisqlite select all                 # scan 的 SQL 风格别名
minisqlite check                      # 校验 header、所有 slot checksum 和行数
minisqlite stress 1000                # 70% put、20% get、10% delete
```

`stress` 使用固定随机种子和 512 个 key 的内存真值表。每个事务完成后都经过 WAL 和两个
`fsync` 边界，最后逐 key 重读并全表扫描，检测丢写、错误覆盖、哈希冲突处理和 header 行数
不一致。因为它会执行大量同步 SD 写，真机速度较慢是预期行为；可先运行 `stress 100`。

断电恢复测试：

```sh
minisqlite init
minisqlite prepare 7 777
# 看到 "WAL durable" 后直接断电，不要正常关机
# 重启并重新挂载 ext2：
minisqlite recover
minisqlite get 7
```

`prepare` 只持久化 redo WAL，不修改 `main.db`。`recover` 校验 WAL 后把目标 slot 和 header
幂等重放到数据库，再执行 `fsync(main.db)` 并清空 WAL。数据库自身的 WAL 测试应用层事务；
ext2 的 xjournal 同时保护目录、inode、位图和间接块等文件系统元数据，两层日志职责不同。

## 当前边界

当前实现仍是小型 ext2 后端，不等同于 Linux ext2：

- 不支持硬链接、符号链接、ACL/xattr、quota、HTree 大目录和 GPT；
- 不支持 ext3 journal、ext4 extent、64 位块号和 metadata checksum；
- 外置日志只有本内核认识：Linux 挂载前若日志中还有已提交未写回的事务，需要先在 xv6 中启动一次
  让它重放；
- 孤儿 inode 没有 ext3 那样的磁盘孤儿链表，崩溃后由 `e2fsck` 回收；
- 已支持非根挂载的 clean `umount`；第一次修改会清除 superblock 的 `EXT2_VALID_FS`，正常
  `umount` 会以最后一个元数据事务重新设置该位并执行 flush。强制断电、根文件系统无法卸载或
  卸载失败时，接回开发机仍应执行 `e2fsck -f`。如果挂载时该位原本已经是 dirty，本次卸载会
  保留 dirty（不会用一次正常卸载替代 `e2fsck`）。

## 创建兼容分区

确认 `/dev/disk4s3` 是专门用于 ext2 的分区后执行：

```sh
diskutil unmount /dev/disk4s3
sudo /opt/homebrew/opt/e2fsprogs/sbin/mke2fs \
  -F -t ext2 -b 4096 \
  -O filetype,sparse_super,large_file,^has_journal,^extent,^64bit,^metadata_csum \
  -L xv6-linux /dev/rdisk4s3
```

格式化会销毁 p3 原有数据，不能对 `/dev/disk4`、`/dev/rdisk4` 或 p2 执行。格式化后先检查：

```sh
sudo /opt/homebrew/opt/e2fsprogs/sbin/e2fsck -fn /dev/rdisk4s3
sudo /opt/homebrew/opt/e2fsprogs/sbin/dumpe2fs -h /dev/rdisk4s3
```

预期启动日志：

```text
ext2: p2 has no ext superblock
ext2: p3 ready rw start=... sectors=... block=4096 groups=...
vfs: mounted ext2 at /mnt/ext2 read-write
```

核心实现位于 `kernel/fs/ext2/ext2.c`，VFS 适配位于 `kernel/fs/vfs.c`。


## 代码结构与调用链

```text
用户程序 open/read/write/rename/unlink/mkdir/ftruncate/fsync/umount
  -> sysfile.c
  -> vfs.c        vnode_ops：按路径操作 + 句柄操作 open/release/hstat/hread/hwrite/htruncate
  -> ext2.c       super_block.priv -> 当前设备的 ext2_fs
                  acquiresleep(&fs->lock)
                  tx_begin()                       -> xj_begin()
                  *_locked()                       ext2 逻辑：目录项、inode、位图、间接块
                    readblock/writeblock           元数据
                    writeblock_data                文件数据（e2.direct=1，绕过日志）
                      -> diskbytes/diskbytes_write
                      -> e2_rsector/e2_cache_write
                      -> bread/bwrite              4 KiB ext2 block cache
                                                  日志开启时元数据先走 xj_read/xj_write
                  tx_end(ok)                       -> xj_commit() 或 xj_abort()
                  releasesleep(&fs->lock)
  -> xjournal.c   描述符+载荷 -> sdflush -> 提交记录 -> sdflush
                  -> bread/bwrite checkpoint home -> sdflush -> 清提交 -> sdflush
  -> sd.c         块设备实际 I/O（CMD17/CMD24），sdflush()（dsb + CMD13 轮询）

umount /mnt/ext2
  -> sys_umount -> vfsumount -> fs_umount
  -> mount.unmounting=1，拒绝新路径进入
  -> 检查子挂载、itable 引用（open/cwd/活动 inode）
  -> super_block.put_super = ext2_put_super
  -> EXT2_VALID_FS clean transaction -> sdflush
  -> xj_shutdown -> binvalidate -> block size 恢复 512 B
  -> 释放 blockbuf/三级 ptrbuf，注销 superblock，释放覆盖点 inode
```

| 文件 | 主要函数 |
|---|---|
| `kernel/fs/ext2/ext2.c` | `ext2_fill_super/setup_ext2` 解析并严格校验 superblock/root inode；每设备 `ext2_fs`；`readinode/writeinode`（64 位偏移）；`fileblock/set_fileblock/clear_fileblock` 直接～三级间接块；`alloc_block/free_block/alloc_inode/free_inode`；`e2dirlookup/e2diradd/e2dirremove/e2dirreplace`；`tx_begin/tx_end/tx_checkpoint`；`ext2open/ext2release` 打开表与孤儿回收；`ext2_put_super` clean 卸载 |
| `kernel/fs/ext2/xjournal.c` | `xj_init`（`recover` 失败即拒绝挂载）、`xj_begin/xj_write/xj_read/xj_contains/xj_abort/xj_commit`、`xj_space`、`xj_shutdown` |
| `kernel/drivers/block/rootdev.c` | `rootdev_xv6_part()` 提供 p2 块设备，日志区就在 xv6 镜像之后 |
| `kernel/drivers/mmc/host/sd.c` | `sdflush()` 写屏障 |
| `kernel/fs/fs.c` / `kernel/fs/vfs.c` | 统一 mount table、busy 检查、`super_block.put_super`、`fs_umount/vfsumount` |
| `user/waltest.c` | WAL 示例与 `fsops/semantics/large/jstress` 测试 |

### 断电时刻与恢复结果

| 断电发生在 | 启动时 |
|---|---|
| 写描述符/载荷期间或之后（提交记录未写） | 无事可做，操作视为没发生 |
| 写提交记录期间 | commit checksum 无效，未达到提交点，清除无效记录 |
| 写回原位期间 | 重放：`xjournal: replayed seq=N (K sectors)` |
| 清除提交记录期间 | 再重放一次（幂等），结果相同 |

如果 commit record 本身已经有效，但 descriptor/payload/checksum 或 home checkpoint 无法读取/校验，
内核会输出 `xjournal: recovery failed; refusing ext2 mount`。此时不能把它当成“未提交”丢弃，
因为事务已经越过提交点。

### QEMU 断电测试步骤

```sh
# 宿主机：准备与真机同布局的 256 MiB 镜像 sd.img（p1 FAT32、p2 xv6、p3 ext2）
# xv6 中：
waltest init
waltest jstress          # 随机时刻直接杀掉 qemu 进程
# 重新启动同一镜像，观察是否有 xjournal: replayed，然后在宿主机检查 p3：
dd if=sd.img of=p3.img bs=512 skip=215040 count=309248
e2fsck -fn p3.img        # 退出码 0 = 无错误
```

书中对应章节：`book/xv6-book/chapters/09-optional.tex`（第 9 章 可选工作）。
