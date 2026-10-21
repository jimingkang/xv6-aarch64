K=kernel
U=user
KERNEL_IMAGE = $K/kernel8-xv6_wifi.img

.DEFAULT_GOAL := build
.SECONDEXPANSION:

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
RPI3_EXT2_CMDLINE ?= cmdline_ext2.txt
# e2fsprogs is a Homebrew keg whose sbin is not on the default PATH.
MKE2FS ?= $(shell if command -v mke2fs >/dev/null 2>&1; then \
	command -v mke2fs; \
	elif test -x /opt/homebrew/opt/e2fsprogs/sbin/mke2fs; then \
	echo /opt/homebrew/opt/e2fsprogs/sbin/mke2fs; \
	elif test -x /usr/local/opt/e2fsprogs/sbin/mke2fs; then \
	echo /usr/local/opt/e2fsprogs/sbin/mke2fs; \
	else echo mke2fs; fi)
DEBUGFS ?= $(shell if command -v debugfs >/dev/null 2>&1; then \
	command -v debugfs; \
	elif test -x /opt/homebrew/opt/e2fsprogs/sbin/debugfs; then \
	echo /opt/homebrew/opt/e2fsprogs/sbin/debugfs; \
	elif test -x /usr/local/opt/e2fsprogs/sbin/debugfs; then \
	echo /usr/local/opt/e2fsprogs/sbin/debugfs; \
	else echo debugfs; fi)
WIFI_FIRMWARE_DIR ?= firmware
SUDO ?= sudo

# Linux-inspired source layering.  Kernel-private common headers live in
# include/kernel, user/kernel ABI headers live in include/uapi, and
# implementation files live with their owning subsystem.
ARCH_OBJS = \
  $K/arch/arm64/entry.o \
  $K/arch/arm64/start.o \
  $K/arch/arm64/swtch.o \
  $K/arch/arm64/trapasm.o \
  $K/arch/arm64/timer.o \
  $K/arch/arm64/bcm2837.o

INIT_OBJS = \
  $K/init/main.o \
  $K/init/cmdline.o \
  $K/init/do_mounts.o

CORE_OBJS = \
  $K/core/proc.o \
  $K/core/workqueue.o \
  $K/core/trap.o \
  $K/core/syscall.o \
  $K/core/sysproc.o \
  $K/core/reboot.o \
  $K/core/sync.o \
  $K/core/spinlock.o \
  $K/core/sleeplock.o

MM_OBJS = \
  $K/mm/kalloc.o \
  $K/mm/vm.o

LIB_OBJS = \
  $K/lib/printf.o \
  $K/lib/rng.o \
  $K/lib/string.o

DRIVER_BASE_OBJS = \
  $K/drivers/base/device.o \
  $K/drivers/base/kobject.o \
  $K/drivers/base/module.o \
  $K/drivers/base/export.o

DRIVER_MMC_OBJS = \
  $K/drivers/mmc/core/sdio.o \
  $K/drivers/mmc/host/arasan_sdio.o \
  $K/drivers/mmc/host/sdhost.o

DRIVER_USB_OBJS = \
  $K/drivers/usb/core/usb.o \
  $K/drivers/usb/host/dwc2.o \
  $K/drivers/input/input.o \
  $K/drivers/input/usbkbd.o \
  $K/drivers/net/usb/usbnet.o

DRIVER_NET_OBJS = \
  $K/drivers/net/wireless/mt7601u.o \
  $K/drivers/net/wireless/brcmfmac.o \
  $K/drivers/net/wireless/wpa_crypto.o

DRIVER_TTY_OBJS = \
  $K/drivers/tty/console.o \
  $K/drivers/tty/tty.o \
  $K/drivers/tty/pty.o \
  $K/drivers/tty/uart.o

DRIVER_BLOCK_OBJS = \
  $K/drivers/block/blkdev.o \
  $K/drivers/block/rootdev.o \
  $K/drivers/block/rootupdate.o

DRIVER_MEDIA_OBJS = \
  $K/drivers/firmware/mbox.o \
  $K/drivers/media/i2c/i2c.o \
  $K/drivers/media/i2c/ov5647.o \
  $K/drivers/media/platform/unicam.o

