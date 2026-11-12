# mygres：pwrite → xjournal commit → fsync → sdflush 落盘路径核查

> 分支：`mygres`（HEAD `fa1a17d`）。工作区未提交改动不涉及内核。行号以该版本为准。

## 结论

在当前代码中，commit record 是在 **pwrite 系统调用内部**同步构造、写盘并 flush 的：pwrite 返回前，事务已持久化，checkpoint 也已完成。`fsync()` 会等待，但它等待的只是一次额外的 `sdflush()`（CMD13 屏障），它不触发、也不需要触发 commit。

## 1. pwrite 调用链

- `sys_pwrite` → `filepwrite` → `file_write_at`（`kernel/fs/sysfile.c:341`，`kernel/fs/file.c:328`、`:196`）
- `e2_file_write_at` → `e2_write` → `ext2writeino`（`kernel/fs/ext2/ext2.c:2168`、`:2136`、`:1693`）：先获取 `fs->lock`，再执行 `tx_begin` → `xj_begin`（`:1700`、`:1251`）
- `write_locked`（`:1633`）：
  - **数据块**：经 `writeblock_data`（`:1650`、`:276`），`fs->direct=1`，再经 `e2_cache_write` → `bwrite` 原地直写（ordered 模式，`:166`、`:181`）
  - **元数据**（inode、位图、组描述符）：经 `diskbytes_write` 由 `xj_write` 按 512B 扇区捕获到内存，不写 home（`:248`、`:252`）
  - 最后调用 `writeinode`（`:1658`），因此 pwrite 事务至少包含 inode 所在扇区
- `write_common` → `tx_end` → `xj_commit`（`:1665`、`:1268`）

## 2. xj_commit 内部（`kernel/fs/ext2/xjournal.c:371`）

| 步骤 | 内容 | 位置 |
|---|---|---|
| 0 | 内存中构造 descriptor（magic、seq、count、home LBA），计算 FNV 校验和 | `:381-388` |
| 1 | 写 descriptor 和 payload，然后 `sdflush()`；这次 flush 也保证之前直写的数据块先于 commit 落盘 | `:391-398` |
| 2 | **构造 commit record**：`make_commit()`（CMAGIC、seq、count、sum 及自身校验），调用点在 `:400`；**写盘并 flush**：`log_rw(XJ_DESC+XJ_CAP)` 后接 `sdflush()`，**这里是提交点** | `:106-115`、`:400`、`:401` |
| 3 | checkpoint：`write_home` 经 `bread/bwrite` 把 payload 写回 ext2 home 块，然后 `sdflush()` | `:404`、`:180`、`:186` |
| 4 | `clear_commit` 清零 commit 扇区，然后 `sdflush()` | `:133-135` |

- 一次 pwrite 共执行 3 次 flush；本次写会话的首次写还会在 `mark_write_session_dirty` 中多执行 1 次（`ext2.c:438`）。
- 日志经 `part_rw` → `mmc_rw` 绕过 bcache，直接写到 xv6 分区尾部（`xjournal.c:63`，`ext2.c:1088`）。

## 3. fsync 调用链

`sys_fsync` → `filefsync` → `e2_file_fsync` → `e2_fsync` → `ext2fsync`（`sysfile.c:302`，`file.c:357`，`ext2.c:2175`、`:2156`、`:1917`）

`ext2fsync` 在持有 `fs->lock` 时只调用一次 `sdflush()`（`:1925`），注释说明了原因：事务已经同步提交。`dataonly` 参数被忽略，所以 fdatasync 与 fsync 的行为相同。

## 4. sdflush 是否真的等待

会等待。实际编译的是 `sdhost.c`（`Makefile:94`），`sd.c` 不参与编译。

`sdflush`（`kernel/drivers/mmc/host/sdhost.c:438`）持有 `sdlock`，轮询 CMD13，直到 READY_FOR_DATA 置位且状态为 TRAN(4)；最多等 5 秒，超时返回 -1（`:446-466`）。写操作本身是 PIO 同步的（`:320-345`）。驱动没有启用 SD cache 扩展，因此卡回到 TRAN 状态是这里能确认的最强持久化边界。

## 5. 发现的问题

1. **checkpoint 失败被静默吞掉**：`write_home` 或 `clear_commit` 失败时，`xj_commit` 只打印日志并返回 0（`xjournal.c:404-406`），磁盘上会残留一条有效的 commit record。下一次 `xj_commit` 不检查这个状态，直接覆盖 descriptor 和 payload（`:391`）。如果之后发生崩溃，`recover()` 会读到旧 commit 和新 descriptor，因 seq 不匹配而拒绝挂载（`:208`）；此时旧事务的 payload 已被覆盖，无法重放。
   **建议**：checkpoint 失败后把 journal 标记为损坏，或在下一次 `xj_begin/xj_commit` 前先重试 `recover()`。
2. **pwrite 部分成功会报告为失败**：大块写入途中会经 `tx_checkpoint` 分段提交（`ext2.c:1300`、`:1646`），已提交的前缀是持久的，但 `filepwrite` 在 `done != n` 时返回 -1（`file.c:336`）。mygres 的 WAL 只有一页，目前可以接受。
3. **fsync 目前不承担持久化职责**：将来如果把事务改成异步或批量提交，fsync 必须改为“先强制 commit，再 flush”，否则 mygres `write_wal`（`user/mygres/main.c:409`）的持久化保证会失效。
4. 小问题：连续的 CMD24 之间不通过 CMD13 检查卡是否 busy，只依赖控制器 FSM 回到 DATA 状态（`sdhost.c:301-317`）。目前可以工作，但不是规范要求的检查方式。

## 6. 对 mygres 的影响

- `write_wal` 中 `pwrite + fsync` 返回 0 时，WAL 页确实已持久化到卡上。
- `O_TRUNC` 与 `pwrite` 是两个独立事务。如果两者之间发生崩溃，只会留下空的 WAL，`read_wal` 会把它当作“没有事务”处理（`main.c:425`），这一处理是正确的。
