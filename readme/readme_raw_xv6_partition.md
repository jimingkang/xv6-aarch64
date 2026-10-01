# Raspberry Pi 3 独立 xv6 原始分区：格式化与部署

本文记录 2026-09-30 对 `/dev/disk4` 实际执行的重新格式化和四分区过程。所有设备名
和容量都只适用于当时插入的 63.8 GB 外置 SD 卡；换卡后必须重新确认，不能直接复制
执行写盘命令。

## 1. 目标布局

目标是让 bootfs、xv6 根文件系统、ext2 和实验性 SPFS 各自占用独立分区：

```text
SD card /dev/disk4
├─ BOOTFS   100 MiB    FAT32    Raspberry Pi firmware、内核和 Wi-Fi firmware
├─ XV6FS    400 MiB    raw      xv6 原生文件系统镜像
├─ EXT2     remainder  ext2     Linux/ext2 文件系统实验
└─ SPFS     500 MiB    raw      预留给 stevedpate/spfs
```

SPFS 是教学型 Linux 文件系统，固定块大小为 2048 字节、整个文件系统只有 760 块。
500 MiB 远大于它当前需要的空间，主要是为后续移植和实验预留。

## 2. 格式化前确认磁盘

先执行只读检查：

```sh
diskutil list /dev/disk4
diskutil info /dev/disk4
diskutil info /dev/disk4s1
```

当时确认的信息是：

```text
Device Location: external
Removable Media: Yes
Disk Size: 63.8 GB
Exact sectors: 124669952 × 512 bytes
```

格式化前的卡只有一个占满全盘的 FAT32 `BOOTFS`，其中没有有效的 Raspberry Pi 启动
文件，只有 `.Spotlight-V100` 和 `.fseventsd`。如果 bootfs 中存在
`bootcode.bin`、`start.elf`、DTB、overlay 或自定义文件，必须先复制到其他磁盘。

## 3. 第一次卸载失败

直接运行 `diskutil partitionDisk` 时曾出现：

```text
The volume on disk4 couldn't be unmounted because it is in use by process mds
```

`mds` 是 macOS Spotlight。解决方法是先强制卸载整卡：

```sh
diskutil unmountDisk force /dev/disk4
```

不要只卸载某一个分区；重新写分区表前应确保整张卡的所有卷都已卸载。

## 4. 建立四分区 MBR

实际使用的命令是：

```sh
diskutil partitionDisk /dev/disk4 4 MBR \
  FAT32 BOOTFS 100MiB \
  UFSD_EXTFS XV6FS 400MiB \
  UFSD_EXTFS EXT2 R \
  UFSD_EXTFS SPFS 500MiB
```

其中 `R` 表示把扣除其他固定分区和对齐空间后的剩余容量全部交给 EXT2。macOS
`diskutil` 不能直接建立任意 MBR type `0x7f` 的未格式化分区，因此先用 Linux/ext
类型占位；随后 `fs.img` 会覆盖 XV6FS 的临时 ext 元数据，SPFS 分区则清除文件系统
签名后保留。

## 5. 实际分区结果

执行后 `diskutil list /dev/disk4` 显示：

```text
/dev/disk4 (external, physical):
   #:                       TYPE NAME       SIZE       IDENTIFIER
   0:     FDisk_partition_scheme           *63.8 GB   disk4
   1:                 DOS_FAT_32 BOOTFS      104.9 MB  disk4s1
   2:                      Linux             419.4 MB  disk4s2
   3:                      Linux              62.8 GB  disk4s3
   4:                      Linux             520.1 MB  disk4s5
```

实际扇区位置为：

| 用途 | macOS 节点 | start LBA | sectors | 实际容量 |
|---|---|---:|---:|---:|
| BOOTFS | `/dev/disk4s1` | 2048 | 204800 | 100 MiB |
| XV6FS | `/dev/disk4s2` | 208896 | 819200 | 400 MiB |
| EXT2 | `/dev/disk4s3` | 1030144 | 122621952 | 约 58.47 GiB |
| SPFS reserve | `/dev/disk4s5` | 123654144 | 1015808 | 496 MiB |

最后一个分区显示为 `disk4s5` 而不是 `disk4s4`，是因为 diskutil 用 MBR extended
partition 容纳第四个数据卷，`s4` 是扩展容器，内部第一个 logical partition 编号为
`s5`。以后格式化 SPFS 必须使用 `/dev/rdisk4s5`。

## 6. 清除 SPFS 预留区

分区时 SPFS 使用 ext 类型占位。随后执行：

