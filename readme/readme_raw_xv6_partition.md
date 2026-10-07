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

`install-rpi3` 可以安装项目生成的内核、`config.txt`、Wi-Fi firmware 和 xv6 根镜像。
它不再把 `fs.img` 复制成 bootfs 中的 `FS.IMG`；根文件系统只允许写入明确
指定的原始 xv6 分区；没有设置 `RPI3_XV6_DEV` 时仍会更新内核、配置和 firmware，但会跳过
根文件系统并打印警告。这避免 100 MiB bootfs 被 32 MiB 镜像占满，也避免重新引入 FAT32
容器模式。
但它不会自动下载 Raspberry Pi 官方 boot firmware；这些基础启动文件必须事先放入
`/Volumes/BOOTFS`。

## 12. 在 xv6 内在线更新 p2 根文件系统

安装过本版本后，可以不再把 SD 卡拔到 macOS 上写 `rdisk4s2`。内核注册了受保护的
字符设备 `/dev/sdroot`，用户命令 `/bin/dd` 可将 TFTP 下载到 `/boot` 的新镜像写入
启动时实际选中的 raw xv6 根分区。正常布局中该分区就是 MBR p2；驱动使用 MBR 探测
得到的 LBA，不写死某一张卡的扇区号，也绝不会暴露整张 SD 卡。

先在开发机生成新镜像，并放入 TFTP 服务目录。默认 `make` 的 `install-tftp` 阶段会
同时同步内核和 `fs.img`：

```sh
make
```

若只想同步 TFTP 文件，可执行 `make install-tftp`。该目标写入的是开发机的
`/private/tftpboot`，不是 SD 卡的 FAT32 bootfs。

然后在 Raspberry Pi 上下载并安装：

```sh
/bin/tftpclient 192.168.0.195 fs.img
/bin/dd
```

等价的完整写法是：

```sh
/bin/dd if=/boot/fs.img of=/dev/sdroot bs=32768
```

`dd` 在真正写 p2 前会依次完成：

```text
检查 /boot/fs.img 恰好为 FSSIZE * BSIZE（当前 32 MiB）
  -> 把完整镜像读入用户内存
  -> 校验 xv6 superblock magic、size 和布局字段
  -> 打开仅允许写入当前 raw root 的 /dev/sdroot
  -> 停止新根文件系统事务，等待已有事务提交和读操作退出
  -> 每次写一个 512-byte SD 扇区
  -> 立即读回该扇区并逐字节比较
  -> 完成后保持旧根冻结，要求断电重启
```

完整预读非常重要：一旦开始覆盖 p2，就不能再从旧根加载 `/bin/dd` 的代码、库或读取
其他文件。当前用户程序是静态链接的，镜像又已经全部进入内存，因此后续只需要内核、
SD 驱动和串口输出。更新期间仍可看到每 4 MiB 一次的装载与回读校验进度。

成功日志末尾应类似：

```text
sdroot: verified 32/32 MiB
sdroot: update complete and verified; reboot now
dd: 33554432 bytes installed and read-back verified
dd: root filesystem is frozen; power-cycle Raspberry Pi now
```

注意：

- 写入过程中不能拔卡、断电或复位；中断写入会留下不完整的根文件系统。
- 命令只接受 `of=/dev/sdroot`，故意不实现任意块设备和整盘写入。
- 只支持独立 raw root 模式；仍从 FAT32 `FS.IMG` 启动时，设备会拒绝打开。
- 写入一旦开始，根文件系统冻结是单向的；无论成功还是失败都必须断电重启。
- 第一次取得 `/bin/dd` 和 `/dev/sdroot` 仍需从 macOS 执行一次
  `make install-rpi3 RPI3_XV6_DEV=/dev/rdisk4s2`。以后才可使用上述在线更新流程。


## 13. 2026-10-06：在 128 GB 卡上用 fdisk 增加 ext2 分区 p3

本节记录另一张卡（`/dev/disk4`，250347520 扇区，约 119 GiB）的操作。设备名和扇区号只适用于
这张卡，换卡后必须重新确认。

### 13.1 原始布局