FS_OBJS = \
  $K/fs/bio.o \
  $K/fs/fs.o \
  $K/fs/vfs.o \
  $K/fs/log.o \
  $K/fs/file.o \
  $K/fs/epoll.o \
  $K/fs/pipe.o \
  $K/fs/exec.o \
  $K/fs/sysfile.o \
  $K/fs/fat/fat32.o \
  $K/fs/ext2/ext2.o \
  $K/fs/ext2/xjournal.o \
  $K/fs/netfs/netfs.o

NET_OBJS = \
  $K/net/net.o \
  $K/net/sysnet.o

OBJS = $(ARCH_OBJS) $(INIT_OBJS) $(CORE_OBJS) $(MM_OBJS) $(LIB_OBJS) \
  $(DRIVER_BASE_OBJS) $(DRIVER_MMC_OBJS) $(DRIVER_USB_OBJS) \
  $(DRIVER_NET_OBJS) $(DRIVER_TTY_OBJS) $(DRIVER_BLOCK_OBJS) \
  $(DRIVER_MEDIA_OBJS) $(FS_OBJS) $(NET_OBJS)

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
CFLAGS += -I$K
CFLAGS += $(shell $(CC) -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector)

# Keep libc/POSIX headers out of the kernel include namespace.  Kernel and
# user code both see include/uapi, while only user objects see user/include.
USER_CFLAGS = $(CFLAGS) -I$U/include

# Verbose filesystem teaching trace.  Use `make FS_TRACE=1` to print the
# superblock, inode-cache and pathname-walk call chain (for example `ls /`).
FS_TRACE ?= 0
CFLAGS += -DFS_TRACE=$(FS_TRACE)

# Page-table/process-context teaching trace.  VM_TRACE=1 prints every valid
# L1/L2/L3 descriptor in TTBR1 once at boot and a complete TTBR0/context/
# trapframe snapshot whenever a user image is created or replaced.
VM_TRACE ?= 0
CFLAGS += -DVM_TRACE=$(VM_TRACE)

# Disable PIE when possible (for Ubuntu 16.10 toolchain)
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]no-pie'),)
CFLAGS += -fno-pie -no-pie
endif
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]nopie'),)
CFLAGS += -fno-pie -nopie
endif

LDFLAGS = -z max-page-size=4096
ASFLAGS = -Og -ggdb -mcpu=cortex-a53 -MD -I. -Iinclude -I$K

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

$K/armstub.o: $K/arch/arm64/armstub.S
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
install-rpi3: $(KERNEL_IMAGE) fs.img config.txt cmdline_xv6.txt
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
	$(SUDO) cp -f cmdline_xv6.txt "$(RPI3_BOOTFS)/cmdline.txt"   # xv6 root=; kernel reads /cmdline.txt (config.txt sets disable_commandline_tags=1)
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
install-rpi3-ext2: $(KERNEL_IMAGE) $(EXT2_IMAGE)
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
	@test -d "$(RPI3_BOOTFS)" || { \
		echo "error: $(RPI3_BOOTFS) is not mounted" 1>&2; \
		exit 1; \
	}
	$(SUDO) dd if=$(EXT2_IMAGE) of="$(RPI3_EXT2_DEV)" bs=1048576 conv=sync
	$(SUDO) cp -f $(KERNEL_IMAGE) "$(RPI3_BOOTFS)/$(RPI3_KERNEL_NAME)"
	sync
	@cmp -s $(KERNEL_IMAGE) "$(RPI3_BOOTFS)/$(RPI3_KERNEL_NAME)" || { \
		echo "error: $(RPI3_BOOTFS)/$(RPI3_KERNEL_NAME) differs from $(KERNEL_IMAGE)" 1>&2; \
		exit 1; \
	}
	@echo "installed $(EXT2_IMAGE) -> $(RPI3_EXT2_DEV) (ext2 partition)"
	@echo "installed $(KERNEL_IMAGE) -> $(RPI3_BOOTFS)/$(RPI3_KERNEL_NAME)"

