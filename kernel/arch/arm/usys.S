.syntax unified
.arm
#include "kernel/syscall.h"
.text
.global fork
fork:
  push {r7,lr}
  mov r7, #SYS_fork
  svc #0
  pop {r7,pc}
.global exit
exit:
  push {r7,lr}
  mov r7, #SYS_exit
  svc #0
  pop {r7,pc}
.global wait
wait:
  push {r7,lr}
  mov r7, #SYS_wait
  svc #0
  pop {r7,pc}
.global pipe
pipe:
  push {r7,lr}
  mov r7, #SYS_pipe
  svc #0
  pop {r7,pc}
.global read
read:
  push {r7,lr}
  mov r7, #SYS_read
  svc #0
  pop {r7,pc}
.global write
write:
  push {r7,lr}
  mov r7, #SYS_write
  svc #0
  pop {r7,pc}
.global close
close:
  push {r7,lr}
  mov r7, #SYS_close
  svc #0
  pop {r7,pc}
.global kill
kill:
  push {r7,lr}
  mov r7, #SYS_kill
  svc #0
  pop {r7,pc}
.global exec
exec:
  push {r7,lr}
  mov r7, #SYS_exec
  svc #0
  pop {r7,pc}
.global open
open:
  push {r7,lr}
  mov r7, #SYS_open
  svc #0
  pop {r7,pc}
.global mknod
mknod:
  push {r7,lr}
  mov r7, #SYS_mknod
  svc #0
  pop {r7,pc}
.global unlink
unlink:
  push {r7,lr}
  mov r7, #SYS_unlink
  svc #0
  pop {r7,pc}
.global fstat
fstat:
  push {r7,lr}
  mov r7, #SYS_fstat
  svc #0
  pop {r7,pc}
.global link
link:
  push {r7,lr}
  mov r7, #SYS_link
  svc #0
  pop {r7,pc}
.global mkdir
mkdir:
  push {r7,lr}
  mov r7, #SYS_mkdir
  svc #0
  pop {r7,pc}
.global chdir
chdir:
  push {r7,lr}
  mov r7, #SYS_chdir
  svc #0
  pop {r7,pc}
.global dup
dup:
  push {r7,lr}
  mov r7, #SYS_dup
  svc #0
  pop {r7,pc}
.global getpid
getpid:
  push {r7,lr}
  mov r7, #SYS_getpid
  svc #0
  pop {r7,pc}
.global sbrk
sbrk:
  push {r7,lr}
  mov r7, #SYS_sbrk
  svc #0
  pop {r7,pc}
.global sleep_ticks
sleep_ticks:
  push {r7,lr}
  mov r7, #SYS_sleep
  svc #0
  pop {r7,pc}
.global uptime
uptime:
  push {r7,lr}
  mov r7, #SYS_uptime
  svc #0
  pop {r7,pc}
.global ps
ps:
  push {r7,lr}
  mov r7, #SYS_ps
  svc #0
  pop {r7,pc}
.global ext2read
ext2read:
  push {r7,lr}
  mov r7, #SYS_ext2read
  svc #0
  pop {r7,pc}
.global ext2readdir
ext2readdir:
  push {r7,lr}
  mov r7, #SYS_ext2readdir
  svc #0
  pop {r7,pc}
.global sync_create
sync_create:
  push {r7,lr}
  mov r7, #SYS_sync_create
  svc #0
  pop {r7,pc}
.global sync_wait
sync_wait:
  push {r7,lr}
  mov r7, #SYS_sync_wait
  svc #0
  pop {r7,pc}
.global sync_signal
sync_signal:
  push {r7,lr}
  mov r7, #SYS_sync_signal
  svc #0
  pop {r7,pc}
.global sync_reset
sync_reset:
  push {r7,lr}
  mov r7, #SYS_sync_reset
  svc #0
  pop {r7,pc}
.global sync_atomic
sync_atomic:
  push {r7,lr}
  mov r7, #SYS_sync_atomic
  svc #0
  pop {r7,pc}
.global sync_destroy
sync_destroy:
  push {r7,lr}
  mov r7, #SYS_sync_destroy
  svc #0
  pop {r7,pc}
.global udp_bind
udp_bind:
  push {r7,lr}
  mov r7, #SYS_udp_bind
  svc #0
  pop {r7,pc}
.global udp_unbind
udp_unbind:
  push {r7,lr}
  mov r7, #SYS_udp_unbind
  svc #0
  pop {r7,pc}
.global udp_send
udp_send:
  push {r7,lr}
  mov r7, #SYS_udp_send
  svc #0
  pop {r7,pc}
.global udp_recv
udp_recv:
  push {r7,lr}
  mov r7, #SYS_udp_recv
  svc #0
  pop {r7,pc}
.global udp_tryrecv
udp_tryrecv:
  push {r7,lr}
  mov r7, #SYS_udp_tryrecv
  svc #0
  pop {r7,pc}
.global udp_recv_timeout
udp_recv_timeout:
  push {r7,lr}
  mov r7, #SYS_udp_recv_timeout
  svc #0
  pop {r7,pc}
.global icmp_send
icmp_send:
  push {r7,lr}
  mov r7, #SYS_icmp_send
  svc #0
  pop {r7,pc}