```text
$ sudo fdisk /dev/disk4
Disk: /dev/disk4  geometry: 15583/255/63 [250347520 sectors]
Signature: 0xAA55
         Starting       Ending
 #: id  cyl  hd sec -  cyl  hd sec [     start -       size]
------------------------------------------------------------------------
 1: 0C  128   0   1 - 1023   3  32 [     16384 -    1048576] Win95 FAT32L
 2: 83 1023   3  32 - 1023 127  63 [   1064960 -  249282560] Linux files*
 3: 00    0   0   0 -    0   0   0 [         0 -          0] unused
 4: 00    0   0   0 -    0   0   0 [         0 -          0] unused
```

p2 是 xv6 原始根分区，一直占到卡尾。xv6 只用到其中开头的 65536 扇区（`FSSIZE=32768` 块 × 2）
和紧随其后的 1034 扇区 ext2 外置日志（xjournal），后面全部空闲。

### 13.2 为什么不用 diskutil

`diskutil addPartition` 只支持 GPT 分区表，而树莓派 3 的启动卡必须是 MBR。macOS 也不认识 p2 的
格式，无法用 `diskutil resizeVolume` 缩小它。因此改用 macOS 自带的 `fdisk -e` 直接修改 MBR
表项：只改分区表，不移动、不改写任何数据。

改动前先确认 p2 确实是 xv6（块 1 的 superblock 魔数为 `0x10203040`、`FSSIZE` 为 32768），并备份：

```sh
sudo dd if=/dev/rdisk4s2 bs=512 skip=2 count=1 2>/dev/null | xxd | head -1
# 应以 4030 2010 0080 0000 开头
sudo dd if=/dev/rdisk4s2 of=~/xv6fs-p2-backup.img bs=512 count=66570
```

如果 p2 是 Raspberry Pi OS 的 ext4（偏移 1080 处为 `53 ef`），不能缩小它的表项。

### 13.3 新布局

| 分区 | 类型 | start LBA | sectors | 容量 | 说明 |
|---|---|---:|---:|---:|---|
| p1 | 0x0C | 16384 | 1048576 | 512 MiB | BOOTFS，不变 |
| p2 | 0x7F | 1064960 | 819200 | 400 MiB | xv6 根，起点不变，只缩小表项 |
| p3 | 0x83 | 1884160 | 248463360 | 约 118.5 GiB | ext2；1884160 是 2048 的倍数，正好接在 p2 之后，结束于卡尾 |

1884160 + 248463360 = 250347520，即整卡扇区数。p2 改为 `0x7F` 只是标明用途：rootdev 优先选
0x7f，`ext2init()` 只尝试 0x83 分区，因此不会再打印 `ext2: p2 has no ext superblock`。

### 13.4 fdisk 操作

```text
$ diskutil unmountDisk /dev/disk4
$ sudo fdisk -e /dev/disk4
fdisk: could not open MBR file /usr/standalone/i386/boot0: No such file or directory
Enter 'help' for information
fdisk: 1> edit 2
  Partition id: 7F
  Do you wish to edit in CHS mode? n
  Partition offset: 1064960
  Partition size: 819200
fdisk:*1> edit 3
  Partition id: 83
  Do you wish to edit in CHS mode? n
  Partition offset: 1884160
  Partition size: 248463360
fdisk:*1> print
fdisk:*1> write
Device could not be accessed exclusively.
A reboot will be needed for changes to take effect. OK? [n] y
Writing MBR at offset 0.
fdisk: 1> quit
```

- `could not open MBR file .../boot0` 无害：这是 Intel Mac 的引导代码模板，Apple Silicon 上不存在。
  只有 `update`、`reinit` 才需要它；`edit`/`write` 保留原有引导代码和磁盘签名。**不要执行
  `reinit` 或 `update`。**
- `Device could not be accessed exclusively` 时回答 `y`。之后弹出并重新插卡，代替重启 Mac。

### 13.5 格式化 p3：整分区 mke2fs，而不是写 32 MiB 镜像

```sh
diskutil list                    # 重新插卡后确认磁盘号
diskutil unmount /dev/disk4s3
sudo /opt/homebrew/opt/e2fsprogs/sbin/mke2fs -F -t ext2 -b 4096 \
  -O filetype,sparse_super,large_file,^has_journal,^extent,^64bit,^metadata_csum \
  -L xv6-linux /dev/rdisk4s3
sudo /opt/homebrew/opt/e2fsprogs/sbin/e2fsck -fn /dev/rdisk4s3
sudo dd if=/dev/rdisk4s2 bs=512 skip=2 count=1 2>/dev/null | xxd | head -1   # xv6 superblock 仍在
```

