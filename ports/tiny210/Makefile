# Invoke from repository root: make -f ports/tiny210/Makefile
# FriendlyARM Tiny210 (Samsung S5PV210, Cortex-A8).
# Layout follows rpi3-lock: shared ARM32 code in kernel/arch/arm, board code
# in kernel/arch/arm/mach-s5pv210, drivers in kernel/drivers/<subsystem>
# registered through module_init() (kernel/module.h).
.DEFAULT_GOAL := all
BUILD ?= build/tiny210
P := ports/tiny210
S := kernel/arch/arm
MACH := kernel/arch/arm/mach-s5pv210
ARM_CC ?= clang
ARM_LD ?= $(shell command -v arm-none-eabi-ld 2>/dev/null || command -v aarch64-elf-ld 2>/dev/null || echo arm-none-eabi-ld)
ARM_OBJCOPY ?= $(shell command -v arm-none-eabi-objcopy 2>/dev/null || command -v aarch64-elf-objcopy 2>/dev/null || echo arm-none-eabi-objcopy)
HOST_CC ?= cc
QEMU_ARM ?= qemu-system-arm
TARGET_FLAGS ?= --target=arm-none-eabi
# Board constants live in mach-s5pv210/board.h (DRAM, console UART, machine id).
BOARD_DEF = -DBOARD_TINY210
TINY210_DRAM_BASE = 0x20000000
BOARD_UART = 0xE2900000
# Keep 100x30 characters while making the one-pixel 8x16 font readable.
# Set FBCON_BOLD=0 to compare against the original glyphs on real hardware.
FBCON_BOLD ?= 1
# FriendlyARM's panel table uses inverted VCLK.  This switch exists for a
# controlled A/B test when the one-pixel pattern still has horizontal echoes.
LCD_VCLK_INVERT ?= 1
# USB/Wi-Fi diagnostic traces.  Error messages remain enabled regardless of
# these switches; the switches only add the normal call-flow/state details.
# MT7601U_* level 1 traces phases/packets; level 2 also traces registers or
# every 802.11 frame.  USB_XFER_TRACE is intentionally verbose.
# USB_WIFI_TRACE=1 enables all five switches at once.  Level 2 additionally
# enables MT7601U register and per-802.11-frame detail.  Any individual switch
# can still override the umbrella value on the make command line.
USB_WIFI_TRACE ?= 0
USB_CORE_TRACE ?= $(USB_WIFI_TRACE)
USB_ENUM_TRACE ?= $(USB_WIFI_TRACE)
USB_XFER_TRACE ?= $(USB_WIFI_TRACE)
MT7601U_CONFIG_TRACE ?= $(USB_WIFI_TRACE)
MT7601U_DATA_TRACE ?= $(USB_WIFI_TRACE)
# 1: WLAN frames on bulk OUT EP4 with QSEL=EDCA; 0: old EP8 + QSEL=MGMT.
MT7601U_TX_EDCA_EP4 ?= 1
# 0: skip the WCID/key table clear added in 3977211 (A/B test).
MT7601U_WCID_CLEAR ?= 1
MT7601U_SCAN_CHAN ?= 0
# Root file system blocks (1 KiB).
TINY210_FSBLOCKS ?= 4096
ifeq ($(QEMU_TEST),1)
KERNEL_LOAD = 0x40008000
EARLY_UART = 0x01C28000
PLAT_FLAGS = -DTINY210_QEMU
else
KERNEL_LOAD = $(shell printf '0x%08x' $$(( $(TINY210_DRAM_BASE) + 0x8000 )))
EARLY_UART = $(BOARD_UART)
PLAT_FLAGS = $(BOARD_DEF) -DS5PV210_LCD_INDEX=$(LCD_INDEX) -DS5PV210_LCD_VCLK_INVERT=$(LCD_VCLK_INVERT) $(if $(filter 1,$(S5PV210_USB_OTG)),-DS5PV210_USB_OTG)
endif
FLAGS = $(TARGET_FLAGS) -mcpu=cortex-a8 -marm -mfloat-abi=soft -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -Os -g -fno-omit-frame-pointer -ffunction-sections -fdata-sections -DXV6_ARM32 -DXV6_TINY210 -DXV6_HAVE_NET -DXV6_HAVE_PTY $(if $(filter 1,$(QEMU_TEST)),,-DXV6_HAVE_VFS -DXV6_NO_EMBEDDED_ROOT) -DFSSIZE_T113=$(TINY210_FSBLOCKS) -DEARLY_UART=$(EARLY_UART) -DFBCON_FONT_BOLD=$(FBCON_BOLD) -DUSB_CORE_TRACE=$(USB_CORE_TRACE) -DUSB_ENUM_TRACE=$(USB_ENUM_TRACE) -DUSB_XFER_TRACE=$(USB_XFER_TRACE) -DMT7601U_CONFIG_TRACE=$(MT7601U_CONFIG_TRACE) -DMT7601U_DATA_TRACE=$(MT7601U_DATA_TRACE) -DMT7601U_TX_EDCA_EP4=$(MT7601U_TX_EDCA_EP4) -DMT7601U_WCID_CLEAR=$(MT7601U_WCID_CLEAR) -DMT7601U_SCAN_CHAN=$(MT7601U_SCAN_CHAN) $(PLAT_FLAGS) -I. -Iinclude -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
CORE = string kalloc spinlock sleeplock proc exec bio fs log pipe pty file sysfile sysproc device input console tty workqueue net epoll sysnet $(if $(filter 1,$(QEMU_TEST)),,fat32 vfs ext2)
SHARED = vm syscall stubs runtime fdt initcall irq time
OWN = setup atag devices
ifeq ($(QEMU_TEST),1)
DRIVERS = irqchip/irq-sun4i clocksource/timer-sun4i tty/serial/8250_sun4i block/ramdisk_embed \
  video/fbdev/fbmem video/fbdev/simplefb
