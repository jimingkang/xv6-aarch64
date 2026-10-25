import re, sys, shutil, os
B = 'xv6-book/chapters/'
os.makedirs('build/tex-bak', exist_ok=True)
for f in ('06-filesystem.tex', '09-optional.tex'):
    shutil.copy(B + f, 'build/tex-bak/' + f)

def rep(s, a, b, cnt=1):
    n = s.count(a)
    if n != cnt:
        sys.exit('count %d != %d for: %s' % (n, cnt, a[:80]))
    return s.replace(a, b)

# ---------------- 06-filesystem.tex ----------------
p = B + '06-filesystem.tex'; s = open(p).read()
chain = open('build/ext2-chain.tex').read()
mount = open('build/ext2-mount.tex').read()

# A. xv6fs chain: ext2init no longer probes
s = re.sub(r'  ext2init\(\)( +)ext2 驱动探测 p3，留给 /mnt/ext2 使用',
           lambda m: '  ext2init()' + m.group(1) + '只登记 ext2 类型，挂载时才读盘', s, count=1)

# B. ext2 call chain + (a')-(d') paragraph
i = s.index('\\paragraph{ext2 根文件系统.}')
j = s.index('注意 xv6 的“设备 1”')
s = s[:i] + chain + s[j:]

# C. stage-one summary
s = rep(s, '随后 \\code{ext2init()} 用 \\code{rootdev\\_ext2\\_part()} 和 \\code{rootdev\\_raw\\_info()} 找 ext2 分区及其外置日志，',
        '随后 \\code{ext2init()} 只把 ext2 登记进文件系统类型链表（\\code{register\\_filesystem()}），不读盘，')

# D. sec:ext2-root enumerate + new material after the note
old_enum_start = s.index('\\item 阶段一，\\code{rootdev\\_init()} 记下 ext2 根所在的分区')
old_enum_end = s.index('\\end{enumerate}', old_enum_start)
s = s[:old_enum_start] + (
    '\\item 阶段一，\\code{rootdev\\_init()} 按 \\code{root=} 找到 \\code{mmcblk0p3}，把 ROOT\\_DEV 记为 5、\\code{rootdev\\_fstype()} 记为 ext2；它仍然寻找 xv6 分区（\\code{rootdev\\_xv6\\_part()}），因为 ext2 的外置日志在那里，找不到也不 panic。\\code{ext2init()} 只登记文件系统类型，不读盘；\n'
    '  \\item 阶段二，\\code{/dev/root} 的次设备号是 5，\\code{mount\\_root()} 按 \\code{rootdev\\_fstype()} 调用 \\code{vfs\\_get\\_sb("ext2", "/dev/root")}，由 ext2 自己的 \\code{get\\_sb} 和 \\code{fill\\_super} 打开分区、恢复外置日志、登记 \\code{supers[5]}，而 xv6fs 走的是 \\code{xv6fs\\_fill\\_super()}。\n'
) + s[old_enum_end:]
k = s.index('\\end{note}', s.index('\\label{sec:ext2-root}')) + len('\\end{note}\n')
s = s[:k] + '\n' + mount + s[k:]

# E. vfsmount description in "由用户态决定挂载"
s = rep(s, 'ext2 找到起点与 \\code{e2.part\\_lba} 相同的分区块设备，用它的设备号',
        'ext2 的 \\code{ext2\\_get\\_sb()} 先试 \\code{lookup\\_bdev(source)}，source 不是设备节点时用已打开的 ext2 实例，或扫描 0x83 分区逐个 \\code{fill\\_super}，见第~\\ref{sec:ext2-mount} 节')
open(p, 'w').write(s)

# ---------------- 09-optional.tex ----------------
p = B + '09-optional.tex'; s = open(p).read()
i = s.index('日志的位置在启动时算出。')
j = s.index('\\subsection{ext2 的磁盘结构}')
s = s[:i] + r'''日志的位置在挂载时算出。\code{ext2init()} 只登记文件系统类型；\code{ext2\_fill\_super()}（第~\ref{sec:ext2-mount} 节）打开分区、读完超级块以后调用 \code{ext2\_journal\_init()}，向根块设备层（第~\ref{sec:rootfs} 节）要 xv6 分区 p2 的块设备，再跳过 xv6 镜像：

\begin{ccode}
struct blkdev *jbd = rootdev_xv6_part();                   // p2
struct blkdev *home = blkdev_get(fs->dev);                 // p3
uint32 jstart = FSSIZE * (BSIZE / SECTOR);                 // 65536
if(jbd && jbd->disk && jbd->nsect > jstart && home){
  if(xj_init(jbd, jstart, jbd->nsect - jstart, home) < 0)  // 先恢复日志
    return -1;                                             // 恢复失败：拒绝挂载
  fs->journal = 1;
} else
  printf("ext2: no raw xv6 root partition; external journal disabled\n");
\end{ccode}

\code{xj\_init()} 在 ext2 被登记进 \code{supers[]}、对外可见之前检查并重放日志，所以之后任何人读到的位图和目录都已经是一致的。如果提交记录有效、但描述符、载荷、校验和或写回原位任何一步失败，\code{xj\_init()} 返回 $-1$，整个挂载失败，绝不把没恢复好的 ext2 暴露出去。QEMU 裸镜像没有 p2，这时日志关闭，ext2 退回经块缓存的同步写。更新根文件系统的 \code{/dev/sdroot}（第~\ref{sec:xv6fs} 节）只写前 \code{FSSIZE} 块，不会碰到日志区。

''' + s[j:]