.global icmp_recv
icmp_recv:
  push {r7,lr}
  mov r7, #SYS_icmp_recv
  svc #0
  pop {r7,pc}
.global mount
mount:
  push {r7,lr}
  mov r7, #SYS_mount
  svc #0
  pop {r7,pc}
.global tty_attach
tty_attach:
  push {r7,lr}
  mov r7, #SYS_tty_attach
  svc #0
  pop {r7,pc}
.global tty_set_foreground
tty_set_foreground:
  push {r7,lr}
  mov r7, #SYS_tty_set_foreground
  svc #0
  pop {r7,pc}
.global vmdump
vmdump:
  push {r7,lr}
  mov r7, #SYS_vmdump
  svc #0
  pop {r7,pc}
.global wifi_connect
wifi_connect:
  push {r7,lr}
  mov r7, #SYS_wifi_connect
  svc #0
  pop {r7,pc}
.global net_dhcp
net_dhcp:
  push {r7,lr}
  mov r7, #SYS_net_dhcp
  svc #0
  pop {r7,pc}
.global tcp_listen
tcp_listen:
  push {r7,lr}
  mov r7, #SYS_tcp_listen
  svc #0
  pop {r7,pc}
.global tcp_accept
tcp_accept:
  push {r7,lr}
  mov r7, #SYS_tcp_accept
  svc #0
  pop {r7,pc}
.global tcp_read
tcp_read:
  push {r7,lr}
  mov r7, #SYS_tcp_read
  svc #0
  pop {r7,pc}
.global tcp_write
tcp_write:
  push {r7,lr}
  mov r7, #SYS_tcp_write
  svc #0
  pop {r7,pc}
.global tcp_close
tcp_close:
  push {r7,lr}
  mov r7, #SYS_tcp_close
  svc #0
  pop {r7,pc}
.global socket_listen
socket_listen:
  push {r7,lr}
  mov r7, #SYS_socket_listen
  svc #0
  pop {r7,pc}
.global socket_accept
socket_accept:
  push {r7,lr}
  mov r7, #SYS_socket_accept
  svc #0
  pop {r7,pc}
.global epoll_create
epoll_create:
  push {r7,lr}
  mov r7, #SYS_epoll_create
  svc #0
  pop {r7,pc}
.global epoll_ctl
epoll_ctl:
  push {r7,lr}
  mov r7, #SYS_epoll_ctl
  svc #0
  pop {r7,pc}
.global epoll_wait
epoll_wait:
  push {r7,lr}
  mov r7, #SYS_epoll_wait
  svc #0
  pop {r7,pc}
.global fcntl
fcntl:
  push {r7,lr}
  mov r7, #SYS_fcntl
  svc #0
  pop {r7,pc}
.global pty_open
pty_open:
  push {r7,lr}
  mov r7, #SYS_pty_open
  svc #0
  pop {r7,pc}
.global getrandom
getrandom:
  push {r7,lr}
  mov r7, #SYS_getrandom
  svc #0
  pop {r7,pc}
.global rename
rename:
  push {r7,lr}
  mov r7, #SYS_rename
  svc #0
  pop {r7,pc}
.global sched_setscheduler
sched_setscheduler:
  push {r7,lr}
  mov r7, #SYS_sched_setscheduler
  svc #0
  pop {r7,pc}
.global sched_getinfo
sched_getinfo:
  push {r7,lr}
  mov r7, #SYS_sched_getinfo
  svc #0
  pop {r7,pc}
.global setnice
setnice:
  push {r7,lr}
  mov r7, #SYS_setnice
  svc #0
  pop {r7,pc}
.global sched_setaffinity
sched_setaffinity:
  push {r7,lr}
  mov r7, #SYS_sched_setaffinity
  svc #0
  pop {r7,pc}
.global sched_yield
sched_yield:
  push {r7,lr}
  mov r7, #SYS_sched_yield
  svc #0
  pop {r7,pc}
.global clock_us
clock_us:
  push {r7,lr}
  mov r7, #SYS_clock_us
  svc #0
  pop {r7,pc}
.global getcpu
getcpu:
  push {r7,lr}
  mov r7, #SYS_getcpu
  svc #0
  pop {r7,pc}
.global clone
clone:
  push {r7,lr}
  mov r7, #SYS_clone
  svc #0
  pop {r7,pc}
.global texit
texit:
  push {r7,lr}
  mov r7, #SYS_texit
  svc #0
  pop {r7,pc}
.global tjoin
tjoin:
  push {r7,lr}
  mov r7, #SYS_tjoin
  svc #0
  pop {r7,pc}
.global gettid
gettid:
  push {r7,lr}
  mov r7, #SYS_gettid
  svc #0
  pop {r7,pc}
.global socket
socket:
  push {r7,lr}
  mov r7, #SYS_socket
  svc #0
  pop {r7,pc}
.global bind
bind:
  push {r7,lr}
  mov r7, #SYS_bind
  svc #0
  pop {r7,pc}
.global listen
listen:
  push {r7,lr}
  mov r7, #SYS_listen
  svc #0
  pop {r7,pc}
.global lseek
lseek:
  push {r7,lr}
  mov r7, #SYS_lseek
  svc #0
  pop {r7,pc}

.section .note.GNU-stack,"",%progbits
