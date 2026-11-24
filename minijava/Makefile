# MiniJava on xv6-aarch64 -- self-contained; does not touch the top-level build.
#
#   make -C minijava                  build mjc (host) and the example programs
#   make -C minijava install          copy _mj* into fs_ext2.img:/bin (and the
#                                     .java sources into /root); run the
#                                     top-level build first
#   make -C minijava qemu             install, rebuild the SD image, boot QEMU
#   make -C minijava asm S=examples/Counter.java    print generated assembly

# same toolchain the top-level build uses (Homebrew aarch64-elf- on macOS)
TOOLPREFIX ?= $(shell if command -v aarch64-elf-gcc >/dev/null 2>&1; then echo aarch64-elf-; \
	elif command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then echo aarch64-linux-gnu-; \
	else echo aarch64-none-elf-; fi)
CC  = $(TOOLPREFIX)gcc
LD  = $(TOOLPREFIX)ld
HOSTCC ?= cc
TOP = ..

CFLAGS = -Wall -Werror -Os -g -fno-omit-frame-pointer -mcpu=cortex-a53+nofp \
         -ffreestanding -fno-common -nostdlib -fno-stack-protector -fno-pie -no-pie \
         -I$(TOP) -I$(TOP)/include -I$(TOP)/kernel -I$(TOP)/user/include \
         -ffixed-x28 -fno-strict-aliasing -mno-outline-atomics
LDFLAGS = -z max-page-size=4096 -pie -dynamic-linker /lib/ld-xv6.so -e main

EXAMPLES = $(wildcard examples/*.java)
PROGS = $(patsubst examples/%.java,build/_mj%,$(EXAMPLES))

all: build/mjc $(PROGS)

build/mjc: mjc.c
	@mkdir -p build
	$(HOSTCC) -O2 -Wall -o $@ $<

build/runtime.o: runtime.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

build/rt_asm.o: rt_asm.S
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

build/%.S: examples/%.java build/mjc
	build/mjc $< > $@.tmp && mv $@.tmp $@

build/%.o: build/%.S
	$(CC) $(CFLAGS) -c -o $@ $<

build/_mj%: build/%.o build/runtime.o build/rt_asm.o $(TOP)/user/libc.so
	$(LD) $(LDFLAGS) -o $@ $< build/runtime.o build/rt_asm.o $(TOP)/user/libc.so

# libc.so comes from the top-level build; build just that if it is missing
# (otherwise make 3.81 only says "No rule to make target build/_mjX").
$(TOP)/user/libc.so:
	$(MAKE) -C $(TOP) user/libc.so user/ld-xv6.so

asm: build/mjc
	build/mjc $(S)

EXT2_IMAGE ?= $(TOP)/fs_ext2.img
DEBUGFS ?= $(shell if command -v debugfs >/dev/null 2>&1; then command -v debugfs; \
	elif test -x /opt/homebrew/opt/e2fsprogs/sbin/debugfs; then echo /opt/homebrew/opt/e2fsprogs/sbin/debugfs; \
	elif test -x /usr/local/opt/e2fsprogs/sbin/debugfs; then echo /usr/local/opt/e2fsprogs/sbin/debugfs; \
	else echo debugfs; fi)
install: all
	@test -f $(EXT2_IMAGE) || { echo "build the top-level images first (make fs.img ext2root-image)"; exit 1; }
	@for p in $(PROGS); do n=$${p#build/_}; echo "rm bin/$$n"; echo "write $$p bin/$$n"; done > build/install.debugfs
	@for e in $(EXAMPLES); do n=$${e#examples/}; echo "rm root/$$n"; echo "write $$e root/$$n"; done >> build/install.debugfs
	$(DEBUGFS) -w -f build/install.debugfs $(EXT2_IMAGE) > build/install.log 2>&1
	@echo "installed $(notdir $(PROGS)) into $(EXT2_IMAGE)"

# Rebuild the QEMU SD image around the patched ext2 root and boot it.
# (The top-level `make qemu` regenerates fs_ext2.img and would drop _mj*.)
KERNEL_IMAGE ?= kernel/kernel8-xv6_wifi.img
QEMU_SD_IMAGE ?= build/qemu-rpi3-sd.img
sd: install
	$(MAKE) -C $(TOP) $(KERNEL_IMAGE)
	cd $(TOP) && python3 tools/mkqemu_sd.py --output $(QEMU_SD_IMAGE) --xv6 fs.img --ext2 fs_ext2.img \
	  --boot-file config.txt=CONFIG.TXT --boot-file cmdline_ext2.txt=CMDLINE.TXT \
	  --boot-file $(KERNEL_IMAGE)=$(notdir $(KERNEL_IMAGE))

qemu: sd
	cd $(TOP) && qemu-system-aarch64 -machine raspi3b -kernel $(KERNEL_IMAGE) -display none \
	  -chardev stdio,id=xv6uart,signal=off -serial null -serial chardev:xv6uart -monitor none \
	  -drive file=$(QEMU_SD_IMAGE),if=sd,format=raw -dtb firmware/bcm2710-rpi-3-b.dtb

.PRECIOUS: build/%.S build/%.o
clean:
	$(RM) -r build
.PHONY: all asm install sd qemu clean
