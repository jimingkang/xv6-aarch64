struct buf;
struct context;
struct file;
struct epoll;
struct epoll_event;
struct inode;
struct pipe;
struct pty;
struct proc;
struct spinlock;
struct sleeplock;
struct stat;
struct superblock;
struct trapframe;
struct ext2_user_dirent;
struct vnode;
struct net_device;
struct device;
struct usb_device;
struct work_struct;

// bio.c
void            binit(void);
struct buf*     bread(uint, uint);
void            brelse(struct buf*);
void            bwrite(struct buf*);
void            bpin(struct buf*);
void            bunpin(struct buf*);

// console.c
void            consoleinit(void);
void            consoleintr(int);
void            consputc(int);
int             consoleread(int, uint64, int);
int             consolewrite(int, uint64, int);

// tty.c
void            ttyinit(void);
int             ttyattach(int);

// exec.c
int             exec(char*, char**);

// file.c
struct file*    filealloc(void);
void            fileclose(struct file*);
struct file*    filedup(struct file*);
void            fileinit(void);
void            epollinit(void);
void            epollnotify_socket(int);
void            epollnotify_pty(struct pty*, int);
struct epoll*   epollalloc(void);
void            epollclose(struct epoll*);
int             epollctl(struct epoll*, int, int, struct file*, struct epoll_event*);
int             epollwait(struct epoll*, uint64, int, int);
int             fileread(struct file*, uint64, int n);
int             filestat(struct file*, uint64 addr);
int             filewrite(struct file*, uint64, int n);

// fs.c
void            fsinit(int);
int             dirlink(struct inode*, char*, uint);
struct inode*   dirlookup(struct inode*, char*, uint*);
struct inode*   ialloc(uint, short);
struct inode*   idup(struct inode*);
void            iinit();
void            ilock(struct inode*);
void            iput(struct inode*);
void            iunlock(struct inode*);
void            iunlockput(struct inode*);
void            iupdate(struct inode*);
int             namecmp(const char*, const char*);
struct inode*   namei(char*);
struct inode*   nameiparent(char*, char*);
int             readi(struct inode*, int, uint64, uint, uint);
void            stati(struct inode*, struct stat*);
int             writei(struct inode*, int, uint64, uint, uint);
void            itrunc(struct inode*);

// ramdisk.c
void            ramdiskinit(void);
void            ramdiskrw(struct buf*, int);

// sd.c
void            sdinit(void);
void            sd_driver_init(void);
int             sdsector(uint32, void*, int);

// fat32.c
void            fat32init(void);
void            fat32rw(struct buf*, int);
struct fat32_file;
int             fat32openroot(char*, struct fat32_file*);
int             fat32pread(struct fat32_file*, uint32, void*, int);
int             fat32ready(void);
struct fat32_dirent;
int             fat32statpath(char*, struct fat32_dirent*);
int             fat32readdirroot(int, struct fat32_dirent*);

// ext2.c
void            ext2init(void);
int             ext2readfile(char*, uint64, void*, int);
int             ext2readdir(char*, int, struct ext2_user_dirent*);
int             ext2stat(char*, uint*, ushort*, uint*);
int             ext2ready(void);

// vfs.c
void            vfsinit(void);
int             vfsopen(char*, int, struct vnode**);
void            vfsclose(struct vnode*);
int             vfsread(struct vnode*, int, uint64, uint, uint);
int             vfsstat(struct vnode*, struct stat*);
int             vfsmount(char*, char*);

// kalloc.c
void*           kalloc(void);
void            kfree(void *);
uint64          kfreepages(void);
void            kinit1(void *, void *);
void            kinit2(void *, void *);

// log.c
void            initlog(int, struct superblock*);
void            log_write(struct buf*);
void            begin_op(void);
void            end_op(void);

// pipe.c
int             pipealloc(struct file**, struct file**);
void            pipeclose(struct pipe*, int);
int             piperead(struct pipe*, uint64, int);
int             pipewrite(struct pipe*, uint64, int);

// pty.c
int             ptyalloc(struct file**, struct file**);
void            ptyclose(struct pty*, int);
int             ptyread(struct pty*, int, uint64, int);
int             ptywrite(struct pty*, int, uint64, int);
int             ptypoll(struct pty*, int, int);

// printf.c
void            printf(char*, ...);
void            panic(char*) __attribute__((noreturn));
void            printfinit(void);

// proc.c
void            exit(int);
int             fork(void);
int             growproc(int);
void            proc_mapstacks(pagetable_t);
int             kill(int);
int             kthread_create(void (*)(void*), void*, char*);
struct cpu*     mycpu(void);
struct cpu*     getmycpu(void);
struct proc*    myproc();
void            procinit(void);
void            scheduler(void) __attribute__((noreturn));
void            sched(void);
void            sleep(void*, struct spinlock*);
void            userinit(void);
int             wait(uint64);
void            wakeup(void*);
void            yield(void);
int             either_copyout(int user_dst, uint64 dst, void *src, uint64 len);
int             either_copyin(void *dst, int user_src, uint64 src, uint64 len);
void            procdump(void);
int             proclist(char*, int);
int             procvmdump(int);

// workqueue.c
void            workqueue_init(void);
void            init_work(struct work_struct*, void (*)(struct work_struct*));
int             schedule_work(struct work_struct*);
void            flush_work(struct work_struct*);
void            cancel_work_sync(struct work_struct*);

// sync.c
void            syncinit(void);
int             ksync_create(int, int);
int             ksync_wait(int);
int             ksync_signal(int, int);
int             ksync_reset(int);
int             ksync_atomic(int, int, int, int);
int             ksync_destroy(int);

