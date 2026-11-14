# Invoke from repository root: make -f ports/tiny210/Makefile
# S5PV210 (Cortex-A8): FriendlyARM Tiny210 (default) / 九鼎 X210.
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
# S5PV210 board: tiny210 (FriendlyARM Tiny210, default) or x210 (九鼎 X210).
# See board.h: DRAM base/size, console UART, machine id.
S5PV210_BOARD ?= tiny210
ifeq ($(S5PV210_BOARD),tiny210)
BOARD_DEF = -DBOARD_TINY210
TINY210_DRAM_BASE = 0x20000000
BOARD_UART = 0xE2900000
else ifeq ($(S5PV210_BOARD),x210)
BOARD_DEF = -DBOARD_X210
TINY210_DRAM_BASE = 0x30000000
BOARD_UART = 0xE2900800
else
$(error S5PV210_BOARD must be tiny210 or x210)
endif
# Root file system blocks (1 KiB).
TINY210_FSBLOCKS ?= 4096
ifeq ($(QEMU_TEST),1)
KERNEL_LOAD = 0x40008000
EARLY_UART = 0x01C28000
PLAT_FLAGS = -DTINY210_QEMU
else
KERNEL_LOAD = $(shell printf '0x%08x' $$(( $(TINY210_DRAM_BASE) + 0x8000 )))
EARLY_UART = $(BOARD_UART)
PLAT_FLAGS = $(BOARD_DEF) -DS5PV210_LCD_INDEX=$(LCD_INDEX) $(if $(filter 1,$(S5PV210_USB_OTG)),-DS5PV210_USB_OTG)
endif
FLAGS = $(TARGET_FLAGS) -mcpu=cortex-a8 -marm -mfloat-abi=soft -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -Os -g -fno-omit-frame-pointer -ffunction-sections -fdata-sections -DXV6_ARM32 -DXV6_TINY210 -DXV6_HAVE_NET $(if $(filter 1,$(QEMU_TEST)),,-DXV6_HAVE_VFS) -DFSSIZE_T113=$(TINY210_FSBLOCKS) -DEARLY_UART=$(EARLY_UART) $(PLAT_FLAGS) -I. -Iinclude -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
CORE = string kalloc spinlock sleeplock proc exec bio fs log pipe file sysfile sysproc device console tty workqueue net epoll sysnet $(if $(filter 1,$(QEMU_TEST)),,fat32 vfs ext2)
SHARED = vm syscall stubs runtime fdt initcall irq time
OWN = setup atag devices
ifeq ($(QEMU_TEST),1)
DRIVERS = irqchip/irq-sun4i clocksource/timer-sun4i tty/serial/8250_sun4i block/ramdisk_embed \
  video/fbdev/fbmem video/fbdev/simplefb
else
DRIVERS = clk/clk-s5pv210 watchdog/s3c2410_wdt irqchip/irq-vic clocksource/samsung_pwm_timer \
  tty/serial/samsung video/fbdev/fbmem video/fbdev/s3c-fb video/fbdev/fbcon block/ramdisk_embed \
  mmc/host/sdhci-s3c mmc/host/sdcore usb/core/usb usb/host/ehci-hcd usb/dwc2/hcd net/usb/usbnet net/wireless/mt7601u net/wireless/wpa_crypto base/firmware
FIRMWARE = mt7601u
endif
# LCD panel (devices.c table): s70 (7", 800x480, default), h43, w50, a70.
S5PV210_LCD ?= s70
LCD_INDEX = $(if $(filter s70,$(S5PV210_LCD)),0,$(if $(filter h43,$(S5PV210_LCD)),1,$(if $(filter w50,$(S5PV210_LCD)),2,$(if $(filter a70,$(S5PV210_LCD)),3,$(error S5PV210_LCD must be s70, h43, w50 or a70)))))
OBJS = $(addprefix $(BUILD)/kernel/,$(addsuffix .o,$(CORE))) \
  $(addprefix $(BUILD)/shared/,$(addsuffix .o,$(SHARED)) vectors.o swtch.o div.o) \
  $(addprefix $(BUILD)/port/,$(addsuffix .o,$(OWN)) entry.o) \
  $(addprefix $(BUILD)/drivers/,$(addsuffix .o,$(DRIVERS))) \
  $(BUILD)/port/printf.o $(BUILD)/ramdisk.o $(BUILD)/initcode-embed.o \
  $(addprefix $(BUILD)/firmware/,$(addsuffix .o,$(FIRMWARE)))
COMMANDS = armcheck sh echo cat ls mkdir rm wc grep kill ln stressfs forktest fbtest wifi dhcp ping
UPROGS = $(BUILD)/user/_init $(addprefix $(BUILD)/user/_,$(COMMANDS))
ULIB = $(BUILD)/user/ulib.o $(BUILD)/user/printf.o $(BUILD)/user/umalloc.o $(BUILD)/shared/usys.o $(BUILD)/user-runtime.o $(BUILD)/shared/div.o
IMG = $(BUILD)/xv6-tiny210

