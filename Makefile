K=kernel
U=user
KERNEL_IMAGE = $K/kernel8-xv6_wifi.img

.DEFAULT_GOAL := build

RPI3_BOOTFS ?= /Volumes/bootfs
RPI3_KERNEL_NAME ?= kernel8-xv6_wifi.img
RPI3_ARMSTUB_NAME ?= armstub-xv6.bin
TFTPBOOT_DIR ?= /private/tftpboot
TFTPBOOT_KERNEL ?= $(TFTPBOOT_DIR)/$(RPI3_KERNEL_NAME)
TFTPBOOT_FS ?= $(TFTPBOOT_DIR)/fs.img
# Raw xv6 partition device.  Intentionally empty: callers must name the exact
# partition (for example /dev/rdisk4s2) to prevent accidental whole-disk writes.
RPI3_XV6_DEV ?=
# ext2 image target partition.  Empty by default: callers must name the exact
# partition (for example /dev/rdisk4s3) to prevent accidental whole-disk writes.
RPI3_EXT2_DEV ?=
EXT2_IMAGE ?= fs_ext2.img
EXT2_SIZE ?= 32M
EXT2_LABEL ?= xv6-linux
# e2fsprogs is a Homebrew keg whose sbin is not on the default PATH.
MKE2FS ?= $(shell if command -v mke2fs >/dev/null 2>&1; then \
	command -v mke2fs; \
	elif test -x /opt/homebrew/opt/e2fsprogs/sbin/mke2fs; then \
	echo /opt/homebrew/opt/e2fsprogs/sbin/mke2fs; \
	elif test -x /usr/local/opt/e2fsprogs/sbin/mke2fs; then \
	echo /usr/local/opt/e2fsprogs/sbin/mke2fs; \
	else echo mke2fs; fi)
WIFI_FIRMWARE_DIR ?= firmware
SUDO ?= sudo

OBJS = \
  $K/entry.o \
  $K/start.o \
  $K/console.o \
	$K/device.o \
	$K/sdio.o \
	$K/usb.o \
	$K/input.o \
	$K/usbkbd.o \
	$K/arasan_sdio.o \
	$K/mt7601u.o \
	$K/brcmfmac.o \
	$K/wpa_crypto.o \
	$K/tty.o \
  $K/printf.o \
  $K/uart.o \
  $K/kalloc.o \
  $K/spinlock.o \
  $K/rng.o \
  $K/string.o \
  $K/main.o \
  $K/vm.o \
  $K/proc.o \
  $K/workqueue.o \
  $K/swtch.o \
  $K/trap.o \
  $K/syscall.o \
  $K/sysproc.o \
  $K/bio.o \
  $K/fs.o \
	$K/vfs.o \
	$K/netfs.o \
  $K/log.o \
  $K/sleeplock.o \
  $K/sync.o \
  $K/net.o \
  $K/sysnet.o \
  $K/dwc2.o \
  $K/usbnet.o \
  $K/file.o \
  $K/epoll.o \
  $K/pipe.o \
  $K/pty.o \
  $K/exec.o \
  $K/sysfile.o \
  $K/trapasm.o \
  $K/timer.o \
  $K/sdhost.o \
	$K/fat32.o \
	$K/rootdev.o \
	$K/rootupdate.o \
	$K/ext2.o \
	$K/xjournal.o \
  $K/bcm2837.o \
  $K/mbox.o \
  $K/i2c.o \
  $K/ov5647.o \
  $K/unicam.o \

# Try to infer the correct TOOLPREFIX if not set
ifndef TOOLPREFIX
TOOLPREFIX := $(shell if aarch64-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'aarch64-elf-'; \
	elif aarch64-unknown-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'aarch64-unknown-elf-'; \
	elif aarch64-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'aarch64-linux-gnu-'; \
	elif aarch64-unknown-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'aarch64-unknown-linux-gnu-'; \
	else echo "***" 1>&2; \
	echo "*** Error: Couldn't find a aarch64 version of GCC/binutils." 1>&2; \
	echo "*** To turn off this error, run 'gmake TOOLPREFIX= ...'." 1>&2; \
	echo "***" 1>&2; exit 1; fi)