```sh
diskutil zeroDisk short /dev/disk4s5
```

该操作清除已有文件系统签名，但保留 MBR 分区本身。`diskutil info` 仍可能根据 MBR
type 把它显示为 Linux/ext personality，这不表示其中仍有一个可挂载的 ext 文件系统。
以后 SPFS 的 `mkfs` 应直接格式化 `/dev/rdisk4s5`。

## 7. 生成并烧录 xv6 根文件系统

先在源码目录生成镜像：

```sh
make fs.img
shasum -a 256 fs.img
```

当前`FSSIZE=32768`，每块1024字节，生成的镜像大小为32 MiB。400 MiB的分区2
足以容纳该文件系统；剩余分区容量是预留扩容空间，不会自动计入superblock。
将镜像写入 **分区 2**，不能写入整卡：

```sh
sudo dd \
  if=/Users/jimingkang/Documents/xv6-aarch64/fs.img \
  of=/dev/rdisk4s2 \
  bs=1048576 conv=sync
sync
```

也可以使用 Makefile 中带目标检查的命令：

```sh
make install-rpi3-rawfs RPI3_XV6_DEV=/dev/rdisk4s2
```

禁止使用以下目标：

```text
/dev/disk4
/dev/rdisk4
```

它们代表整张卡，写错会覆盖 MBR 和全部四个分区。

## 8. 明确建立 ext2

虽然 `diskutil` 使用 ext personality 建立了 p3，仍应使用 Homebrew e2fsprogs 明确
创建 ext2，避免出现 xv6 ext2 驱动不支持的 ext4 incompat features：

```sh
sudo /opt/homebrew/opt/e2fsprogs/sbin/mke2fs \
  -F -t ext2 -L EXT2 /dev/rdisk4s3
```

验证：

```sh
sudo /opt/homebrew/opt/e2fsprogs/sbin/dumpe2fs -h /dev/rdisk4s3
sudo /opt/homebrew/opt/e2fsprogs/sbin/e2fsck -fn /dev/rdisk4s3
```

应确认 `Filesystem magic number` 为 `0xEF53`，并且没有 extents、64bit、metadata
checksum 等当前 xv6 ext2 驱动不支持的 incompat feature。

## 9. macOS 权限与隐私限制

普通进程没有 raw device 写权限：

```text
crw-r----- root operator /dev/rdisk4s2
```

因此 `dd` 和 `mke2fs` 必须通过用户自己的 Terminal 执行 `sudo`。不要把管理员密码
发送到聊天中。通过 AppleScript 启动的管理员 shell 还可能因 macOS TCC 隐私保护而
无法读取 `Documents`，出现：

```text
dd: .../fs.img: Operation not permitted
```

这种情况下可先复制到临时目录：

```sh
cp fs.img /private/tmp/xv6-fs.img
sudo dd if=/private/tmp/xv6-fs.img of=/dev/rdisk4s2 \
  bs=1048576 conv=sync
```

## 10. 内核识别逻辑

内核不再要求根文件系统必须位于 FAT32 `FS.IMG`。启动时会：

```text
读取 MBR
  -> 跳过 FAT32 和 extended partition
  -> 检查其他 primary partition
  -> 读取 partition block 1
  -> 验证 FSMAGIC 和 FSSIZE
  -> 根文件系统块直接映射到 partition_start + block_offset
```

macOS 给 XV6FS 保留的 MBR type 是 Linux `0x83`，但它不会和真正的 ext2 混淆：只有
xv6 superblock magic 匹配的分区才会成为根文件系统。

预期启动日志：

```text
xv6fs: selected raw p2 lba=208896 sectors=819200 size=400 MiB
fat32: selected p1 lba=2048 firmware-only (no FS.IMG)
```

此后 xv6 文件写入直接落到 p2，不再修改 FAT32 的 FAT、目录项或 cluster chain。

## 11. 恢复 BOOTFS

新建的 BOOTFS 是空的。树莓派启动前必须恢复 Raspberry Pi firmware，包括
`bootcode.bin`、`start.elf`、对应 DTB 和 overlays，然后安装本项目内核、配置和 Wi-Fi
firmware：

```sh
make install-rpi3 RPI3_XV6_DEV=/dev/rdisk4s2
```

`install-rpi3` 可以安装项目生成的内核、`config.txt`、Wi-Fi firmware 和 xv6 根镜像，
但它不会自动下载 Raspberry Pi 官方 boot firmware；这些基础启动文件必须事先放入
`/Volumes/BOOTFS`。
