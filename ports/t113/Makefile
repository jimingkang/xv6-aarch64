# Invoke from repository root: make -f ports/t113/Makefile
.DEFAULT_GOAL := all
BUILD ?= build/t113
ARM_CC ?= clang
ARM_LD ?= $(shell command -v arm-none-eabi-ld 2>/dev/null || command -v aarch64-elf-ld 2>/dev/null || echo arm-none-eabi-ld)
ARM_OBJCOPY ?= $(shell command -v arm-none-eabi-objcopy 2>/dev/null || command -v aarch64-elf-objcopy 2>/dev/null || echo arm-none-eabi-objcopy)
HOST_CC ?= cc
QEMU_ARM ?= qemu-system-arm
TARGET_FLAGS ?= --target=arm-none-eabi
FLAGS = $(TARGET_FLAGS) -mcpu=cortex-a7 -marm -mfloat-abi=soft -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -Os -g -fno-omit-frame-pointer -ffunction-sections -fdata-sections -DXV6_ARM32 -I. -Iinclude -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
ifeq ($(QEMU_TEST),1)
FLAGS += -DT113_QEMU
endif
CORE = string kalloc spinlock sleeplock proc exec bio fs log pipe file sysfile sysproc device console tty
PORT = vm platform syscall stubs runtime
OBJS = $(addprefix $(BUILD)/kernel/,$(addsuffix .o,$(CORE))) $(addprefix $(BUILD)/port/,$(addsuffix .o,$(PORT))) $(addprefix $(BUILD)/port/,entry.o vectors.o swtch.o div.o printf.o) $(BUILD)/ramdisk.o $(BUILD)/initcode-embed.o
COMMANDS = armcheck sh echo cat ls mkdir rm wc grep kill ln stressfs forktest
UPROGS = $(BUILD)/user/_init $(addprefix $(BUILD)/user/_,$(COMMANDS))
ULIB = $(BUILD)/user/ulib.o $(BUILD)/user/printf.o $(BUILD)/user/umalloc.o $(BUILD)/port/usys.o $(BUILD)/user-runtime.o $(BUILD)/port/div.o

.SECONDARY:
.PHONY: all clean qemu check
all: $(BUILD)/xv6-t113.bin $(BUILD)/xv6-t113.uImage
$(BUILD)/kernel/%.o: kernel/%.c
	@mkdir -p $(@D)
	$(ARM_CC) $(FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/port/%.o: ports/t113/%.c
	@mkdir -p $(@D)
	$(ARM_CC) $(FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/port/%.o: ports/t113/%.S
	@mkdir -p $(@D)
	$(ARM_CC) $(FLAGS) -c $< -o $@
$(BUILD)/port/printf.o: kernel/printf.c
	@mkdir -p $(@D)
	$(ARM_CC) $(FLAGS) -c $< -o $@
$(BUILD)/user/%.o: user/%.c
	@mkdir -p $(@D)
	$(ARM_CC) $(FLAGS) -MMD -MP -c $< -o $@
$(BUILD)/user-runtime.o: ports/t113/runtime.c
	@mkdir -p $(@D)
	$(ARM_CC) $(FLAGS) -DXV6_USER_RUNTIME -c $< -o $@
$(BUILD)/user/armcheck.o: ports/t113/armcheck.c
	@mkdir -p $(@D)
	$(ARM_CC) $(FLAGS) -c $< -o $@
$(BUILD)/user/init.o: ports/t113/init.c
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
$(BUILD)/initcode.o: ports/t113/initcode.S
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
$(BUILD)/kernel.elf: $(OBJS) ports/t113/kernel.ld
	$(ARM_LD) -m armelf --gc-sections -z max-page-size=4096 -T ports/t113/kernel.ld -Map $(BUILD)/kernel.map -o $@ $(OBJS)
$(BUILD)/xv6-t113.bin: $(BUILD)/kernel.elf
	$(ARM_OBJCOPY) -O binary $< $@
$(BUILD)/xv6-t113.uImage: $(BUILD)/xv6-t113.bin ports/t113/mkuimage.py
	python3 ports/t113/mkuimage.py $< $@
qemu: all
	$(QEMU_ARM) -M virt -cpu cortex-a7 -m 128M -nographic -monitor none -device loader,file=$(BUILD)/kernel.elf,cpu-num=0
check:
	python3 ports/t113/test_shell.py
clean:
	rm -rf $(BUILD)
-include $(wildcard $(BUILD)/kernel/*.d $(BUILD)/port/*.d $(BUILD)/user/*.d)