endif

#QEMUPREFIX = ~/qemu/build/
QEMU = $(QEMUPREFIX)qemu-system-aarch64

CC = $(TOOLPREFIX)gcc
AS = $(TOOLPREFIX)gas
LD = $(TOOLPREFIX)ld
OBJCOPY = $(TOOLPREFIX)objcopy
OBJDUMP = $(TOOLPREFIX)objdump
AR = $(TOOLPREFIX)ar

CFLAGS = -Wall -Werror -Os -g -fno-omit-frame-pointer -mcpu=cortex-a53+nofp
CFLAGS += -Wno-error=infinite-recursion
CFLAGS += -Wno-error=unused-but-set-variable
CFLAGS += -Wno-error=incompatible-pointer-types
CFLAGS += -MD
CFLAGS += -ffreestanding -fno-common -nostdlib
CFLAGS += -I.
CFLAGS += -Iinclude
CFLAGS += $(shell $(CC) -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector)

# Disable PIE when possible (for Ubuntu 16.10 toolchain)
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]no-pie'),)
CFLAGS += -fno-pie -no-pie
endif
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]nopie'),)
CFLAGS += -fno-pie -nopie
endif

LDFLAGS = -z max-page-size=4096
ASFLAGS = -Og -ggdb -mcpu=cortex-a53 -MD -I.

# kernel/buildinfo.c is regenerated on every link, so the boot banner shows
# when this kernel image was produced (not when main.c last compiled) and
# which git branch/commit it came from ("+dirty": uncommitted changes).
$K/kernel: $(OBJS) $K/kernel.ld $U/initcode
	@echo 'const char kernel_build_time[] = "'"$$(date '+%Y-%m-%d %H:%M:%S %Z')"'";' > $K/buildinfo.c
	@echo 'const char kernel_build_rev[] = "'"$$(GIT_OPTIONAL_LOCKS=0 git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown) $$(GIT_OPTIONAL_LOCKS=0 git describe --always --dirty=+dirty 2>/dev/null || echo -)"'";' >> $K/buildinfo.c
	$(CC) $(CFLAGS) -c -o $K/buildinfo.o $K/buildinfo.c
	$(LD) $(LDFLAGS) -T $K/kernel.ld -o $K/kernel $(OBJS) $K/buildinfo.o
	$(OBJDUMP) -S $K/kernel > $K/kernel.asm
	$(OBJDUMP) -t $K/kernel | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $K/kernel.sym

$K/kernel8-xv6_wifi.img: $K/kernel
	$(OBJCOPY) -O binary $< $@

$K/armstub.o: $K/armstub.S
	$(CC) $(ASFLAGS) -c -o $@ $<

$K/armstub.elf: $K/armstub.o
	$(LD) --section-start=.text=0 -o $@ $<

$K/armstub-xv6.bin: $K/armstub.elf
	$(OBJCOPY) -O binary $< $@

.PHONY: build
build: $(KERNEL_IMAGE) fs.img config.txt install-tftp

# Keep the image exported by the host TFTP server synchronized with the image
# just built.  /private/tftpboot is normally owned by root on macOS, hence
# SUDO is used just like the physical-SD installation targets below.
.PHONY: install-tftp
install-tftp: $(KERNEL_IMAGE) fs.img
	@test -d "$(TFTPBOOT_DIR)" || { \
		echo "error: TFTPBOOT_DIR $(TFTPBOOT_DIR) does not exist" 1>&2; \
		exit 1; \
	}
	$(SUDO) cp -f $(KERNEL_IMAGE) "$(TFTPBOOT_KERNEL)"
	$(SUDO) cp -f fs.img "$(TFTPBOOT_FS)"
	@cmp -s $(KERNEL_IMAGE) "$(TFTPBOOT_KERNEL)" || { \
		echo "error: TFTP image differs from $(KERNEL_IMAGE)" 1>&2; \
		exit 1; \
	}
	@cmp -s fs.img "$(TFTPBOOT_FS)" || { \
		echo "error: TFTP image differs from fs.img" 1>&2; \
		exit 1; \
	}
	@echo "installed $(KERNEL_IMAGE) -> $(TFTPBOOT_KERNEL)"
	@echo "installed fs.img -> $(TFTPBOOT_FS)"