# Build a fresh, self-contained ext2 root image.  Keeping image construction
# separate from the raw-device recipe makes it possible to inspect/test the
# exact bytes which will be installed before touching an SD-card partition.
.PHONY: ext2root-image
ext2root-image: $$(UPROGS) $U/libc.so $U/ld-xv6.so modules_install
	truncate -s $(EXT2_SIZE) "$(EXT2_IMAGE)"
	$(MKE2FS) -F -t ext2 -b 4096 \
	  -O filetype,sparse_super,large_file,^has_journal,^extent,^64bit,^metadata_csum \
	  -L $(EXT2_LABEL) "$(EXT2_IMAGE)"
	@{ echo "mkdir bin"; \
	   for f in $(UPROGS); do n=$${f#$U/_}; echo "write $$f bin/$$n"; done; \
	   echo "write $U/_init init"; \
	   echo "mkdir lib"; echo "write $U/libc.so lib/libc.so"; \
	   echo "write $U/ld-xv6.so lib/ld-xv6.so"; \
	   echo "mkdir lib/modules"; \
	   for f in $(MODULES); do n=$${f##*/}; echo "write $(MODULE_INSTALL_DIR)/$$n lib/modules/$$n"; done; } > ext2root.debugfs
	$(DEBUGFS) -w -f ext2root.debugfs "$(EXT2_IMAGE)" > ext2root.log 2>&1
	@echo "built ext2 root image $(EXT2_IMAGE) ($(EXT2_SIZE))"

# Install a complete ext2-root SD-card setup.  This target deliberately
# reformats only the explicitly named partition by writing EXT2_IMAGE to it;
# it never accepts a whole-disk device.  Kernel/config/cmdline/firmware are
# synchronized to bootfs in the same operation so root and kernel stay paired.
.PHONY: install-rpi3-ext2root
install-rpi3-ext2root: $(KERNEL_IMAGE) config.txt $(RPI3_EXT2_CMDLINE) ext2root-image
	@test -n "$(RPI3_EXT2_DEV)" || { \
		echo "error: set RPI3_EXT2_DEV to the ext2 partition, never the whole disk" 1>&2; \
		echo "example: make install-rpi3-ext2root RPI3_EXT2_DEV=/dev/rdisk4s3" 1>&2; \
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
	@test -d "$(RPI3_BOOTFS)" || { \
		echo "error: $(RPI3_BOOTFS) is not mounted" 1>&2; \
		exit 1; \
	}
	@echo "WARNING: overwriting ext2 filesystem on $(RPI3_EXT2_DEV) with $(EXT2_IMAGE)"
	$(SUDO) dd if="$(EXT2_IMAGE)" of="$(RPI3_EXT2_DEV)" bs=1048576 conv=sync
	$(SUDO) cp -f $(KERNEL_IMAGE) "$(RPI3_BOOTFS)/$(RPI3_KERNEL_NAME)"
	$(SUDO) cp -f config.txt "$(RPI3_BOOTFS)/config.txt"
	$(SUDO) cp -f "$(RPI3_EXT2_CMDLINE)" "$(RPI3_BOOTFS)/cmdline.txt"
	@if test -f "$(WIFI_FIRMWARE_DIR)/BCM43430.BIN" && \
	    test -f "$(WIFI_FIRMWARE_DIR)/BCM43430.TXT"; then \
		$(SUDO) cp -f "$(WIFI_FIRMWARE_DIR)/BCM43430.BIN" "$(RPI3_BOOTFS)/BCM43430.BIN"; \
		$(SUDO) cp -f "$(WIFI_FIRMWARE_DIR)/BCM43430.TXT" "$(RPI3_BOOTFS)/BCM43430.TXT"; \
		test ! -f "$(WIFI_FIRMWARE_DIR)/BCM43430.CLM" || \
		  $(SUDO) cp -f "$(WIFI_FIRMWARE_DIR)/BCM43430.CLM" "$(RPI3_BOOTFS)/BCM43430.CLM"; \
	elif test -f "$(WIFI_FIRMWARE_DIR)/brcmfmac43430-sdio.bin" && \
	     test -f "$(WIFI_FIRMWARE_DIR)/brcmfmac43430-sdio.txt"; then \
		$(SUDO) cp -f "$(WIFI_FIRMWARE_DIR)/brcmfmac43430-sdio.bin" "$(RPI3_BOOTFS)/BCM43430.BIN"; \
		$(SUDO) cp -f "$(WIFI_FIRMWARE_DIR)/brcmfmac43430-sdio.txt" "$(RPI3_BOOTFS)/BCM43430.TXT"; \
	else \
		echo "BCM43430 firmware not installed (BIN/TXT required; set WIFI_FIRMWARE_DIR=...)"; \
	fi
	@if test -f "$(WIFI_FIRMWARE_DIR)/BCM43455.BIN" && \
	    test -f "$(WIFI_FIRMWARE_DIR)/BCM43455.TXT" && \
	    test -f "$(WIFI_FIRMWARE_DIR)/BCM43455.CLM"; then \
		$(SUDO) cp -f "$(WIFI_FIRMWARE_DIR)/BCM43455.BIN" "$(RPI3_BOOTFS)/BCM43455.BIN"; \
		$(SUDO) cp -f "$(WIFI_FIRMWARE_DIR)/BCM43455.TXT" "$(RPI3_BOOTFS)/BCM43455.TXT"; \
		$(SUDO) cp -f "$(WIFI_FIRMWARE_DIR)/BCM43455.CLM" "$(RPI3_BOOTFS)/BCM43455.CLM"; \
	fi
	@if test -f "$(WIFI_FIRMWARE_DIR)/mt7601u.bin"; then \
		$(SUDO) cp -f "$(WIFI_FIRMWARE_DIR)/mt7601u.bin" "$(RPI3_BOOTFS)/MT7601U.BIN"; \
	fi
	sync
	@cmp -s $(KERNEL_IMAGE) "$(RPI3_BOOTFS)/$(RPI3_KERNEL_NAME)" || { \
		echo "error: installed kernel differs from $(KERNEL_IMAGE)" 1>&2; exit 1; \
	}
	@cmp -s "$(RPI3_EXT2_CMDLINE)" "$(RPI3_BOOTFS)/cmdline.txt" || { \
		echo "error: installed ext2 cmdline differs from $(RPI3_EXT2_CMDLINE)" 1>&2; exit 1; \
	}
	@echo "installed $(EXT2_IMAGE) -> $(RPI3_EXT2_DEV) (ext2 root partition)"
	@echo "installed /init and $(words $(UPROGS)) programs in /bin, $(words $(MODULES)) modules in /lib/modules"
	@echo "installed $(KERNEL_IMAGE), config.txt and ext2 cmdline -> $(RPI3_BOOTFS)"

$U/initcode: $U/initcode.S
	$(CC) $(CFLAGS) -nostdinc -I. -Ikernel -c $U/initcode.S -o $U/initcode.o
	$(LD) $(LDFLAGS) -N -e start -Ttext 0 -o $U/initcode.out $U/initcode.o
	$(OBJCOPY) -S -O binary $U/initcode.out $U/initcode
	$(OBJDUMP) -S $U/initcode.o > $U/initcode.asm

tags: $(OBJS) _init
	etags *.S *.c

LIBC_OBJS = $U/libc_syscall.o $U/ulib.o $U/usys.o $U/printf.o $U/umalloc.o $U/thread.o \
	$U/posix.o $U/tweetnacl.o $U/ssh_crypto.o
LIBC_PIC_OBJS = $(LIBC_OBJS:.o=.pic.o)

# User programs and libc use the private user-facing libc/POSIX headers.
$U/%.o: $U/%.c
	$(CC) $(USER_CFLAGS) -c -o $@ $<

# Shared libc is the normal user ABI.  DYNAMIC_USER=0 retains the original
# static ET_EXEC build for recovery images and dynamic-loader debugging.
DYNAMIC_USER ?= 1
ifeq ($(DYNAMIC_USER),1)
ULIB = $U/libc.so
USER_LD_MODE = -pie -dynamic-linker /lib/ld-xv6.so
USER_LINK_STAMP = $U/.dynamic-user
else
ULIB = $U/libc.a
USER_LD_MODE = -N -Ttext 0
USER_LINK_STAMP = $U/.static-user
endif

$U/.dynamic-user:
	rm -f $U/.static-user
	touch $@

$U/.static-user:
	rm -f $U/.dynamic-user
	touch $@

$U/libc.o: $(LIBC_OBJS)
	$(LD) -r -o $@ $^

$U/libc.a: $(LIBC_OBJS) $U/libc.o
	$(AR) rcs $@ $(LIBC_OBJS)

# ELF shared libc loaded and relocated by the user-space interpreter.
$U/%.pic.o: $U/%.c
	$(CC) $(USER_CFLAGS) -fPIC -c -o $@ $<

$U/usys.pic.o: $U/usys.S
	$(CC) $(USER_CFLAGS) -fPIC -c -o $@ $<

$U/libc.so: $(LIBC_PIC_OBJS)
	$(LD) $(LDFLAGS) -shared --no-undefined -soname libc.so -o $@ $^

# The ELF interpreter is deliberately self-contained.  It executes before
# libc has been mapped or relocated and uses raw syscalls through usys.pic.o.
# It is a relocation-free ET_DYN image, so exec may choose its load bias and
# only has to map PT_LOAD segments and transfer control, as Linux does.
$U/ld-xv6.so: $U/ldso.pic.o $U/usys.pic.o
	$(LD) $(LDFLAGS) -shared -Bsymbolic -e ldso_start -o $@ $^

_%: %.o $(ULIB) $(USER_LINK_STAMP)
	$(LD) $(LDFLAGS) $(USER_LD_MODE) -e main -o $@ $(filter-out $(USER_LINK_STAMP),$^)
	$(OBJDUMP) -S $@ > $*.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $*.sym

$U/usys.S : $U/usys.pl
	perl $U/usys.pl > $U/usys.S

$U/usys.o : $U/usys.S
	$(CC) $(USER_CFLAGS) -c -o $U/usys.o $U/usys.S

$U/_forktest: $U/forktest.o $(ULIB) $(USER_LINK_STAMP)
	# Keep forktest small while it exercises dynamic process-table growth.
	$(LD) $(LDFLAGS) $(USER_LD_MODE) -e main -o $U/_forktest $U/forktest.o $(ULIB)
	$(OBJDUMP) -S $U/_forktest > $U/forktest.asm

$U/_camshot: $U/camshot.o $U/jpeg.o $(ULIB) $(USER_LINK_STAMP)
	$(LD) $(LDFLAGS) $(USER_LD_MODE) -e main -o $@ $(filter-out $(USER_LINK_STAMP),$^)
	$(OBJDUMP) -S $@ > $U/camshot.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $U/camshot.sym

$U/_camserver: $U/camserver.o $U/camproc.o $U/jpeg.o $(ULIB) $(USER_LINK_STAMP)
	$(LD) $(LDFLAGS) $(USER_LD_MODE) -e main -o $@ $(filter-out $(USER_LINK_STAMP),$^)
	$(OBJDUMP) -S $@ > $U/camserver.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $U/camserver.sym

mkfs/mkfs: mkfs/mkfs.c mkfs/lsdir.c include/kernel/fs.h include/kernel/param.h
	gcc -Werror -Wall -idirafter include -o mkfs/mkfs mkfs/mkfs.c mkfs/lsdir.c

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
	$U/_insmod\
	$U/_rmmod\
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
	$U/_minisqlite\
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
	$U/_camserver\
	$U/_chrt\
	$U/_taskset\
	$U/_nice\
	$U/_renice\
	$U/_rtlat\
	$U/_evtest\
	$U/_spin\
	$U/_threadtest\
	$U/_reboot\
	$U/_wget\
	$U/_posixtest\

# ---------------------------------------------------------------------------
# Loadable kernel modules (.ko): the =m half of the driver model.
#
# A driver listed in MODULES is deliberately absent from OBJS, so it is NOT
# linked into the kernel image.  Instead each one is compiled to a relocatable
# object, partial-linked with `ld -r` (which keeps it ET_REL and full of
# relocations), stripped of debug sections, and written into the root
# filesystem under /lib/modules.  insmod reads it back and init_module()
# relocates it into the kernel's module arena -- the same build-time split
# Linux draws between built-in drivers and modules shipped as files.
# ---------------------------------------------------------------------------
MODULES = $K/drivers/input/usbmouse.ko

$K/%.mod.o: $K/%.c
	$(CC) $(CFLAGS) -DMODULE -c -o $@ $<

$K/%.ko: $K/%.mod.o
	$(LD) -r -o $@.raw $<
	$(OBJCOPY) --strip-debug $@.raw $@
	@rm -f $@.raw

# `make modules_install`, the Linux analogue: after a .ko is built it is copied
# into an installed-modules staging tree (build/lib/modules) rather than handed
# to mkfs by path.  mkfs then ingests that whole tree into the image's
# /lib/modules, exactly as a rootfs carries /lib/modules/$(uname -r)/ on Linux.
# Override MODULE_INSTALL_DIR to install somewhere else (e.g. a real host
# /lib/modules, which needs write permission there).
MODULE_INSTALL_DIR ?= build/lib/modules

modules_install: $(MODULES)
	@rm -rf $(MODULE_INSTALL_DIR)
	@mkdir -p $(MODULE_INSTALL_DIR)
	cp $(MODULES) $(MODULE_INSTALL_DIR)/

fs.img: mkfs/mkfs $(UPROGS) $U/libc.so $U/ld-xv6.so modules_install
	mkfs/mkfs fs.img -L $U/libc.so -I $U/ld-xv6.so -M $(MODULE_INSTALL_DIR) $(UPROGS)
	truncate -s 32M fs.img

# Linux/ext2 image used by the VFS /mnt/ext2 backend.  mke2fs formats a regular
# file with only the features kernel/fs/ext2/ext2.c understands (no journal, no extents,
# no 64-bit block numbers, no metadata checksums); see readme/readme_ext2.md.
$(EXT2_IMAGE):
	truncate -s $(EXT2_SIZE) $@
	$(MKE2FS) -F -t ext2 -b 4096 \
	  -O filetype,sparse_super,large_file,^has_journal,^extent,^64bit,^metadata_csum \
	  -L $(EXT2_LABEL) $@

KERNEL_DEPS = $(OBJS:.o=.d) $(MODULES:.ko=.mod.d)
-include $(KERNEL_DEPS) kernel/buildinfo.d user/*.d

.PHONY: clean-t113 modules_install
clean-t113:
	# T113 sources live in ports/t113; build/t113 contains generated files only.
	rm -rf build/t113

clean: clean-t113
	rm -f *.tex *.dvi *.idx *.aux *.log *.ind *.ilg \
	*/*.o */*.d */*.asm */*.sym \
	$U/initcode $U/initcode.out $K/kernel $(KERNEL_IMAGE) $K/kernel8.img \
	$K/buildinfo.c \
	$K/armstub.o $K/armstub.elf $K/armstub-xv6.bin fs.img $(EXT2_IMAGE) ext2root.debugfs \
	$(MODULES) $(MODULES:.ko=.mod.o) $(MODULES:%=%.raw) \
	mkfs/mkfs .gdbinit $U/libc.a $U/libc.so $U/ld-xv6.so $U/.dynamic-user $U/.static-user \
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
	find $K -mindepth 2 -type f \( -name '*.o' -o -name '*.d' -o -name '*.asm' -o -name '*.sym' -o -name '*.ko.raw' \) -delete
	rm -rf $U/obj $U/.deps $U/build $K/obj $K/.deps $K/build
	rm -rf $(MODULE_INSTALL_DIR)

.PHONY: clean

# try to generate a unique GDB port
GDBPORT = $(shell expr `id -u` % 5000 + 25000)
# QEMU's gdb stub command line changed in 0.11
QEMUGDB = $(shell if $(QEMU) -help | grep -q '^-gdb'; \
	then echo "-gdb tcp:127.0.0.1:$(GDBPORT)"; \
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
