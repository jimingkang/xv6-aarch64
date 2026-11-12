# mygres：PostgreSQL 风格共享内存、WAL、索引与远程协议

`mygres` 是 xv6 上的 PostgreSQL 架构教学实现。它不是完整 PostgreSQL，也不承诺 SQL/catalog 兼容；目标是用当前 VFS、`mmap`、fork、semaphore 和 TCP 栈展示 PostgreSQL 的关键分层：postmaster、backend、匿名主共享区、事务、WAL、共享索引和 frontend/backend wire protocol。

源代码全部位于 `user/mygres/`：

| 文件 | 作用 |
|---|---|
| `main.c` | 存储引擎、WAL、共享索引、postmaster、IPC 客户端和 pgwire |
| `mygresd.c` | `/bin/mygresd` 入口，exec `/bin/mygres server` |

## 与 PostgreSQL 官方源码的对应关系

本实现按 PostgreSQL `src/` 的职责边界取舍，而不是逐函数移植：

| mygres 代码 | PostgreSQL 对应目录/入口 | 当前承担的职责 |
|---|---|---|
| `run_postmaster()` | [`src/backend/postmaster/postmaster.c`](https://github.com/postgres/postgres/blob/master/src/backend/postmaster/postmaster.c) | 建共享区、派生 backend、管理 listener |
| `create_postmaster_database()` | [`src/backend/storage/ipc/ipci.c`](https://github.com/postgres/postgres/blob/master/src/backend/storage/ipc/ipci.c) 的 `CreateSharedMemoryAndSemaphores()` | 计算并初始化主共享区 |
| `pgwire_listener_loop()`、`pgwire_connection()` | [`src/backend/libpq/pqcomm.c`](https://github.com/postgres/postgres/blob/master/src/backend/libpq/pqcomm.c) | listen/accept、消息 framing、frontend/backend I/O |
| `pg_execute_query()` | [`src/backend/tcop/postgres.c`](https://github.com/postgres/postgres/blob/master/src/backend/tcop/postgres.c) | traffic-cop 主循环和事务状态 |
| `write_wal()`、`recover_locked()` | [`src/backend/access/transam/xlog.c`](https://github.com/postgres/postgres/blob/master/src/backend/access/transam/xlog.c) | WAL insert/flush 与 startup REDO 的最小版本 |
| `shm_state.rows/index` | [`src/backend/storage/buffer`](https://github.com/postgres/postgres/tree/master/src/backend/storage/buffer)、[`src/backend/access/hash`](https://github.com/postgres/postgres/tree/master/src/backend/access/hash) | 共享 buffer 与可重建索引 |

PostgreSQL 官方注释特别强调：postmaster 创建共享内存和 semaphore，但通常不参与 lock manager 或共享数据操作；收到连接后立即 fork，由子进程认证并成为 backend。mygres 也让 postmaster 只负责创建/监督，数据库操作由本地 worker 或远程连接 backend 执行。后台持久化职责可继续参考 [`checkpointer.c`](https://github.com/postgres/postgres/blob/master/src/backend/postmaster/checkpointer.c)。

## 进程与共享内存模型

```text
/bin/mygresd 4
        │ exec /bin/mygres server 4
        ▼
┌──────────────────── postmaster ────────────────────┐
│ mmap(MAP_SHARED | MAP_ANONYMOUS)                  │
│   └─ shm_state: buffer rows + hash index + lock   │
│                                                   │
│ fork backend 0 ── wait(request_sem[0])            │
│ fork backend 1 ── wait(request_sem[1])            │
│ fork backend 2 ── wait(request_sem[2])            │
│ fork backend 3 ── wait(request_sem[3])            │
│ fork pgwire listener ── TCP :5432                 │
│                           └─ accept + fork backend │
└───────────────────────────────────────────────────┘
```

主共享区没有文件名，也不会被独立启动的客户端映射。它由 postmaster 在 fork 前创建，backend 继承相同的共享物理页。这与 PostgreSQL 在常见 Unix 平台上的主共享区模型一致。

当前 xv6 尚无 Unix-domain socket，因此 `/var/run/mygres.ipc` 是一个一页大小的命名 rendezvous 区：它仅保存请求槽、响应槽和内核 semaphore handle，不保存数据库 buffer 或索引。客户端原子领取空闲槽，提交请求后睡在 response semaphore；backend 睡在 request semaphore，不进行轮询。

旧版 `/var/lib/mygres/main.shm` 已取消，`mygres init` 会清理该迁移遗留文件。

## 持久文件

默认数据库目录是 `/var/lib/mygres`：

| 文件 | 作用 |
|---|---|
| `main.db` | 4 KiB 数据库头以及 1024 个固定长度 heap row |
| `main.wal` | 一页 4 KiB 的已提交事务 REDO 记录 |

磁盘 heap 是权威状态；匿名共享区是可重建 cache。postmaster 启动时先回放 WAL，再扫描 heap 重建 buffer 和 2048-bucket 哈希索引。

## 启动和本地客户端

```sh
/bin/mygres init
/bin/mygresd 4 &
/bin/mygres status
/bin/mygres put 10 1000
/bin/mygres get 10
/bin/mygres scan
/bin/mygres check
/bin/mygres stats
```

交互事务仍在客户端私有内存中暂存操作；COMMIT 时把整个修改集交给一个 backend：

```text
$ /bin/mygres shell
mygres=# BEGIN
mygres=*# PUT 1 100
mygres=*# PUT 2 200
mygres=*# GET 1
mygres=*# COMMIT
mygres=# QUIT
```

## WAL 提交和恢复

```text
backend 获取匿名共享区中的提交锁
        │
        ├─ 生成最终 row image 与新 txid
        ├─ write main.wal
        ├─ fsync(main.wal)                 ← durable commit point
        ├─ pwrite heap rows + database header
        ├─ fsync(main.db)
        ├─ 更新共享 buffer/index
        └─ truncate + fsync(main.wal)
```

完整 WAL 保存最终 row image，因此 REDO 是幂等的。`prepare` 只把 WAL 持久化，故意不写数据库页，用于模拟提交点后的断电：

```sh
/bin/mygres prepare 7 700
# 重启 mygresd；postmaster 在开放客户端前执行 REDO
/bin/mygres recover
/bin/mygres get 7
```

## PostgreSQL 客户端协议

pgwire listener 监听 TCP 5432，支持 PostgreSQL protocol 3.0 的 StartupMessage、SSL/GSS 明文回退、trust authentication、ParameterStatus、BackendKeyData、ReadyForQuery、Simple Query，以及 Parse/Bind/Describe/Execute/Close/Flush/Sync 扩展查询流程。结果消息包括 RowDescription、DataRow、CommandComplete、ErrorResponse 和 Terminate。每个远程连接 fork 独立 backend，继承 postmaster 的匿名主共享区。

```sh
psql "host=<rpi-ip> port=5432 user=root dbname=mygres sslmode=disable"
```

当前 SQL 子集：

```sql
CREATE TABLE kv (key integer primary key, value bigint);
INSERT INTO kv (key, value) VALUES (1, 100);
SELECT key, value FROM kv;
SELECT key, value FROM kv WHERE key = 1;
UPDATE kv SET value = 200 WHERE key = 1;
DELETE FROM kv WHERE key = 1;
BEGIN;
COMMIT;
ROLLBACK;
SELECT version();
```

扩展查询当前支持一条活动 prepared statement/portal、最多 16 个参数、文本结果，以及 `int2/int4/int8` 的文本或二进制输入参数。这足以让常见客户端完成基本参数化 CRUD，但不是完整 PostgreSQL：prepared-statement cache、多 portal、二进制结果、TLS、SCRAM、系统 catalog 和完整 SQL parser 尚未实现；遇到这些能力时客户端应回退到文本/Simple Query。

## 并发压力测试

```sh
/bin/mygres stress 8 100
```

测试进程并发领取 IPC 槽，固定 backend 在匿名共享区上串行提交 WAL，最后检查磁盘 header、heap row 数和共享索引是否一致。

## 当前边界和后续后台进程

- 数据模型只有 `uint32 key -> uint64 value`。
- WAL 同时只容纳一个事务，提交由共享锁串行化。
- 事务隔离是简化的 read-committed，没有 MVCC、snapshot、VACUUM 和死锁检测。
- 索引只是共享内存哈希表，重启时重建，不是磁盘 B-tree。
- 当前每次提交都同步 WAL 和数据文件，优先验证持久化顺序。
- IPC 槽不具备 PostgreSQL 完整的 backend crash 清理；客户端持槽时被强杀可能需要重启 postmaster。

下一步最有价值的是拆出 `WAL writer` 和 `checkpointer`：backend 只保证 WAL 到达提交点，后台进程批量刷新 heap；之后再增加 autovacuum、磁盘 B-tree 和 MVCC。