s = rep(s, '它们是全局变量而不是局部数组——内核栈只有 4\\,KiB，放不下其中任何一个（第~\\ref{chap:drivers} 章的摄像头调试记录里有过同样的教训）。所有 ext2 操作都持着同一把锁，所以全局缓冲是安全的。',
        '它们属于每个 ext2 实例（\\code{struct ext2\\_fs}），由 \\code{ext2\\_fill\\_super()} 用 \\code{kalloc()} 分配，而不是局部数组——内核栈只有 4\\,KiB，放不下其中任何一个（第~\\ref{chap:drivers} 章的摄像头调试记录里有过同样的教训）。同一实例的操作都持着该实例的锁，所以这些缓冲是安全的。')

s = rep(s, '\\code{e2.open[]} 表仍然存在', '每个实例的 \\code{open[]} 表仍然存在')
s = rep(s, '在 e2.open[] 里占一个引用', '在 fs->open[] 里占一个引用')

s = rep(s, '所有 ext2 操作由睡眠锁 \\code{e2.lock} 串行化。',
        '同一个 ext2 实例的操作由它自己的睡眠锁 \\code{fs->lock}（\\code{struct ext2\\_fs} 里，每个设备一把）串行化，不同分区互不阻塞。')
s = rep(s, '先 inode 锁、后 \\code{e2.lock}', '先 inode 锁、后 \\code{fs->lock}')
s = rep(s, '要先放掉 \\code{e2.lock}', '要先放掉 \\code{fs->lock}')

s = rep(s, '''int ext2rename(char *oldpath, char *newpath) {
  acquiresleep(&e2.lock);
  tx_begin();                                               // xj_begin()，清空 freed[]
  int r = tx_end(rename_locked(oldpath, newpath) == 0);     // 成功则提交，失败则中止
  releasesleep(&e2.lock);
  return r;
}''', '''int ext2rename(char *oldpath, char *newpath) {
  struct ext2_fs *fs = ext2_default();                      // 当前挂着的 ext2 实例
  acquiresleep(&fs->lock);
  tx_begin(fs);                                             // xj_begin()，清空 freed[]
  int r = tx_end(fs, rename_locked(fs, oldpath, newpath) == 0);  // 成功则提交，失败则中止
  releasesleep(&fs->lock);
  return r;
}''')

s = rep(s, '驱动还没有 \\code{umount}，从来不会把它置回去，所以开发机上的 \\code{e2fsck} 总会做一次完整检查。',
        '非根挂载可以 \\code{umount}：\\code{fs\\_umount()} 先标记 \\code{unmounting} 挡住新的路径遍历，确认没有打开的文件、当前目录、活动 inode 和子挂载，再调用 \\code{super\\_block.put\\_super = ext2\\_put\\_super}：用最后一个元数据事务把 \\code{VALID\\_FS} 置回、flush、关闭外置日志、\\code{binvalidate()}，最后释放超级块和实例。强制断电、根文件系统无法卸载或卸载失败时，接回开发机仍应执行 \\code{e2fsck -f}；挂载时该位原本就是 dirty 的，正常卸载也会保留 dirty。')

s = rep(s, '\\code{rootdev\\_raw\\_info()} 告诉 ext2 p2 的位置，日志就放在 xv6 镜像之后',
        '\\code{rootdev\\_xv6\\_part()} 告诉 ext2 p2 的块设备，日志就放在 xv6 镜像之后')
s = rep(s, '\\code{ext2\\_iops}、\\code{ext2\\_register\\_super()}：作为统一 VFS 的一种文件系统，可挂在 \\code{/mnt/ext2} 或作根文件系统',
        '\\code{ext2\\_type}（\\code{ext2\\_get\\_sb}、\\code{ext2\\_fill\\_super}）、\\code{ext2\\_iops}、\\code{ext2\\_register\\_super()}、\\code{ext2\\_put\\_super()}：作为统一 VFS 的一种文件系统，可挂在 \\code{/mnt/ext2} 或作根文件系统')
open(p, 'w').write(s)
print('ok')
