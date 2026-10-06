# xv6 的 ext2 文件系统

本项目把 SD 卡上的传统 ext2 分区接入 VFS，默认挂载到 `/mnt/ext2`。它不替换 p2
上的 xv6 原生根文件系统：

```text
SD card
├─ p1 FAT32  -> /boot
├─ p2 XV6FS  -> /
├─ p3 ext2   -> /mnt/ext2
└─ p4/SPFS   -> reserved
```

内核扫描 MBR 中所有 type `0x83` 的分区，并通过 `0xef53` superblock 和 feature bits
判断是否为兼容的 ext2。因此 p2 即使也使用 `0x83` 占位，也不会被误认为 ext2。

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
```

原来的诊断命令仍然保留：

```sh
ext2ls /
ext2cat /hello.txt
```

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

所有 ext2 元数据访问由 `e2.lock` 串行化，避免多个 CPU 同时修改 bitmap。写入范围还会
同时检查分区扇区边界、superblock block 数和当前实现支持的最大文件大小。

## 外置事务日志（xjournal）

ext2 本身没有 journal。当 SD 卡上存在原始 xv6 根分区（p2）时，内核把 p2 中 xv6 镜像之后的
空闲区（分区偏移 `FSSIZE*2` 扇区起，共 1034 扇区）作为 ext2 的**外置日志**，实现位于
`kernel/xjournal.c`：

```text
p2: [ xv6 根文件系统 32 MiB | 描述符 9 | 载荷 ≤1024 扇区 | 提交记录 1 | 未用 ]
```

每个修改型 ext2 操作（create、write、truncate、mkdir、unlink、rename、孤儿回收）是一个事务：

- 元数据扇区（superblock、组描述符、位图、inode、目录块、间接块）写入事务缓冲，后续读取先查缓冲；
- 文件数据块直接写回原位（ordered 模式），在提交前到达卡上；
- 提交顺序：描述符+载荷 → flush → 提交记录（提交点）→ flush → 写回原位 → flush → 清提交记录 → flush；
- 操作中途失败时整个事务被丢弃，因此 rename/unlink/mkdir 不再留下半完成状态；
- 启动时 `ext2init()` 在解析任何 ext2 元数据前先检查提交记录，校验通过就重放
  （`xjournal: replayed seq=N (K sectors)`），校验失败的残缺事务被丢弃；
- 超大 truncate/write 超出单事务容量时，在 inode 与已释放块一致的位置分段提交。

日志位置由 `ext2init()` 向根块设备查询：`rootdev_raw_info()`（`kernel/rootdev.c`）返回 p2 的起点和大小，
日志起点 = p2 起点 + `FSSIZE*2`。真机分区表（p2 起点 208896）上应打印 `xjournal: ready lba=274432`；
QEMU 测试镜像（p2 起点 133120）上是 `lba=198656`。

没有原始 xv6 根分区（QEMU 裸镜像或旧的 FS.IMG 模式）时打印
`ext2: no raw xv6 root partition; external journal disabled`，退回逐扇区同步写。
`/dev/sdroot` 只写前 `FSSIZE` 块，不会覆盖日志区。

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

- 所有 ext2 操作由睡眠锁 `e2.lock` 串行化。SD 传输是轮询 PIO，旧的自旋锁会让持锁 CPU 在整个
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
立即检查 `e2fsck` 报错），重启重放后全部无错误；其余 11 次断电前后都无错误。拆分 `kernel/rootdev.c` 之后又随机断电 4 次：1 次重放
（`xjournal: replayed seq=85 (27 sectors)`），4 次 `e2fsck -fn` 全部无错误。

## 当前边界

当前实现仍是小型 ext2 后端，不等同于 Linux ext2：

- 不支持硬链接、符号链接、ACL/xattr、quota、HTree 大目录和 GPT；
- 不支持 ext3 journal、ext4 extent、64 位块号和 metadata checksum；
- 外置日志只有本内核认识：Linux 挂载前若日志中还有已提交未写回的事务，需要先在 xv6 中启动一次
  让它重放；
- 孤儿 inode 没有 ext3 那样的磁盘孤儿链表，崩溃后由 `e2fsck` 回收；
- 尚无 `umount`。第一次修改时驱动会清除 superblock 的 `EXT2_VALID_FS`，接回开发机时仍应执行
  `e2fsck -f`。

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

核心实现位于 `kernel/ext2.c`，VFS 适配位于 `kernel/vfs.c`。


## 代码结构与调用链

```text
用户程序 open/read/write/rename/unlink/mkdir/ftruncate/fsync
  -> sysfile.c
  -> vfs.c        vnode_ops：按路径操作 + 句柄操作 open/release/hstat/hread/hwrite/htruncate
  -> ext2.c       acquiresleep(&e2.lock)
                  tx_begin()                       -> xj_begin()
                  *_locked()                       ext2 逻辑：目录项、inode、位图、间接块
                    readblock/writeblock           元数据
                    writeblock_data                文件数据（e2.direct=1，绕过日志）
                      -> diskbytes/diskbytes_write
                      -> e2_rsector/e2_wsector     日志开启时元数据走 xj_read/xj_write
                  tx_end(ok)                       -> xj_commit() 或 xj_abort()
                  releasesleep(&e2.lock)
  -> xjournal.c   描述符+载荷 -> sdflush -> 提交记录 -> sdflush -> 写回原位 -> sdflush -> 清提交 -> sdflush
  -> sd.c         sdsector()（CMD17/CMD24），sdflush()（dsb + CMD13 轮询）
```

| 文件 | 主要函数 |
|---|---|
| `kernel/ext2.c` | `setup_ext2` 解析 superblock；`readinode/writeinode`（64 位偏移）；`fileblock/set_fileblock/clear_fileblock` 直接～三级间接块；`alloc_block/free_block/alloc_inode/free_inode`；`e2dirlookup/e2diradd/e2dirremove/e2dirreplace`；`tx_begin/tx_end/tx_checkpoint`；`ext2open/ext2release` 打开表与孤儿回收；`rename_locked/is_ancestor` |
| `kernel/xjournal.c` | `xj_init`（含 `recover`）、`xj_begin/xj_write/xj_read/xj_contains/xj_abort/xj_commit`、`xj_space` |
| `kernel/rootdev.c` | `rootdev_raw_info()` 提供 p2 位置，日志区就在 xv6 镜像之后 |
| `kernel/sd.c` | `sdflush()` 写屏障 |
| `kernel/vfs.c` | 句柄型 vnode：`vfsopen` 绑定 inode，`vfsclose` 调 `release` |
| `user/waltest.c` | WAL 示例与 `fsops/semantics/large/jstress` 测试 |

### 断电时刻与恢复结果

| 断电发生在 | 启动时 |
|---|---|
| 写描述符/载荷期间或之后（提交记录未写） | 无事可做，操作视为没发生 |
| 写提交记录期间 | 提交记录校验失败，丢弃 |
| 写回原位期间 | 重放：`xjournal: replayed seq=N (K sectors)` |
| 清除提交记录期间 | 再重放一次（幂等），结果相同 |

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