.SECONDARY:
.PHONY: all clean qemu check sdimg bl1 uarttest fattest bootfs
all: $(IMG).bin $(IMG).uImage $(BUILD)/xv6_tiny210.img $(BUILD)/tiny210-bl1.bin $(BUILD)/tiny210-sd.img
ifneq ($(QEMU_TEST),1)
all: bootfs
endif
# Copy the boot files to the SD card's FAT32 partition when it is mounted
# (default /Volumes/bootfs): the kernel and mt7601u.bin every time; FS.IMG
# only if the card has none yet, because it is the board's writable root
# (BOOTFS_FSIMG=1 overwrites it). cp -X keeps macOS ._ files off the card.
# BL1 (sector 1) still needs: sudo dd if=$(BUILD)/tiny210-bl1.bin of=/dev/rdiskN bs=512 seek=1
TINY210_BOOTFS ?= /Volumes/bootfs
BOOTFS_FSIMG ?= 0
bootfs: $(BUILD)/xv6_tiny210.img $(BUILD)/fs.img firmware/mt7601u.bin
	@if [ -d "$(TINY210_BOOTFS)" ]; then \
	  cp -X $(BUILD)/xv6_tiny210.img "$(TINY210_BOOTFS)/xv6_tiny210.img" && \
	  cp -X firmware/mt7601u.bin "$(TINY210_BOOTFS)/mt7601u.bin" && \
	  if [ ! -f "$(TINY210_BOOTFS)/FS.IMG" ] || [ "$(BOOTFS_FSIMG)" = 1 ]; then \
	    cp -X $(BUILD)/fs.img "$(TINY210_BOOTFS)/FS.IMG" && echo "bootfs: FS.IMG written"; \
	  else echo "bootfs: FS.IMG kept (the board's root; BOOTFS_FSIMG=1 replaces it)"; fi && \
	  rm -f "$(TINY210_BOOTFS)/._xv6_tiny210.img" "$(TINY210_BOOTFS)/._mt7601u.bin" "$(TINY210_BOOTFS)/._FS.IMG" && \
	  sync && echo "bootfs: xv6_tiny210.img and mt7601u.bin copied to $(TINY210_BOOTFS)"; \
	else echo "bootfs: $(TINY210_BOOTFS) not mounted, nothing copied"; fi
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
$(BUILD)/mkfs: mkfs/mkfs.c kernel/fs.h kernel/types.h kernel/param.h
	@mkdir -p $(@D)
	$(HOST_CC) -Wall -Werror -I. -DFSSIZE_T113=$(TINY210_FSBLOCKS) $< -o $@
# Optional Wi-Fi credentials, kept out of git (see .gitignore): a file with
# "ssid=NAME" and "psk=PASSPHRASE" lines becomes /etc/wifi.conf, and init
# then joins that network and runs DHCP on wlan0 at boot.
WIFI_CONF ?= ports/tiny210/wifi.conf
ETC_FILES = $(if $(filter 1,$(QEMU_TEST)),,$(wildcard $(WIFI_CONF)))
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
$(BUILD)/kernel.elf: $(OBJS) $(MACH)/kernel.ld
	$(ARM_LD) -m armelf --gc-sections -z max-page-size=4096 --defsym=KERNEL_LOAD=$(KERNEL_LOAD) -T $(MACH)/kernel.ld -Map $(BUILD)/kernel.map -o $@ $(OBJS)
# Raw image with an ARM zImage header (Samsung bootm "Boot with zImage", bootz).
$(IMG).bin: $(BUILD)/kernel.elf $(P)/mktiny210.py
	$(ARM_OBJCOPY) -O binary $< $@
	python3 $(P)/mktiny210.py check-zimage $@ $(KERNEL_LOAD)
# Legacy uImage (IH_OS_LINUX, load/entry DRAM+0x8000), read by bl1 or `bootm`.
$(IMG).uImage: $(IMG).bin $(P)/mktiny210.py
	python3 $(P)/mktiny210.py uimage $< $@ $(KERNEL_LOAD)
# The file BL1 loads from the FAT32 partition (same bytes as the uImage).
$(BUILD)/xv6_tiny210.img: $(IMG).uImage
	cp $< $@
