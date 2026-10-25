import sys, shutil, os, re
B = 'xv6-book/chapters/'
os.makedirs('build/tex-bak3', exist_ok=True)
for f in ('02-organization.tex', '06-filesystem.tex', 'fig-fstrace-dev8.tex'):
    shutil.copy(B + f, 'build/tex-bak3/' + f)

def rep(s, a, b, cnt=1):
    n = s.count(a)
    if n != cnt:
        sys.exit('count %d != %d for: %s' % (n, cnt, a[:90]))
    return s.replace(a, b)

# ---------------- 06：fs-trace ----------------
p = B + '06-filesystem.tex'; s = open(p).read()
s = rep(s, '日志共 4408 行。这份日志取自 \\code{/dev/root} 还经过“设备 1”包装的内核；现在同一个 p2 直接以块设备 4（\\code{mmcblk0p2}）挂载，下文的 dev 1 对应今天的 dev 4。',
        '日志共 4408 行。')
s = rep(s, '和根文件系统（dev 1）。它们是三个不同的文件系统', '和根文件系统（dev 4，即 \\code{mmcblk0p2}）。它们是三个不同的文件系统')
s = rep(s, '根文件系统（dev 1） & 含义', '根文件系统（dev 4） & 含义')
s = rep(s, '根文件系统（dev 1） & 0 & 1 &', '根文件系统（dev 4） & 0 & 1 &')
s = rep(s, 'xv6\\_read\\_inode: dev=1 ino=25', 'xv6\\_read\\_inode: dev=4 ino=25')
s = rep(s, '第 3 步建出的 /dev/root，次设备号 1', '第 3 步建出的 /dev/root，次设备号 4')
s = rep(s, '3 \\code{T\\_DEVICE} & 7 \\code{BLOCKDEV} & 1 & 1 & 0 \\\\', '3 \\code{T\\_DEVICE} & 7 \\code{BLOCKDEV} & 4 & 1 & 0 \\\\')
s = rep(s, '为 12 个设备建节点', '为 11 个设备建节点')
s = rep(s, '\\code{video0}、\\code{sdroot}、\\code{mmcblk0}', '\\code{video0}、\\code{mmcblk0}')
s = rep(s, '共 14 项 448 字节', '共 13 项 416 字节')
s = rep(s, '重新从块 2 读 dinode，前后 12 次', '重新从块 2 读 dinode，前后 11 次')
s = rep(s, '14 个节点都在 inode 块 2', '13 个 inode 都在 inode 块 2')
s = rep(s, '\\paragraph{根文件系统（dev 1）.}', '\\paragraph{根文件系统（dev 4）.}')
s = rep(s, '\\caption{根文件系统（dev 1）中日志读到过的', '\\caption{根文件系统（dev 4）中日志读到过的')
open(p, 'w').write(s)

# rootfs 的 dinode 转储里 /dev/root 的 minor（若有）
p7 = B + 'fig-fstrace-dev7.tex'

# ---------------- 06：devtmpfs 图 ----------------
shutil.copy('build/fig-fstrace-dev8.tex', B + 'fig-fstrace-dev8.tex')

# ---------------- 02：main() ----------------
p = B + '02-organization.tex'; s = open(p).read()
i = s.index('\\begin{ccode}\nvoid main() {')
j = s.index('几条顺序约束值得先记住')
s = s[:i] + r'''\begin{ccode}
void main() {
  if(cpuid() == 0){
    kinit1(end, P2V(EARLYTOP));  // 只用启动页表覆盖的前 2 MiB    -> 第 4 章
    kvminit();  kvminithart();   // 建立并切换到最终内核页表      -> 第 4 章
    kinit2(P2V(EARLYTOP), P2V(PHYSTOP));
    kobject_subsys_init(); device_init();                       // -> 第 7 章
    sdio_bus_init(); usb_bus_init();
    consoleinit(); ttyinit(); printfinit();                     // 从这里起 printf 可用
    procinit(); workqueue_init();                               // -> 第 5 章
    rnginit(); syncinit(); netinit();
    trapinit(); trapinithart(); gicv3init(); gicv3inithart();   // -> 第 3 章
    timerinit();
    binit(); iinit(); fileinit();                               // -> 第 6 章
    do_initcalls();                                             // 各驱动与文件系统，见下
    userinit();                                                 // 第一个用户进程
    start_secondary_cpus();                                     // 唤醒 CPU1~3
    started = 1;
  } else {
    while(started == 0) ;
    kvminithart(); trapinithart(); gicv3inithart(); timerinit();
  }
  scheduler();
}
\end{ccode}

驱动和文件系统不再手写在 \code{main()} 里。每个模块用 \code{subsys\_initcall()}、\code{fs\_initcall()}、\code{module\_init()}、\code{late\_initcall()} 这类宏把自己放进链接器生成的 \code{.initcall.list} 表，\code{do\_initcalls()} 按级别从低到高、同级按序号依次调用（序号相同的按链接顺序）：

\begin{txtcode}
subsys   input_init()                 输入子系统核心
         i2c_core_init()              I2C 核心
         blkdev_init()                块设备表，主设备号 BLOCKDEV
fs       fat32init()                  FAT32 驱动的全局状态
device   sd_driver_init()             SD 卡：add_disk(mmcblk0)，块层扫描 MBR，
                                      登记 mmcblk0p1～p4
         arasan_sdio_driver_init()  1 SDIO 控制器（Wi-Fi 芯片挂在上面）
         usbkbd_driver_init()       2 USB 键盘
         dwc2_driver_init()         4 USB 主控制器
         ov5647_init()              5 摄像头传感器
late     rootdev_init()               挂起 FAT32 启动分区、读 cmdline_xv6.txt，
                                      选出 ROOT_DEV                -> 第 6 章
         brcmfmac_driver_init()     2 Wi-Fi，固件在启动分区里
         mt7601u_driver_init()      3 USB Wi-Fi，固件在启动分区里
         camera_init()              4 unicam 摄像头接口
         ext2init()                 5 只登记 ext2 文件系统类型
         vfsinit()                  6 登记 procfs、sysfs、fat32、netfs
\end{txtcode}

''' + s[j:]
s = rep(s, '\\item Wi-Fi 驱动需要从 FAT32 启动分区读取固件，所以必须排在 \\code{rootdev\\_init()} 之后——它扫描 MBR 时顺带挂起 FAT32 启动分区。',
        '\\item Wi-Fi 驱动需要从 FAT32 启动分区读取固件，所以放在 \\code{late} 级、排在 \\code{rootdev\\_init()} 之后——由它挂起 FAT32 启动分区；分区表则早在 \\code{device} 级 SD 驱动 \\code{add\\_disk()} 时由块层扫描好。')
open(p, 'w').write(s)
print('ok')