// net.c
void            netinit(void);
int             net_udp_bind(int);
int             net_udp_unbind(int);
int             net_udp_send(uint32, int, int, uint64, int);
int             net_udp_recv(int, uint64, uint64, uint64, int);
int             net_icmp_send(uint32, int, int, uint64, int);
int             net_icmp_recv(int, uint64, uint64, uint64, int);
int             net_tcp_listen(int, int);
int             net_tcp_accept(int, int);
int             net_tcp_read(int, uint64, int, int);
int             net_tcp_write(int, uint64, int, int);
int             net_tcp_close(int);
int             net_tcp_poll(int, int);
void            net_tcp_tick(void);
void            net_rx(void*, int);
void            net_rx_dev(struct net_device*, void*, int);
int             net_dhcp(void);
int             net_dhcp_dev(struct net_device*);
void            netdev_poll_all(void);

// sdio.c / brcmfmac.c
void            sdio_bus_init(void);
void            brcmfmac_driver_init(void);
void            brcmfmac_driver_exit(void);
void            brcmfmac_sdio_irq(void);
int             brcmfmac_connect(char*, char*);
void            wpa_pbkdf2(char*, char*, uint8*);
void            wpa_make_snonce(uint8*, uint8*, uint64, uint8*);
void            wpa_derive_ptk(uint8*, uint8*, uint8*, uint8*, uint8*, uint8*);
void            wpa_eapol_mic(uint8*, void*, int, uint8*);
int             wpa_aes_unwrap(uint8*, uint8*, int, uint8*);
void            arasan_sdio_driver_init(void);
void            arasan_sdio_driver_exit(void);
void            usb_bus_init(void);
void            mt7601u_driver_init(void);
void            mt7601u_driver_exit(void);
void            mt7601u_poll(void);
void            mt7601u_rx_irq(void);
void            mt7601u_pause(int);

// dwc2.c / usbnet.c
void            dwc2_driver_init(void);
int             dwc2_cdc_xmit(void*, int);
void            dwc2_cdc_poll(void);
int             usbnet_attach(struct usb_device*);
void            usbnet_detach(void);
void            usbnet_rx(void*, int);

// swtch.S
void            swtch(struct context*, struct context*);

// spinlock.c
void            acquire(struct spinlock*);
int             holding(struct spinlock*);
void            initlock(struct spinlock*, char*);
void            release(struct spinlock*);
void            push_off(void);
void            pop_off(void);

// sleeplock.c
void            acquiresleep(struct sleeplock*);
void            releasesleep(struct sleeplock*);
int             holdingsleep(struct sleeplock*);
void            initsleeplock(struct sleeplock*, char*);

// string.c
int             memcmp(const void*, const void*, uint);
void*           memmove(void*, const void*, uint);
void*           memset(void*, int, uint);
char*           safestrcpy(char*, const char*, int);
int             strlen(const char*);
int             strncmp(const char*, const char*, uint);
char*           strncpy(char*, const char*, int);

// syscall.c
int             argint(int, int*);
int             argstr(int, char*, int);
int             argaddr(int, uint64 *);
int             fetchstr(uint64, char*, int);
int             fetchaddr(uint64, uint64*);
void            syscall();

// trap.c
extern uint     ticks;
void            trapinit(void);
void            trapinithart(void);
extern struct spinlock tickslock;
void            usertrapret(struct trapframe *);

// uart.c
void            uartinit(void);
void            uartintr(void);
void            uartputc(int);
void            uartputc_sync(int);
int             uartgetc(void);

// vm.c
void            kvminit(void);
void            kvminithart(void);
void            kvmdump(void);
void            uvmdump(pagetable_t, int, char *, char *);
void            kvmmap(pagetable_t, uint64, uint64, uint64, uint64);
int             mappages(pagetable_t, uint64, uint64, uint64, uint64);
pagetable_t     uvmcreate(void);
void            uvminit(pagetable_t, uchar *, uint);
uint64          uvmalloc(pagetable_t, uint64, uint64);
uint64          uvmdealloc(pagetable_t, uint64, uint64);
int             uvmcopy(pagetable_t, pagetable_t, uint64);
void            uvmfree(pagetable_t, uint64);
void            uvmunmap(pagetable_t, uint64, uint64, int);
void            uvmclear(pagetable_t, uint64);
void            uvmsync_icache(pagetable_t, uint64);
uint64          uva2ka(pagetable_t, uint64);
int             copyout(pagetable_t, uint64, char *, uint64);
int             copyin(pagetable_t, char *, uint64, uint64);
int             copyinstr(pagetable_t, char *, uint64, uint64);
void            switchuvm(struct proc *);
void            switchkvm(void);

// gicv3.c
void            gicv3init(void);
void            gicv3inithart(void);
uint32          gic_iar(void);
int             gic_iar_irq(uint32);
void            gic_eoi(uint32);

// timer.c
void            timerinit(void);
int             timerintr(void);

// rng.c
void            rnginit(void);
int             rngbytes(void *, int);

// virtio_disk.c
void            virtio_disk_init(void);
void            virtio_disk_rw(struct buf *, int);
void            virtio_disk_intr(void);

// arasan_sdio.c
void            arasan_sdio_irq_enable(void);
void            arasan_sdio_irq(void);
int             arasan_sdio_irq_pending(void);
void            arasan_sdio_irq_complete(void);

// dwc2.c
void            dwc2_irq(void);

// number of elements in fixed-size array
#define NELEM(x) (sizeof(x)/sizeof((x)[0]))
