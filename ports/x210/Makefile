# Invoke from repository root: make -f ports/x210/Makefile
# 九鼎 X210 (Samsung S5PV210, Cortex-A8). Shared ARM32 code lives in
# ports/t113; the DTB reader is shared with ports/am335x.
.DEFAULT_GOAL := all
BUILD ?= build/x210
P := ports/x210
S := ports/t113
A := ports/am335x
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
X210_DRAM_BASE = 0x20000000
BOARD_UART = 0xE2900000
else ifeq ($(S5PV210_BOARD),x210)
BOARD_DEF = -DBOARD_X210
X210_DRAM_BASE = 0x30000000
BOARD_UART = 0xE2900800
else
$(error S5PV210_BOARD must be tiny210 or x210)
endif
# Root file system blocks (1 KiB).
X210_FSBLOCKS ?= 4096
ifeq ($(QEMU_TEST),1)
KERNEL_LOAD = 0x40008000
EARLY_UART = 0x01C28000
PLAT_FLAGS = -DX210_QEMU
else
KERNEL_LOAD = $(shell printf '0x%08x' $$(( $(X210_DRAM_BASE) + 0x8000 )))
EARLY_UART = $(BOARD_UART)
PLAT_FLAGS = $(BOARD_DEF)
endif
FLAGS = $(TARGET_FLAGS) -mcpu=cortex-a8 -marm -mfloat-abi=soft -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -Os -g -fno-omit-frame-pointer -ffunction-sections -fdata-sections -DXV6_ARM32 -DXV6_X210 -DEARLY_UART=$(EARLY_UART) $(PLAT_FLAGS) -I. -Iinclude -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
CORE = string kalloc spinlock sleeplock proc exec bio fs log pipe file sysfile sysproc device console tty
SHARED = vm syscall stubs runtime
OWN = platform atag
OBJS = $(addprefix $(BUILD)/kernel/,$(addsuffix .o,$(CORE))) \
  $(addprefix $(BUILD)/shared/,$(addsuffix .o,$(SHARED)) vectors.o swtch.o div.o) \
  $(addprefix $(BUILD)/port/,$(addsuffix .o,$(OWN)) entry.o) \
  $(BUILD)/port/fdt.o $(BUILD)/port/printf.o $(BUILD)/ramdisk.o $(BUILD)/initcode-embed.o
COMMANDS = armcheck sh echo cat ls mkdir rm wc grep kill ln stressfs forktest
UPROGS = $(BUILD)/user/_init $(addprefix $(BUILD)/user/_,$(COMMANDS))
ULIB = $(BUILD)/user/ulib.o $(BUILD)/user/printf.o $(BUILD)/user/umalloc.o $(BUILD)/shared/usys.o $(BUILD)/user-runtime.o $(BUILD)/shared/div.o
IMG = $(BUILD)/xv6-x210

