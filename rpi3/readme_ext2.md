# 最小 Linux 文件系统支持：只读 ext2

本实现把 ext2 作为独立的只读文件系统访问，不替换 xv6 原生根文件系统：

```text
SD card
  p1 FAT32 -> FS.IMG -> xv6 根文件系统（原有启动路径）
  p2 ext4  -> 检测到 ext4 特性后拒绝，绝不写入
  p3 ext2  -> ext2 驱动选择并只读访问
```

内核启动时扫描 MBR 中所有类型为 `0x83` 的 Linux 分区。这样第二分区可以继续保留 ext4；若第三分区是兼容 ext2，驱动会跳过 p2 并选择 p3。

## 使用

```text
$ ext2ls /
$ ext2ls /etc
$ ext2cat /hello.txt
```

这是旁路接口，不是完整 VFS 挂载。因此不能用普通 `ls /linux` 或 `cat /linux/file`，也不能从 ext2 执行程序。

## 支持范围

- 只读
- 1 KiB、2 KiB、4 KiB block
- MBR 分区表和 `0x83` 分区
- 经典 ext2 inode
- 直接块和一级间接块
- 普通文件与线性目录
- 稀疏文件中的空洞读取

暂不支持写入、二级/三级间接块、符号链接、ext3 journal、ext4 extent、64 位块号、metadata checksum、HTree 大目录和 GPT。

## 创建兼容分区

确认 `/dev/disk4s3` 已经是专门给 xv6 测试使用的新分区后，先卸载它，再创建传统 ext2：

```sh
diskutil unmount /dev/disk4s3
sudo /opt/homebrew/opt/e2fsprogs/sbin/mke2fs \
  -t ext2 -b 4096 -O filetype,sparse_super,large_file,^extent,^64bit,^metadata_csum \
  -L xv6-linux /dev/rdisk4s3
```

格式化会销毁该分区原有数据，必须再次确认目标确实是 `disk4s3`，不能对 `disk4` 或 `disk4s2` 执行。

启动成功时应看到：

```text
ext2: p2 unsupported incompat features=...
ext2: mounted p3 read-only start=... sectors=... block=4096 groups=...
```

实现文件：`kernel/ext2.c`，用户接口为 `ext2read`、`ext2readdir`，命令为 `ext2ls` 和 `ext2cat`。