.PHONY: install-rpi3
install-rpi3: $(KERNEL_IMAGE) fs.img config.txt
	@test -d "$(RPI3_BOOTFS)" || { \
		echo "error: $(RPI3_BOOTFS) is not mounted" 1>&2; \
		exit 1; \
	}
	$(SUDO) cp -f $(KERNEL_IMAGE) "$(RPI3_BOOTFS)/$(RPI3_KERNEL_NAME)"
	@if test -n "$(RPI3_XV6_DEV)"; then \
		case "$(RPI3_XV6_DEV)" in \
		  /dev/disk*s[0-9]*|/dev/rdisk*s[0-9]*) ;; \
		  *) echo "error: RPI3_XV6_DEV must be a partition device such as /dev/rdisk4s3" 1>&2; exit 1 ;; \
		esac; \
		test -e "$(RPI3_XV6_DEV)" || { echo "error: $(RPI3_XV6_DEV) does not exist" 1>&2; exit 1; }; \
		$(SUDO) dd if=fs.img of="$(RPI3_XV6_DEV)" bs=1048576 conv=sync; \
	else \
		echo "warning: fs.img not installed; set RPI3_XV6_DEV to the raw xv6 partition" 1>&2; \
	fi
	$(SUDO) cp -f config.txt "$(RPI3_BOOTFS)/config.txt"
	@cmp -s $(KERNEL_IMAGE) "$(RPI3_BOOTFS)/$(RPI3_KERNEL_NAME)" || { \
		echo "error: installed kernel differs from $(KERNEL_IMAGE)" 1>&2; \
		exit 1; \
	}
	@cmp -s config.txt "$(RPI3_BOOTFS)/config.txt" || { \
		echo "error: installed config.txt differs from workspace" 1>&2; \
		exit 1; \
	}
	@if test -f "$(WIFI_FIRMWARE_DIR)/brcmfmac43430-sdio.bin" && \
	    test -f "$(WIFI_FIRMWARE_DIR)/brcmfmac43430-sdio.txt"; then \
		$(SUDO) cp -f "$(WIFI_FIRMWARE_DIR)/brcmfmac43430-sdio.bin" "$(RPI3_BOOTFS)/BCM43430.BIN"; \
		$(SUDO) cp -f "$(WIFI_FIRMWARE_DIR)/brcmfmac43430-sdio.txt" "$(RPI3_BOOTFS)/BCM43430.TXT"; \
		if test -f "$(WIFI_FIRMWARE_DIR)/brcmfmac43430-sdio.clm_blob"; then \
			$(SUDO) cp -f "$(WIFI_FIRMWARE_DIR)/brcmfmac43430-sdio.clm_blob" "$(RPI3_BOOTFS)/BCM43430.CLM"; \
		fi; \
		echo "installed BCM43430 firmware files -> $(RPI3_BOOTFS)"; \
	else \
		echo "BCM43430 firmware not installed (BIN/TXT required; set WIFI_FIRMWARE_DIR=...)"; \
	fi
	@if test -f "$(WIFI_FIRMWARE_DIR)/brcmfmac43455-sdio.bin" && \
	    test -f "$(WIFI_FIRMWARE_DIR)/brcmfmac43455-sdio.txt" && \
	    test -f "$(WIFI_FIRMWARE_DIR)/brcmfmac43455-sdio.clm_blob"; then \
		$(SUDO) cp -f "$(WIFI_FIRMWARE_DIR)/brcmfmac43455-sdio.bin" "$(RPI3_BOOTFS)/BCM43455.BIN"; \
		$(SUDO) cp -f "$(WIFI_FIRMWARE_DIR)/brcmfmac43455-sdio.txt" "$(RPI3_BOOTFS)/BCM43455.TXT"; \
		$(SUDO) cp -f "$(WIFI_FIRMWARE_DIR)/brcmfmac43455-sdio.clm_blob" "$(RPI3_BOOTFS)/BCM43455.CLM"; \
		echo "installed BCM43455 firmware files -> $(RPI3_BOOTFS)"; \
	fi
	@if test -f "$(WIFI_FIRMWARE_DIR)/mt7601u.bin"; then \
		$(SUDO) cp -f "$(WIFI_FIRMWARE_DIR)/mt7601u.bin" "$(RPI3_BOOTFS)/MT7601U.BIN"; \
		echo "installed MT7601U firmware -> $(RPI3_BOOTFS)/MT7601U.BIN"; \
	else \
		echo "MT7601U firmware not installed (set WIFI_FIRMWARE_DIR=...)"; \
	fi
	sync
	@echo "installed $(KERNEL_IMAGE) -> $(RPI3_BOOTFS)/$(RPI3_KERNEL_NAME)"
	@if test -n "$(RPI3_XV6_DEV)"; then \
		echo "installed fs.img -> $(RPI3_XV6_DEV) (raw xv6 partition)"; \
	else \
		echo "skipped fs.img installation (RPI3_XV6_DEV is empty)"; \
	fi
	@echo "installed config.txt -> $(RPI3_BOOTFS)/config.txt (firmware armstub8)"