else
DRIVERS = clk/clk-s5pv210 watchdog/s3c2410_wdt irqchip/irq-vic clocksource/samsung_pwm_timer \
  tty/serial/samsung video/fbdev/fbmem video/fbdev/s3c-fb video/fbdev/fbcon \
  mmc/host/sdhci-s3c mmc/host/sdcore input/keyboard/s5pv210-keys tty/vt/keyboard usb/core/usb usb/host/ehci-hcd hid/usbhid/usbkbd usb/dwc2/hcd net/usb/usbnet net/wireless/mt7601u net/wireless/wpa_crypto base/firmware
FIRMWARE = mt7601u
endif
DRIVERS += video/win32k
# LCD panel (devices.c table): s70 (7", 800x480, default), h43, w50, a70.
S5PV210_LCD ?= s70
LCD_INDEX = $(if $(filter s70,$(S5PV210_LCD)),0,$(if $(filter h43,$(S5PV210_LCD)),1,$(if $(filter w50,$(S5PV210_LCD)),2,$(if $(filter a70,$(S5PV210_LCD)),3,$(error S5PV210_LCD must be s70, h43, w50 or a70)))))
OBJS = $(addprefix $(BUILD)/kernel/,$(addsuffix .o,$(CORE))) \
  $(addprefix $(BUILD)/shared/,$(addsuffix .o,$(SHARED)) vectors.o swtch.o div.o) \
  $(addprefix $(BUILD)/port/,$(addsuffix .o,$(OWN)) entry.o) \
  $(addprefix $(BUILD)/drivers/,$(addsuffix .o,$(DRIVERS))) \
  $(BUILD)/port/printf.o $(if $(filter 1,$(QEMU_TEST)),$(BUILD)/ramdisk.o) $(BUILD)/initcode-embed.o \
  $(BUILD)/version.o \
  $(addprefix $(BUILD)/firmware/,$(addsuffix .o,$(FIRMWARE)))
COMMANDS = armcheck sh echo cat ls mkdir rm wc grep kill ln stressfs forktest fbtest evtest wifi dhcp ping tftpclient reboot windemo
UPROGS = $(BUILD)/user/_init $(addprefix $(BUILD)/user/_,$(COMMANDS))
ULIB = $(BUILD)/user/ulib.o $(BUILD)/user/printf.o $(BUILD)/user/umalloc.o $(BUILD)/shared/usys.o $(BUILD)/user-runtime.o $(BUILD)/shared/div.o
IMG = $(BUILD)/xv6-tiny210