# Our own BL1 (no U-Boot): runs in iRAM, inits clocks/UART/DDR, loads xv6.
BL1FLAGS = $(BOARD_DEF) $(TARGET_FLAGS) -mcpu=cortex-a8 -marm -mfloat-abi=soft -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -Os -g -ffunction-sections -fdata-sections -I. -I$(MACH) -I$(MACH)/bl1 -Wall -Wextra
BL1OBJS = $(BUILD)/bl1/start.o $(BUILD)/bl1/bl1.o $(BUILD)/bl1/bl1lib.o $(BUILD)/bl1/bl1fat.o $(BUILD)/bl1/sdcore.o
$(BUILD)/bl1/%.o: $(MACH)/bl1/%.c $(MACH)/bl1/bl1lib.h $(MACH)/bl1/bl1fat.h $(MACH)/board.h
	@mkdir -p $(@D)
	$(ARM_CC) $(BL1FLAGS) -c $< -o $@
$(BUILD)/bl1/sdcore.o: kernel/drivers/mmc/host/sdcore.c kernel/drivers/mmc/host/sdcore.h
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
UTOBJS = $(BUILD)/bl1/start.o $(BUILD)/bl1/bl1-uarttest.o $(BUILD)/bl1/bl1lib.o $(BUILD)/bl1/bl1fat.o $(BUILD)/bl1/sdcore.o
$(BUILD)/bl1/bl1-uarttest.o: $(MACH)/bl1/bl1.c $(MACH)/bl1/bl1lib.h $(MACH)/board.h
	@mkdir -p $(@D)
	$(ARM_CC) $(BL1FLAGS) -DBL1_UART_TEST -Wno-unused-function -c $< -o $@
$(BUILD)/bl1/uarttest.elf: $(UTOBJS) $(MACH)/bl1/bl1.ld
	$(ARM_LD) -m armelf --gc-sections -T $(MACH)/bl1/bl1.ld -o $@ $(UTOBJS)
$(BUILD)/tiny210-uarttest.bin: $(BUILD)/bl1/uarttest.elf $(P)/mktiny210.py
	$(ARM_OBJCOPY) -O binary $< $(BUILD)/bl1/uarttest-code.bin
	python3 $(P)/mktiny210.py bl1 $(BUILD)/bl1/uarttest-code.bin $@
uarttest: $(BUILD)/tiny210-uarttest.bin
# Host test of BL1's FAT32 reader on a raw card image (macOS example:
#   hdiutil create -size 64m -fs "MS-DOS FAT32" -layout MBRSPUD -o card,
#   copy XV6.IMG/FS.IMG in, hdiutil convert card.dmg -format UDTO -o card).
$(BUILD)/bl1-fattest: $(MACH)/bl1/test_fat_host.c $(MACH)/bl1/bl1fat.c $(MACH)/bl1/bl1fat.h
	@mkdir -p $(@D)
	$(HOST_CC) -Wall -I. -I$(MACH)/bl1 -o $@ $(MACH)/bl1/test_fat_host.c $(MACH)/bl1/bl1fat.c
fattest: $(BUILD)/bl1-fattest
	@test -n "$(CARD)" || { echo "usage: make -f ports/tiny210/Makefile fattest CARD=card.cdr"; exit 1; }
	$(BUILD)/bl1-fattest $(CARD) $(IMG).uImage $(BUILD)/fs.img
bl1: $(BUILD)/tiny210-bl1.bin
sdimg: $(BUILD)/tiny210-sd.img
# Host build of the BL1 logic, checked against the real uImage.
$(BUILD)/bl1-hosttest: $(MACH)/bl1/test_host.c $(MACH)/bl1/bl1lib.c $(MACH)/atag.c $(MACH)/bl1/bl1lib.h $(MACH)/atag.h
	@mkdir -p $(@D)
	$(HOST_CC) -Wall -I. -I$(MACH) -I$(MACH)/bl1 -o $@ $(MACH)/bl1/test_host.c $(MACH)/bl1/bl1lib.c $(MACH)/atag.c
qemu: all
	$(QEMU_ARM) -M cubieboard -m 512M -nographic -monitor none -kernel $(IMG).uImage -append "console=ttySAC0,115200 xv6test"
check: $(BUILD)/bl1-hosttest
	python3 $(P)/mktiny210.py selftest
	$(MAKE) -f $(P)/Makefile BUILD=build/tiny210 S5PV210_BOARD=$(S5PV210_BOARD) build/tiny210/xv6-tiny210.uImage build/tiny210/tiny210-sd.img
	$(BUILD)/bl1-hosttest build/tiny210/xv6-tiny210.uImage
	$(MAKE) -f $(P)/Makefile BUILD=build/tiny210-board-x210 S5PV210_BOARD=x210 build/tiny210-board-x210/xv6-tiny210.uImage build/tiny210-board-x210/tiny210-sd.img
	$(BUILD)/bl1-hosttest build/tiny210-board-x210/xv6-tiny210.uImage
	python3 $(P)/test_shell.py
clean:
	rm -rf $(BUILD)
-include $(wildcard $(BUILD)/kernel/*.d $(BUILD)/port/*.d $(BUILD)/shared/*.d $(BUILD)/user/*.d $(BUILD)/drivers/*/*.d $(BUILD)/drivers/*/*/*.d)