`make install-rpi3-ext2 RPI3_EXT2_DEV=/dev/rdisk4s3` 写入的是 32 MiB 的 `fs_ext2.img`
（`EXT2_SIZE ?= 32M`）。它在 118 GiB 的分区里只建了一个 8192 块的文件系统，其余空间都用不到。
要用整个分区，应当像上面那样直接对 `/dev/rdisk4s3` 执行 `mke2fs`。已经写了镜像的话，也可以用
`resize2fs /dev/rdisk4s3` 扩展到整个分区。

### 13.6 第一次启动：日志、问题与分析

#### 日志

```text
ext2: mounted p3 read-only start=1884160 sectors=248463360 block=4096 groups=1
$ ls /mnt
.                              1 60 96
..                             1 1 1024
ext2                           1 2 4096
$ cd /mnt
exec cd failed
$ ls /mnt/ext2
exec ls failed
```

#### 分析

1. **卡上运行的是旧内核。** `ext2: mounted p%d read-only` 这种写法只存在于 `986aba1` 及以前的只读
   ext2 驱动，提交 `5097741` 就把它删了。当前源码构建的内核打印的是：

   ```text
   rootdev: raw xv6 root p2 lba=1064960 sectors=819200 size=400 MiB
   xjournal: ready lba=1130496 capacity=1024 sectors
   ext2: p3 ready rw start=1884160 sectors=248463360 block=4096 groups=948
   vfs: mounted ext2 at /mnt/ext2 read-write
   ```

   说明 BOOTFS 中的 `kernel8-xv6_wifi.img` 没有更新。旧内核没有可写 ext2、xjournal、`cwdpath`
   和 exec 走 VFS 这些功能。

2. **`groups=1` 说明 p3 上只是 32 MiB 的 ext2。** 4 KiB 块、每组 32768 块，只有总块数不超过
   32768（不超过 128 MiB）时才是 1 组。整分区 `mke2fs` 应有 248463360 / 8 = 31057920 块，
   即 948 组。这与写入 32 MiB 的 `fs_ext2.img` 一致。

3. **ext2 本身已识别：** `ls /mnt` 中 `ext2` 显示 inode 2、大小 4096，这是 ext2 根目录，说明挂载成功。

4. **`exec cd failed`、`exec ls failed` 与 ext2 无关。** `cd` 是 sh 的内建命令，只有输入行严格以
   `cd ` 开头才会被识别；`exec cd failed` 说明这一行没有被当作内建命令，而是被拆成程序名 `cd` 去
   `exec`。`ls /mnt/ext2` 失败在 exec `/bin/ls` 这一步，还没有访问 ext2（同一个 `ls` 刚刚成功
   列出了 `/mnt`）。最可能的原因是输入行里混入了终端没有处理掉的不可见字符（例如方向键、删除键
   产生的控制字符），使命令名不再等于 `cd`、`ls`；错误信息只打印出可见部分。重新逐字输入即可
   确认。如果更新内核后仍能复现，再抓取原始输入继续排查。

#### 解决

```sh
# 1. 在 macOS 上用当前源码重建并安装内核；新增用户程序（waltest 等）需要同时更新 p2
make
make install-rpi3 RPI3_XV6_DEV=/dev/rdisk4s2       # BOOTFS 挂载在 /Volumes/bootfs
# 2. 把 p3 格式化为整分区 ext2（见 13.5），或 resize2fs 扩展已有镜像
# 3. 启动后确认日志与 13.6 第 1 点中新内核的输出一致，然后：
cd /mnt/ext2
waltest init
waltest fsops
```

如果 `/etc/fstab` 里 ext2 一行仍是 `ro`，用 `edit /etc/fstab` 改为
`ext2 /mnt/ext2 ext2 rw 0 0` 后重启。

#### 结果

执行 `make install-rpi3 RPI3_XV6_DEV=/dev/rdisk4s2` 重新安装内核和 p2 根文件系统后，问题解决：
`cd /mnt`、`ls /mnt/ext2` 恢复正常。这证实第 1 点的判断——之前 BOOTFS 中是旧内核（旧 sh 和旧 VFS）。
`fs_ext2.img` 本身只是空的 32 MiB ext2（只有 `.`、`..`、`lost+found`），没有把 user 程序或 xv6
文件系统写进 p3；如需使用整个 p3，按 13.5 执行整分区 `mke2fs` 或 `resize2fs`。