.SECONDARY:
.PHONY: all clean qemu check sdimg bl1 uarttest bootfs rootfs tftpboot FORCE
all: $(IMG).bin $(IMG).uImage $(BUILD)/xv6_tiny210.img $(BUILD)/tiny210-bl1.bin $(BUILD)/tiny210-sd.img
ifneq ($(QEMU_TEST),1)
all: bootfs tftpboot
endif
# BL1 boots raw sectors, not the FAT32 partition: BL1 @ sector 1 and the
# uImage @ sector 49.  Write a card with the whole image (MBR preserved)
#   sudo dd if=$(BUILD)/tiny210-sd.img of=/dev/rdiskN bs=512 skip=1 seek=1
# or just the two boot pieces:
#   sudo dd if=$(BUILD)/tiny210-bl1.bin of=/dev/rdiskN bs=512 seek=1
#   sudo dd if=$(IMG).uImage of=/dev/rdiskN bs=512 seek=49
# Copy the files the kernel reads to the SD card's FAT32 partition when it
# is mounted (default /Volumes/bootfs): mt7601u.bin every time; FS.IMG only
# if the card has none yet, because it is the board's writable root
# (BOOTFS_FSIMG=1 overwrites it). cp -X keeps macOS ._ files off the card.
TINY210_BOOTFS ?= /Volumes/bootfs
BOOTFS_FSIMG ?= 0
bootfs: $(BUILD)/fs.img firmware/mt7601u.bin
	@if [ -d "$(TINY210_BOOTFS)" ]; then \
	  cp -X firmware/mt7601u.bin "$(TINY210_BOOTFS)/mt7601u.bin" && \
	  if [ ! -f "$(TINY210_BOOTFS)/FS.IMG" ] || [ "$(BOOTFS_FSIMG)" = 1 ]; then \
	    cp -X $(BUILD)/fs.img "$(TINY210_BOOTFS)/FS.IMG" && echo "bootfs: FS.IMG written"; \
	  else echo "bootfs: FS.IMG kept (the board's root; BOOTFS_FSIMG=1 replaces it)"; fi && \
	  rm -f "$(TINY210_BOOTFS)/._mt7601u.bin" "$(TINY210_BOOTFS)/._FS.IMG" && \
	  sync && echo "bootfs: mt7601u.bin copied to $(TINY210_BOOTFS)"; \
	else echo "bootfs: $(TINY210_BOOTFS) not mounted, nothing copied"; fi
	@if [ -d "$(TINY210_BOOTFS)" ]; then diskutil info "$(TINY210_BOOTFS)" | \
	  awk '/Part of Whole:/ {print $$4}' > $(BUILD)/card-disk; else rm -f $(BUILD)/card-disk; fi
	@$(MAKE) -f $(P)/Makefile --no-print-directory rootfs
	@# Eject the card: the reader may cache writes (a raw partition write once
	@# read back fine on the Mac yet the board saw an empty partition), and
	@# eject flushes it. BOOTFS_EJECT=0 keeps the card mounted.
	@if [ "$(BOOTFS_EJECT)" != 0 ] && [ -f $(BUILD)/card-disk ]; then \
	  disk=$$(cat $(BUILD)/card-disk); sync; \
	  diskutil eject $$disk >/dev/null && echo "bootfs: $$disk ejected, the card can be removed"; fi
