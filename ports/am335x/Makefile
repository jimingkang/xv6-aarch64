# Invoke from repository root: make -f ports/am335x/Makefile
# BeagleBone Black (TI AM335x, Cortex-A8). Shared ARM32 code lives in ports/t113.
.DEFAULT_GOAL := all
BUILD ?= build/am335x
P := ports/am335x
S := ports/t113
ARM_CC ?= clang
ARM_LD ?= $(shell command -v arm-none-eabi-ld 2>/dev/null || command -v aarch64-elf-ld 2>/dev/null || echo arm-none-eabi-ld)
ARM_OBJCOPY ?= $(shell command -v arm-none-eabi-objcopy 2>/dev/null || command -v aarch64-elf-objcopy 2>/dev/null || echo arm-none-eabi-objcopy)
HOST_CC ?= cc
DTC ?= dtc
QEMU_ARM ?= qemu-system-arm
TARGET_FLAGS ?= --target=arm-none-eabi
ifeq ($(QEMU_TEST),1)
KERNEL_LOAD = 0x42000000
EARLY_UART = 0x01C28000
PLAT_FLAGS = -DAM335X_QEMU
else
KERNEL_LOAD = 0x82000000
EARLY_UART = 0x44E09000
PLAT_FLAGS =
endif
FLAGS = $(TARGET_FLAGS) -mcpu=cortex-a8 -marm -mfloat-abi=soft -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -Os -g -fno-omit-frame-pointer -ffunction-sections -fdata-sections -DXV6_ARM32 -DXV6_AM335X -DEARLY_UART=$(EARLY_UART) $(PLAT_FLAGS) -I. -Iinclude -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
CORE = string kalloc spinlock sleeplock proc exec bio fs log pipe file sysfile sysproc device console tty
SHARED = vm syscall stubs runtime
OWN = platform fdt
OBJS = $(addprefix $(BUILD)/kernel/,$(addsuffix .o,$(CORE))) \
  $(addprefix $(BUILD)/shared/,$(addsuffix .o,$(SHARED)) vectors.o swtch.o div.o) \
  $(addprefix $(BUILD)/port/,$(addsuffix .o,$(OWN)) entry.o) \
  $(BUILD)/port/printf.o $(BUILD)/ramdisk.o $(BUILD)/initcode-embed.o
COMMANDS = armcheck sh echo cat ls mkdir rm wc grep kill ln stressfs forktest
UPROGS = $(BUILD)/user/_init $(addprefix $(BUILD)/user/_,$(COMMANDS))
ULIB = $(BUILD)/user/ulib.o $(BUILD)/user/printf.o $(BUILD)/user/umalloc.o $(BUILD)/shared/usys.o $(BUILD)/user-runtime.o $(BUILD)/shared/div.o
IMG = $(BUILD)/xv6-am335x

.SECONDARY:
.PHONY: all clean qemu check
all: $(IMG).bin $(IMG).uImage $(BUILD)/uEnv.txt
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
	$(HOST_CC) -Wall -Werror -I. -DFSSIZE_T113=4096 $< -o $@
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
# Raw image with an ARM zImage header: `bootz ${loadaddr} - ${fdtaddr}`.
$(IMG).bin: $(BUILD)/kernel.elf $(P)/mkimage.py
	$(ARM_OBJCOPY) -O binary $< $@
	python3 $(P)/mkimage.py check-zimage $@ $(KERNEL_LOAD)
# Legacy uImage (IH_OS_LINUX): `bootm <staging> - ${fdtaddr}`.
$(IMG).uImage: $(IMG).bin $(P)/mkimage.py
	python3 $(P)/mkimage.py uimage $< $@ $(KERNEL_LOAD)
$(BUILD)/uEnv.txt: $(P)/uEnv.txt
	cp $< $@
$(BUILD)/test.dtb: $(P)/qemu-test.dts
	@mkdir -p $(@D)
	$(DTC) -q -I dts -O dtb -o $@ $<
qemu: all $(BUILD)/test.dtb
	$(QEMU_ARM) -M cubieboard -m 512M -nographic -monitor none -kernel $(IMG).uImage -dtb $(BUILD)/test.dtb
check:
	python3 $(P)/test_shell.py
clean:
	rm -rf $(BUILD)
-include $(wildcard $(BUILD)/kernel/*.d $(BUILD)/port/*.d $(BUILD)/shared/*.d $(BUILD)/user/*.d)