.SECONDARY:
.PHONY: all clean qemu check sdimg bl1 uarttest
all: $(IMG).bin $(IMG).uImage $(BUILD)/x210-bl1.bin $(BUILD)/x210-sd.img
$(BUILD)/kernel/%.o: kernel/%.c
	@mkdir -p $(@D)
	$(ARM_CC) $(FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/shared/%.o: $(S)/%.c
	@mkdir -p $(@D)
	$(ARM_CC) $(FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/shared/%.o: $(S)/%.S
	@mkdir -p $(@D)
	$(ARM_CC) $(FLAGS) -c $< -o $@
$(BUILD)/port/%.o: $(P)/%.c
	@mkdir -p $(@D)
	$(ARM_CC) $(FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/port/%.o: $(P)/%.S
	@mkdir -p $(@D)
	$(ARM_CC) $(FLAGS) -c $< -o $@
$(BUILD)/port/fdt.o: $(A)/fdt.c
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
	$(HOST_CC) -Wall -Werror -I. -DFSSIZE_T113=$(X210_FSBLOCKS) $< -o $@
$(BUILD)/fs.img: $(BUILD)/mkfs $(UPROGS)
	$(BUILD)/mkfs $@ $(UPROGS)
$(BUILD)/ramdisk.S: $(BUILD)/fs.img
	@printf '.section .ramdisk,"aw"\n.global ramdisk_start,ramdisk_end\nramdisk_start:\n.incbin "$(BUILD)/fs.img"\nramdisk_end:\n.section .note.GNU-stack,"",%%progbits\n' > $@
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
$(BUILD)/kernel.elf: $(OBJS) $(P)/kernel.ld
	$(ARM_LD) -m armelf --gc-sections -z max-page-size=4096 --defsym=KERNEL_LOAD=$(KERNEL_LOAD) -T $(P)/kernel.ld -Map $(BUILD)/kernel.map -o $@ $(OBJS)
# Raw image with an ARM zImage header (Samsung bootm "Boot with zImage", bootz).
$(IMG).bin: $(BUILD)/kernel.elf $(P)/mkx210.py
	$(ARM_OBJCOPY) -O binary $< $@
	python3 $(P)/mkx210.py check-zimage $@ $(KERNEL_LOAD)
# Legacy uImage (IH_OS_LINUX, load/entry DRAM+0x8000), read by bl1 or `bootm`.
$(IMG).uImage: $(IMG).bin $(P)/mkx210.py
	python3 $(P)/mkx210.py uimage $< $@ $(KERNEL_LOAD)
# Our own BL1 (no U-Boot): runs in iRAM, inits clocks/UART/DDR, loads xv6.
BL1FLAGS = $(BOARD_DEF) $(TARGET_FLAGS) -mcpu=cortex-a8 -marm -mfloat-abi=soft -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -Os -g -ffunction-sections -fdata-sections -I. -I$(P) -I$(P)/bl1 -Wall -Wextra
BL1OBJS = $(BUILD)/bl1/start.o $(BUILD)/bl1/bl1.o $(BUILD)/bl1/bl1lib.o
$(BUILD)/bl1/%.o: $(P)/bl1/%.c $(P)/bl1/bl1lib.h $(P)/board.h
	@mkdir -p $(@D)
	$(ARM_CC) $(BL1FLAGS) -c $< -o $@
$(BUILD)/bl1/%.o: $(P)/bl1/%.S
	@mkdir -p $(@D)
	$(ARM_CC) $(BL1FLAGS) -c $< -o $@
$(BUILD)/bl1/bl1.elf: $(BL1OBJS) $(P)/bl1/bl1.ld
	$(ARM_LD) -m armelf --gc-sections -T $(P)/bl1/bl1.ld -Map $(BUILD)/bl1/bl1.map -o $@ $(BL1OBJS)
$(BUILD)/bl1/bl1-code.bin: $(BUILD)/bl1/bl1.elf
	$(ARM_OBJCOPY) -O binary $< $@
# 16 KiB BL1 with the iROM header (size, checksum): SD sector 1.
$(BUILD)/x210-bl1.bin: $(BUILD)/bl1/bl1-code.bin $(P)/mkx210.py
	python3 $(P)/mkx210.py bl1 $< $@
# Whole card: BL1 @ sector 1, xv6 uImage @ sector 49.
$(BUILD)/x210-sd.img: $(BUILD)/bl1/bl1-code.bin $(IMG).uImage $(P)/mkx210.py
	python3 $(P)/mkx210.py sdimg $@ $(BUILD)/bl1/bl1-code.bin $(IMG).uImage
# Serial check BL1 (no kernel): prints a numbered line forever.
UTOBJS = $(BUILD)/bl1/start.o $(BUILD)/bl1/bl1-uarttest.o $(BUILD)/bl1/bl1lib.o
$(BUILD)/bl1/bl1-uarttest.o: $(P)/bl1/bl1.c $(P)/bl1/bl1lib.h $(P)/board.h
	@mkdir -p $(@D)
	$(ARM_CC) $(BL1FLAGS) -DBL1_UART_TEST -Wno-unused-function -c $< -o $@
$(BUILD)/bl1/uarttest.elf: $(UTOBJS) $(P)/bl1/bl1.ld
	$(ARM_LD) -m armelf --gc-sections -T $(P)/bl1/bl1.ld -o $@ $(UTOBJS)
$(BUILD)/x210-uarttest.bin: $(BUILD)/bl1/uarttest.elf $(P)/mkx210.py
	$(ARM_OBJCOPY) -O binary $< $(BUILD)/bl1/uarttest-code.bin
	python3 $(P)/mkx210.py bl1 $(BUILD)/bl1/uarttest-code.bin $@
uarttest: $(BUILD)/x210-uarttest.bin
bl1: $(BUILD)/x210-bl1.bin
sdimg: $(BUILD)/x210-sd.img
# Host build of the BL1 logic, checked against the real uImage.
$(BUILD)/bl1-hosttest: $(P)/bl1/test_host.c $(P)/bl1/bl1lib.c $(P)/atag.c $(P)/bl1/bl1lib.h $(P)/atag.h
	@mkdir -p $(@D)
	$(HOST_CC) -Wall -I. -I$(P)/bl1 -o $@ $(P)/bl1/test_host.c $(P)/bl1/bl1lib.c $(P)/atag.c
qemu: all
	$(QEMU_ARM) -M cubieboard -m 512M -nographic -monitor none -kernel $(IMG).uImage -append "console=ttySAC0,115200 xv6test"
check: $(BUILD)/bl1-hosttest
	python3 $(P)/mkx210.py selftest
	$(MAKE) -f $(P)/Makefile BUILD=build/x210 S5PV210_BOARD=$(S5PV210_BOARD) build/x210/xv6-x210.uImage build/x210/x210-sd.img
	$(BUILD)/bl1-hosttest build/x210/xv6-x210.uImage
	$(MAKE) -f $(P)/Makefile BUILD=build/x210-board-x210 S5PV210_BOARD=x210 build/x210-board-x210/xv6-x210.uImage build/x210-board-x210/x210-sd.img
	$(BUILD)/bl1-hosttest build/x210-board-x210/xv6-x210.uImage
	python3 $(P)/test_shell.py
clean:
	rm -rf $(BUILD)
-include $(wildcard $(BUILD)/kernel/*.d $(BUILD)/port/*.d $(BUILD)/shared/*.d $(BUILD)/user/*.d)