# The root file system on the card's second partition (the kernel prefers a
# primary, non-FAT partition with an xv6 superblock over FS.IMG): when the
# card is mounted, find the disk that holds TINY210_BOOTFS and write fs.img
# raw at the LBA its MBR partition entry names -- not through /dev/rdiskN s2.
# The board reads the MBR entry, so writing the whole disk at that same LBA
# removes the macOS slice device from the path.  Always written (4 MiB, about
# a second): a "written" stamp once claimed a write the card did not keep.
# The disk is unmounted first and dd uses conv=fsync; all 4 MiB are compared
# back, because a counterfeit card accepts writes past its real capacity and
# only shows 0xff after the card has been power-cycled.
ROOTFS_SLICE ?= 2
rootfs: $(BUILD)/fs.img
	@if [ ! -d "$(TINY210_BOOTFS)" ]; then echo "rootfs: $(TINY210_BOOTFS) not mounted, root partition not written"; exit 0; fi; \
	disk=$$(diskutil info "$(TINY210_BOOTFS)" | awk '/Part of Whole:/ {print $$4}'); \
	part=$${disk}s$(ROOTFS_SLICE); \
	if [ -z "$$disk" ] || ! diskutil info $$part >/dev/null 2>&1; then \
	  echo "rootfs: $$disk has no partition $(ROOTFS_SLICE); see ports/tiny210/README.md"; exit 0; fi; \
	ptype=$$(diskutil info $$part | awk -F': *' '/Partition Type:/ {print $$2}'); \
	case "$$ptype" in *FAT*|*DOS*|*Windows*|*Apple*|*EFI*|*NTFS*|*HFS*) \
	  echo "rootfs: /dev/$$part is \"$$ptype\", not a raw root partition; not written"; exit 0;; esac; \
	sync; diskutil unmountDisk $$disk >/dev/null 2>&1 || echo "rootfs: cannot unmount $$disk"; \
	start=$$(sudo dd if=/dev/r$$disk bs=512 count=1 | \
	        od -An -tu4 -j $$((446 + ($(ROOTFS_SLICE)-1)*16 + 8)) -N4 | tr -d ' '); \
	if [ -z "$$start" ] || [ "$$start" = 0 ]; then \
	  echo "rootfs: MBR partition $(ROOTFS_SLICE) is empty; not written"; exit 0; fi; \
	slice=$$(diskutil info $$part | awk '/Partition Offset/ {print $$5}' | tr -cd '0-9'); \
	echo "rootfs: MBR p$(ROOTFS_SLICE) lba=$$start, diskutil slice offset=$$slice"; \
	if [ -n "$$slice" ] && [ "$$slice" != "$$start" ]; then \
	  echo "rootfs: WARNING: the Mac slice and the MBR disagree; writing at the MBR lba the board reads"; fi; \
	sz=$$(stat -f%z $(BUILD)/fs.img); secs=$$((sz / 512)); \
	echo "rootfs: writing fs.img at lba=$$start to /dev/r$$disk ($$ptype; sudo)"; \
	sudo dd if=$(BUILD)/fs.img of=/dev/r$$disk bs=512 seek=$$start conv=fsync; \
	dd_rc=$$?; \
	sudo dd if=/dev/r$$disk bs=512 skip=$$start count=$$secs 2>/dev/null | cmp -s - $(BUILD)/fs.img; \
	cmp_rc=$$?; \
	sync; \
	if [ $$dd_rc = 0 ] && [ $$cmp_rc = 0 ]; then \
	  echo "rootfs: done ($$secs sectors read back OK at lba $$start)"; \
	else echo "rootfs: NOT written or read-back mismatch; a card that accepts writes and reads back 0xff after reinsert is counterfeit"; \
	  echo "rootfs: rerun: sudo dd if=$(BUILD)/fs.img of=/dev/r$$disk bs=512 seek=$$start conv=fsync"; fi
# The board's tftpclient fetches files from here into its FAT32 /boot. The
# kernel copy is for convenience only: BL1 boots the raw uImage at sector 49,
# so a kernel fetched at runtime does not change what the next boot runs.
# User programs go to $(TFTP_DIR)/tiny210/<name> for `tftpclient NAME /bin`.
# The directory is root's: copy directly when writable, else through sudo if
# it needs no password, else say what to run once.
tftpboot: $(BUILD)/xv6_tiny210.img $(UPROGS)
	@if [ ! -d "$(TFTP_DIR)" ]; then echo "tftpboot: $(TFTP_DIR) does not exist, nothing copied"; \
	elif cp -X $(BUILD)/xv6_tiny210.img "$(TFTP_DIR)/" 2>/dev/null || \
	     sudo -n cp -X $(BUILD)/xv6_tiny210.img "$(TFTP_DIR)/" 2>/dev/null; then \
	  mkdir -p "$(TFTP_DIR)/tiny210" && \
	  for f in $(UPROGS); do n=$$(basename $$f); cp -X $$f "$(TFTP_DIR)/tiny210/$${n#_}"; done && \
	  echo "tftpboot: xv6_tiny210.img and user programs (tiny210/) copied to $(TFTP_DIR)"; \
	else echo "tftpboot: cannot write $(TFTP_DIR); run once: sudo chown $$(id -un) $(TFTP_DIR)"; fi