.PHONY: install-rpi3-rawfs
install-rpi3-rawfs: fs.img
	@test -n "$(RPI3_XV6_DEV)" || { \
		echo "error: set RPI3_XV6_DEV to the xv6 partition, never the whole disk" 1>&2; \
		echo "example: make install-rpi3-rawfs RPI3_XV6_DEV=/dev/rdisk4s3" 1>&2; \
		exit 1; \
	}
	@case "$(RPI3_XV6_DEV)" in \
	  /dev/disk*s[0-9]*|/dev/rdisk*s[0-9]*) ;; \
	  *) echo "error: RPI3_XV6_DEV must be a partition device, never a whole disk" 1>&2; exit 1 ;; \
	esac
	@test -e "$(RPI3_XV6_DEV)" || { \
		echo "error: $(RPI3_XV6_DEV) does not exist" 1>&2; \
		exit 1; \
	}
	$(SUDO) dd if=fs.img of="$(RPI3_XV6_DEV)" bs=1048576 conv=sync
	sync
	@echo "installed fs.img -> $(RPI3_XV6_DEV) (raw xv6 partition)"

.PHONY: install-rpi3-ext2
install-rpi3-ext2: $(EXT2_IMAGE)
	@test -n "$(RPI3_EXT2_DEV)" || { \
		echo "error: set RPI3_EXT2_DEV to the ext2 partition, never the whole disk" 1>&2; \
		echo "example: make install-rpi3-ext2 RPI3_EXT2_DEV=/dev/rdisk4s3" 1>&2; \
		exit 1; \
	}
	@case "$(RPI3_EXT2_DEV)" in \
	  /dev/disk*s[0-9]*|/dev/rdisk*s[0-9]*) ;; \
	  *) echo "error: RPI3_EXT2_DEV must be a partition device, never a whole disk" 1>&2; exit 1 ;; \
	esac
	@test -e "$(RPI3_EXT2_DEV)" || { \
		echo "error: $(RPI3_EXT2_DEV) does not exist" 1>&2; \
		exit 1; \
	}
	$(SUDO) dd if=$(EXT2_IMAGE) of="$(RPI3_EXT2_DEV)" bs=1048576 conv=sync
	sync
	@echo "installed $(EXT2_IMAGE) -> $(RPI3_EXT2_DEV) (ext2 partition)"