# Command-line trace values are not source timestamps.  Persist them so that
# changing (for example) USB_ENUM_TRACE=0 to =1 rebuilds the three affected
# objects without requiring `make clean` or `make -B`.
TRACE_CONFIG = $(BUILD)/.usb-wifi-trace
$(TRACE_CONFIG): FORCE
	@mkdir -p $(@D)
	@printf '%s\n' \
	  'USB_WIFI_TRACE=$(USB_WIFI_TRACE)' \
	  'USB_CORE_TRACE=$(USB_CORE_TRACE)' \
	  'USB_ENUM_TRACE=$(USB_ENUM_TRACE)' \
	  'USB_XFER_TRACE=$(USB_XFER_TRACE)' \
	  'MT7601U_CONFIG_TRACE=$(MT7601U_CONFIG_TRACE)' \
	  'MT7601U_DATA_TRACE=$(MT7601U_DATA_TRACE)' \
	  'MT7601U_TX_EDCA_EP4=$(MT7601U_TX_EDCA_EP4)' \
	  'MT7601U_WCID_CLEAR=$(MT7601U_WCID_CLEAR)' \
	  'MT7601U_SCAN_CHAN=$(MT7601U_SCAN_CHAN)' > $@.new
	@cmp -s $@.new $@ && rm -f $@.new || mv $@.new $@