$U/initcode: $U/initcode.S
	$(CC) $(CFLAGS) -nostdinc -I. -Ikernel -c $U/initcode.S -o $U/initcode.o
	$(LD) $(LDFLAGS) -N -e start -Ttext 0 -o $U/initcode.out $U/initcode.o
	$(OBJCOPY) -S -O binary $U/initcode.out $U/initcode
	$(OBJDUMP) -S $U/initcode.o > $U/initcode.asm

tags: $(OBJS) _init
	etags *.S *.c

LIBC_OBJS = $U/ulib.o $U/usys.o $U/printf.o $U/umalloc.o $U/thread.o \
	$U/posix.o $U/tweetnacl.o $U/ssh_crypto.o
ULIB = $U/libc.a

$U/libc.a: $(LIBC_OBJS)
	$(AR) rcs $@ $^

_%: %.o $(ULIB)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $*.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $*.sym

$U/usys.S : $U/usys.pl
	perl $U/usys.pl > $U/usys.S

$U/usys.o : $U/usys.S
	$(CC) $(CFLAGS) -c -o $U/usys.o $U/usys.S

$U/_forktest: $U/forktest.o $(ULIB)
	# Keep forktest small while it exercises dynamic process-table growth.
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $U/_forktest $U/forktest.o $(ULIB)
	$(OBJDUMP) -S $U/_forktest > $U/forktest.asm

$U/_camshot: $U/camshot.o $U/jpeg.o $(ULIB)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $U/camshot.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $U/camshot.sym

mkfs/mkfs: mkfs/mkfs.c $K/fs.h $K/param.h
	gcc -Werror -Wall -I. -o mkfs/mkfs mkfs/mkfs.c

# Prevent deletion of intermediate files, e.g. cat.o, after first build, so
# that disk image changes after first build are persistent until clean.  More
# details:
# http://www.gnu.org/software/make/manual/html_node/Chained-Rules.html
.PRECIOUS: %.o

UPROGS=\
	$U/_cat\
	$U/_echo\
	$U/_forktest\
	$U/_grep\
	$U/_init\
	$U/_kill\
	$U/_ln\
	$U/_login\
	$U/_ls\
	$U/_mkdir\
	$U/_mv\
	$U/_touch\
	$U/_file\
	$U/_edit\
	$U/_ps\
	$U/_syncdemo\
	$U/_prodcons\
	$U/_nettest\
	$U/_netdns\
	$U/_tftpclient\
	$U/_tftpd\
	$U/_ping\
	$U/_wifi\
	$U/_dhcp\
	$U/_dd\
	$U/_waltest\
	$U/_tcpd\
	$U/_epollserver\
	$U/_ptytest\
	$U/_sshd\
	$U/_ext2ls\
	$U/_ext2cat\
	$U/_tcc\
	$U/_rm\
	$U/_sh\
	$U/_stressfs\
	$U/_usertests\
	$U/_vmmap\
	$U/_grind\
	$U/_wc\
	$U/_zombie\
	$U/_camshot\
	$U/_chrt\
	$U/_taskset\
	$U/_nice\
	$U/_renice\
	$U/_rtlat\
	$U/_spin\
	$U/_threadtest\
	$U/_posixtest\

fs.img: mkfs/mkfs $(UPROGS)
	mkfs/mkfs fs.img $(UPROGS)
	truncate -s 32M fs.img

# Linux/ext2 image used by the VFS /mnt/ext2 backend.  mke2fs formats a regular
# file with only the features kernel/ext2.c understands (no journal, no extents,
# no 64-bit block numbers, no metadata checksums); see readme/readme_ext2.md.
$(EXT2_IMAGE):
	truncate -s $(EXT2_SIZE) $@
	$(MKE2FS) -F -t ext2 -b 4096 \
	  -O filetype,sparse_super,large_file,^has_journal,^extent,^64bit,^metadata_csum \
	  -L $(EXT2_LABEL) $@