$(BUILD)/drivers/usb/core/usb.o \
$(BUILD)/drivers/usb/host/ehci-hcd.o \
$(BUILD)/drivers/net/wireless/mt7601u.o: $(TRACE_CONFIG)
$(BUILD)/kernel/%.o: kernel/%.c
	@mkdir -p $(@D)
	$(ARM_CC) $(FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/shared/%.o: $(S)/%.c
	@mkdir -p $(@D)
	$(ARM_CC) $(FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/shared/%.o: $(S)/%.S
	@mkdir -p $(@D)
	$(ARM_CC) $(FLAGS) -c $< -o $@
$(BUILD)/port/%.o: $(MACH)/%.c
	@mkdir -p $(@D)
	$(ARM_CC) $(FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/port/%.o: $(MACH)/%.S
	@mkdir -p $(@D)
	$(ARM_CC) $(FLAGS) -c $< -o $@
$(BUILD)/drivers/%.o: kernel/drivers/%.c
	@mkdir -p $(@D)
	$(ARM_CC) $(FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/port/printf.o: kernel/printf.c
	@mkdir -p $(@D)
	$(ARM_CC) $(FLAGS) -c $< -o $@
$(BUILD)/user/%.o: user/%.c
	@mkdir -p $(@D)
	$(ARM_CC) $(FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/user-runtime.o: $(S)/runtime.c
	@mkdir -p $(@D)
	$(ARM_CC) $(FLAGS) -DXV6_USER_RUNTIME -c $< -o $@
$(BUILD)/user/armcheck.o: $(S)/armcheck.c
	@mkdir -p $(@D)
	$(ARM_CC) $(FLAGS) -c $< -o $@
$(BUILD)/user/init.o: $(S)/init.c
	@mkdir -p $(@D)
	$(ARM_CC) $(FLAGS) -c $< -o $@
$(BUILD)/user/_%: $(BUILD)/user/%.o $(ULIB)
	$(ARM_LD) -m armelf --gc-sections -z max-page-size=4096 -N -e main -Ttext 0 -o $@ $^
$(BUILD)/user/_windemo: $(BUILD)/user/windemo.o $(BUILD)/user/win32.o $(BUILD)/user/thread.o $(ULIB)
	$(ARM_LD) -m armelf --gc-sections -z max-page-size=4096 -N -e main -Ttext 0 -o $@ $^
$(BUILD)/mkfs: mkfs/mkfs.c kernel/fs.h kernel/types.h kernel/param.h
	@mkdir -p $(@D)
	$(HOST_CC) -Wall -Werror -I. -DFSSIZE_T113=$(TINY210_FSBLOCKS) $< -o $@
# Optional Wi-Fi credentials, kept out of git (see .gitignore): a file with
# "ssid=NAME" and "psk=PASSPHRASE" lines becomes /etc/wifi.conf, and init
# then joins that network and runs DHCP on wlan0 at boot.
WIFI_CONF ?= ports/tiny210/wifi.conf
# make tiny210 also copies user programs and xv6_tiny210.img to macOS's
# built-in TFTP server root (TFTP_DIR, port 69), and /etc/tftp.conf tells the
# board's tftpclient this Mac's Wi-Fi/Ethernet address (TFTP_SERVER=a.b.c.d).
TFTP_DIR ?= /private/tftpboot
TFTP_SERVER ?= $(shell for i in en0 en1 en2 en3; do ipconfig getifaddr $$i 2>/dev/null && break; done)
TFTP_CONF = $(if $(TFTP_SERVER),$(BUILD)/etc/tftp.conf)
ETC_FILES = $(if $(filter 1,$(QEMU_TEST)),,$(wildcard $(WIFI_CONF)) $(TFTP_CONF))
$(BUILD)/etc/tftp.conf: FORCE
	@mkdir -p $(@D)
	@printf 'server=%s\n' '$(TFTP_SERVER)' > $@.new
	@cmp -s $@.new $@ && rm -f $@.new || mv $@.new $@
FORCE:
$(BUILD)/fs.img: $(BUILD)/mkfs $(UPROGS) $(ETC_FILES)
	$(BUILD)/mkfs $@ $(UPROGS) $(if $(ETC_FILES),--etc $(ETC_FILES))
$(BUILD)/ramdisk.S: $(BUILD)/fs.img
	@printf '.section .ramdisk,"aw"\n.global ramdisk_start,ramdisk_end\nramdisk_start:\n.incbin "$(BUILD)/fs.img"\nramdisk_end:\n.section .note.GNU-stack,"",%%progbits\n' > $@
# Firmware blobs for request_firmware() (kernel/drivers/base/firmware.c).
$(BUILD)/firmware/%.S: firmware/%.bin
	@mkdir -p $(@D)
	@printf '.section .rodata\n.balign 4\n.global fw_$*_start,fw_$*_end\nfw_$*_start:\n.incbin "firmware/$*.bin"\nfw_$*_end:\n.section .note.GNU-stack,"",%%progbits\n' > $@
$(BUILD)/firmware/%.o: $(BUILD)/firmware/%.S
	$(ARM_CC) $(FLAGS) -c $< -o $@
$(BUILD)/ramdisk.o: $(BUILD)/ramdisk.S
	$(ARM_CC) $(FLAGS) -c $< -o $@
$(BUILD)/initcode.o: $(S)/initcode.S
	@mkdir -p $(@D)
	$(ARM_CC) $(FLAGS) -c $< -o $@
$(BUILD)/initcode.elf: $(BUILD)/initcode.o
	$(ARM_LD) -m armelf -N -e start -Ttext 0 -o $@ $<
$(BUILD)/initcode.bin: $(BUILD)/initcode.elf
	$(ARM_OBJCOPY) -O binary $< $@
$(BUILD)/initcode-embed.S: $(BUILD)/initcode.bin
	@printf '.section .rodata\n.global initcode,initcode_end\ninitcode:\n.incbin "$(BUILD)/initcode.bin"\ninitcode_end:\n.section .note.GNU-stack,"",%%progbits\n' > $@
$(BUILD)/initcode-embed.o: $(BUILD)/initcode-embed.S
	$(ARM_CC) $(FLAGS) -c $< -o $@
# Build stamp printed at boot ("xv6: built ..."): regenerated whenever any
# other kernel object (or the linker script) changed, so it is the time the
# kernel was last rebuilt, with the git commit it came from.
$(BUILD)/version.c: $(filter-out $(BUILD)/version.o,$(OBJS)) $(MACH)/kernel.ld
	@mkdir -p $(@D)
	@printf 'const char xv6_build_info[] = "%s (git %s)";\n' \
	  "$$(date '+%Y-%m-%d %H:%M:%S')" "$$(git describe --always --dirty 2>/dev/null || echo unknown)" > $@
$(BUILD)/version.o: $(BUILD)/version.c
	$(ARM_CC) $(FLAGS) -c $< -o $@
$(BUILD)/kernel.elf: $(OBJS) $(MACH)/kernel.ld
	$(ARM_LD) -m armelf --gc-sections -z max-page-size=4096 --defsym=KERNEL_LOAD=$(KERNEL_LOAD) -T $(MACH)/kernel.ld -Map $(BUILD)/kernel.map -o $@ $(OBJS)
# Raw image with an ARM zImage header (Samsung bootm "Boot with zImage", bootz).
$(IMG).bin: $(BUILD)/kernel.elf $(P)/mktiny210.py
	$(ARM_OBJCOPY) -O binary $< $@
	python3 $(P)/mktiny210.py check-zimage $@ $(KERNEL_LOAD)
# Legacy uImage (IH_OS_LINUX, load/entry DRAM+0x8000), written to sector 49
# for BL1, or handed to U-Boot's `bootm`.
$(IMG).uImage: $(IMG).bin $(P)/mktiny210.py
	python3 $(P)/mktiny210.py uimage $< $@ $(KERNEL_LOAD)
# The uImage under the name tftpclient downloads into the board's FAT32 /boot.
# BL1 does not read it: it boots the raw uImage at sector 49.
$(BUILD)/xv6_tiny210.img: $(IMG).uImage
	cp $< $@
# Our own BL1 (no U-Boot): runs in iRAM, inits clocks/UART/DDR, copies the
# uImage from SD sector 49 to DRAM+0x7FC0 and verifies it.
BL1FLAGS = $(BOARD_DEF) $(TARGET_FLAGS) -mcpu=cortex-a8 -marm -mno-unaligned-access -mfloat-abi=soft -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -Os -g -ffunction-sections -fdata-sections -I. -I$(MACH) -I$(MACH)/bl1 -Wall -Wextra
BL1OBJS = $(BUILD)/bl1/start.o $(BUILD)/bl1/bl1.o $(BUILD)/bl1/bl1lib.o
$(BUILD)/bl1/%.o: $(MACH)/bl1/%.c $(MACH)/bl1/bl1lib.h $(MACH)/board.h
	@mkdir -p $(@D)
	$(ARM_CC) $(BL1FLAGS) -c $< -o $@
$(BUILD)/bl1/%.o: $(MACH)/bl1/%.S
	@mkdir -p $(@D)
	$(ARM_CC) $(BL1FLAGS) -c $< -o $@
$(BUILD)/bl1/bl1.elf: $(BL1OBJS) $(MACH)/bl1/bl1.ld
	$(ARM_LD) -m armelf --gc-sections -T $(MACH)/bl1/bl1.ld -Map $(BUILD)/bl1/bl1.map -o $@ $(BL1OBJS)
$(BUILD)/bl1/bl1-code.bin: $(BUILD)/bl1/bl1.elf
	$(ARM_OBJCOPY) -O binary $< $@
# 16 KiB BL1 with the iROM header (size, checksum): SD sector 1.
$(BUILD)/tiny210-bl1.bin: $(BUILD)/bl1/bl1-code.bin $(P)/mktiny210.py
	python3 $(P)/mktiny210.py bl1 $< $@
# Whole card: BL1 @ sector 1, xv6 uImage @ sector 49.
$(BUILD)/tiny210-sd.img: $(BUILD)/bl1/bl1-code.bin $(IMG).uImage $(P)/mktiny210.py
	python3 $(P)/mktiny210.py sdimg $@ $(BUILD)/bl1/bl1-code.bin $(IMG).uImage
# Serial check BL1 (no kernel): prints a numbered line forever.
UTOBJS = $(BUILD)/bl1/start.o $(BUILD)/bl1/bl1-uarttest.o $(BUILD)/bl1/bl1lib.o
$(BUILD)/bl1/bl1-uarttest.o: $(MACH)/bl1/bl1.c $(MACH)/bl1/bl1lib.h $(MACH)/board.h
	@mkdir -p $(@D)
	$(ARM_CC) $(BL1FLAGS) -DBL1_UART_TEST -Wno-unused-function -c $< -o $@
$(BUILD)/bl1/uarttest.elf: $(UTOBJS) $(MACH)/bl1/bl1.ld
	$(ARM_LD) -m armelf --gc-sections -T $(MACH)/bl1/bl1.ld -o $@ $(UTOBJS)
$(BUILD)/tiny210-uarttest.bin: $(BUILD)/bl1/uarttest.elf $(P)/mktiny210.py
	$(ARM_OBJCOPY) -O binary $< $(BUILD)/bl1/uarttest-code.bin
	python3 $(P)/mktiny210.py bl1 $(BUILD)/bl1/uarttest-code.bin $@
uarttest: $(BUILD)/tiny210-uarttest.bin
bl1: $(BUILD)/tiny210-bl1.bin
# Build the whole-card image and, when the card is mounted, write it: BL1 to
# sector 1 and the uImage to sector 49, leaving sector 0 (MBR) alone.  The
# write is read back (both sectors) before the card is ejected; SD_WRITE=0
# only builds the image.
SD_WRITE ?= 1
sdimg: $(BUILD)/tiny210-sd.img
	@if [ "$(SD_WRITE)" = 0 ]; then \
	  echo "sdimg: $(BUILD)/tiny210-sd.img built; SD_WRITE=0, card not written"; exit 0; fi; \
	if [ ! -d "$(TINY210_BOOTFS)" ]; then \
	  echo "sdimg: $(TINY210_BOOTFS) not mounted; image built, card not written"; \
	  echo "sdimg: sudo dd if=$(BUILD)/tiny210-sd.img of=/dev/rdiskN bs=512 skip=1 seek=1"; exit 0; fi; \
	disk=$$(diskutil info "$(TINY210_BOOTFS)" | awk '/Part of Whole:/ {print $$4}'); \
	if [ -z "$$disk" ]; then echo "sdimg: cannot find the card disk; not written"; exit 0; fi; \
	dd if=$(BUILD)/tiny210-sd.img bs=512 skip=1 count=1 of=$(BUILD)/bl1-s0.bin 2>/dev/null; \
	dd if=$(BUILD)/tiny210-sd.img bs=512 skip=49 count=1 of=$(BUILD)/uimage-s0.bin 2>/dev/null; \
	echo "sdimg: writing BL1 @ sector 1 and uImage @ sector 49 to /dev/r$$disk (sudo)"; \
	diskutil unmountDisk $$disk >/dev/null 2>&1 || echo "sdimg: cannot unmount $$disk"; \
	if sudo dd if=$(BUILD)/tiny210-sd.img of=/dev/r$$disk bs=512 skip=1 seek=1 conv=fsync && sync && \
	   sudo dd if=/dev/r$$disk bs=512 skip=1 count=1 2>/dev/null | cmp -s - $(BUILD)/bl1-s0.bin && \
	   sudo dd if=/dev/r$$disk bs=512 skip=49 count=1 2>/dev/null | cmp -s - $(BUILD)/uimage-s0.bin; then \
	  sync; \
	  if [ "$(BOOTFS_EJECT)" != 0 ] && diskutil eject $$disk >/dev/null 2>&1; then \
	    echo "sdimg: read back OK; $$disk ejected"; \
	  else echo "sdimg: read back OK; $$disk not ejected"; fi; \
	else echo "sdimg: write or read-back failed; rerun: sudo dd if=$(BUILD)/tiny210-sd.img of=/dev/r$$disk bs=512 skip=1 seek=1"; fi
# Host build of the BL1 logic, checked against the real uImage.
$(BUILD)/bl1-hosttest: $(MACH)/bl1/test_host.c $(MACH)/bl1/bl1lib.c $(MACH)/atag.c $(MACH)/bl1/bl1lib.h $(MACH)/atag.h
	@mkdir -p $(@D)
	$(HOST_CC) -Wall -I. -I$(MACH) -I$(MACH)/bl1 -o $@ $(MACH)/bl1/test_host.c $(MACH)/bl1/bl1lib.c $(MACH)/atag.c
qemu: all
	$(QEMU_ARM) -M cubieboard -m 512M -nographic -monitor none -kernel $(IMG).uImage -append "console=ttySAC0,115200 xv6test"
check: $(BUILD)/bl1-hosttest
	python3 $(P)/mktiny210.py selftest
	$(MAKE) -f $(P)/Makefile BUILD=build/tiny210 build/tiny210/xv6-tiny210.uImage build/tiny210/tiny210-sd.img
	$(BUILD)/bl1-hosttest build/tiny210/xv6-tiny210.uImage
	python3 $(P)/test_shell.py
clean:
	rm -rf $(BUILD)
-include $(wildcard $(BUILD)/kernel/*.d $(BUILD)/port/*.d $(BUILD)/shared/*.d $(BUILD)/user/*.d $(BUILD)/drivers/*/*.d $(BUILD)/drivers/*/*/*.d)