-include kernel/*.d user/*.d

.PHONY: clean-t113
clean-t113:
	# T113 sources live in ports/t113; build/t113 contains generated files only.
	rm -rf build/t113

clean: clean-t113
	rm -f *.tex *.dvi *.idx *.aux *.log *.ind *.ilg \
	*/*.o */*.d */*.asm */*.sym \
	$U/initcode $U/initcode.out $K/kernel $(KERNEL_IMAGE) $K/kernel8.img \
	$K/buildinfo.c \
	$K/armstub.o $K/armstub.elf $K/armstub-xv6.bin fs.img $(EXT2_IMAGE) \
	mkfs/mkfs .gdbinit $U/libc.a \
        $U/usys.S \
	$(UPROGS)
	# Remove numbered duplicate build products created by repeated Finder/copy
	# operations (for example "_cat 12", "libc 28.a", "kernel 10").
	# Every pattern is restricted to a known generated basename/extension so
	# source files such as *.c, *.h, initcode.S and usys.pl remain untouched.
	find $U -maxdepth 1 -type f \( \
		-name '_* [0-9]*' -o \
		-name 'initcode [0-9]*' -o -name 'initcode [0-9]*.out' -o \
		-name 'libc [0-9]*.a' -o -name 'usys [0-9]*.S' \
	\) -delete
	find $K -maxdepth 1 -type f \( \
		-name 'kernel [0-9]*' -o -name 'kernel8 [0-9]*.img' -o \
		-name 'kernel8-xv6_wifi [0-9]*.img' -o \
		-name 'armstub [0-9]*.o' -o -name 'armstub [0-9]*.elf' -o \
		-name 'armstub-xv6 [0-9]*.bin' \
	\) -delete
	find mkfs -maxdepth 1 -type f -name 'mkfs [0-9]*' -delete
	rm -rf $U/obj $U/.deps $U/build $K/obj $K/.deps $K/build

.PHONY: clean

# try to generate a unique GDB port
GDBPORT = $(shell expr `id -u` % 5000 + 25000)
# QEMU's gdb stub command line changed in 0.11
QEMUGDB = $(shell if $(QEMU) -help | grep -q '^-gdb'; \
	then echo "-gdb tcp::$(GDBPORT)"; \
	else echo "-s -p $(GDBPORT)"; fi)
ifndef CPUS
CPUS := 4
endif

# Use the generated image by default. Override with SDIMAGE=/dev/diskN only
# after all volumes on that physical SD card have been unmounted.
SDIMAGE ?= fs.img

QEMUOPTS = -machine raspi3b -kernel $(KERNEL_IMAGE) -display none
# raspi3b serial[0] is PL011; serial[1] is the AUX Mini UART used by xv6.
QEMUOPTS += -serial null -serial mon:stdio
QEMUOPTS += -drive file=$(SDIMAGE),if=sd,format=raw

# QEMU user-mode NAT uses the host's active route (Wi-Fi on a MacBook) without
# requiring a tap device or root. raspi3b exposes the NIC as a USB CDC/RNDIS
# adapter; the guest still needs a DWC2 USB host + USB Ethernet driver.
QEMUNETOPTS = -netdev user,id=net0,ipv4=on,ipv6=off,net=10.0.2.0/24
QEMUNETOPTS += -device usb-net,netdev=net0,mac=52:54:00:12:34:56
QEMUNETOPTS += -object filter-dump,id=netdump,netdev=net0,file=packets.pcap

qemu: $(KERNEL_IMAGE) fs.img
	$(QEMU) $(QEMUOPTS)

.PHONY: qemu-net
qemu-net: $(KERNEL_IMAGE) fs.img
	$(QEMU) $(QEMUOPTS) $(QEMUNETOPTS)

.gdbinit: .gdbinit.tmpl-aarch64
	sed "s/:1234/:$(GDBPORT)/" < $^ > $@

qemu-gdb: $(KERNEL_IMAGE) .gdbinit fs.img
	@echo "*** Now run 'gdb' in another window." 1>&2
	$(QEMU) $(QEMUOPTS) -S $(QEMUGDB)
