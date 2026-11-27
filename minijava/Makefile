# MiniJava on xv6-aarch64: Java source -> standard .class -> mjvm.
#
#   make -C minijava               build mjavac, classes and both JVM binaries
#   make -C minijava host-test     run the classes on HotSpot and host mjvm
#   make -C minijava javap C=Counts
#   make -C minijava install       install /bin/mjvm and /root/classes
#   make -C minijava qemu          make a fresh ext2 image, install, boot QEMU
#
# The original direct-to-AArch64 compiler is intentionally kept in aot/; it
# is no longer part of the default build.

TOP := ..
BUILD := build
CLASSDIR := $(BUILD)/classes
JCLASSDIR := $(BUILD)/jclasses

HOSTCC ?= cc
HOST_CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter \
	-Wno-missing-field-initializers
JAVAC ?= javac
JAVA ?= java
JAVAP ?= javap

TOOLPREFIX ?= $(shell if command -v aarch64-elf-gcc >/dev/null 2>&1; then echo aarch64-elf-; \
	elif command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then echo aarch64-linux-gnu-; \
	else echo aarch64-none-elf-; fi)
CC := $(TOOLPREFIX)gcc
LD := $(TOOLPREFIX)ld
OBJDUMP := $(TOOLPREFIX)objdump

XV6_CFLAGS := -Wall -Werror -Os -g -fno-omit-frame-pointer \
	-mcpu=cortex-a53+nofp -Wno-error=infinite-recursion \
	-Wno-error=unused-but-set-variable -Wno-error=incompatible-pointer-types \
	-MD -ffreestanding -fno-common -nostdlib -fno-stack-protector \
	-fno-pie -no-pie -ffixed-x28 -fno-strict-aliasing -mno-outline-atomics \
	-I$(TOP) -I$(TOP)/include -I$(TOP)/kernel -I$(TOP)/user/include
XV6_LDFLAGS := -z max-page-size=4096 -pie \
	-dynamic-linker /lib/ld-xv6.so -e main

JAVA_SRCS := $(sort $(wildcard examples/*.java))
MAIN_CLASSES := $(basename $(notdir $(JAVA_SRCS)))
XV6_LIBC := $(TOP)/user/libc.so
XV6_LDSO := $(TOP)/user/ld-xv6.so
XV6_JVM := $(BUILD)/_mjvm

.PHONY: all
all: $(BUILD)/mjavac $(CLASSDIR)/.stamp $(BUILD)/mjvm-host $(XV6_JVM)

$(BUILD):
	mkdir -p $@

$(BUILD)/mjavac: javac/mjavac.c | $(BUILD)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ $<

# mjavac can emit several classes for one source, so compile the complete
# source set as one action and use a stamp rather than pretending that each
# Source.java produces only Source.class.
$(CLASSDIR)/.stamp: $(JAVA_SRCS) $(BUILD)/mjavac
	mkdir -p $(CLASSDIR)
	$(BUILD)/mjavac -d $(CLASSDIR) $(JAVA_SRCS)
	touch $@

$(BUILD)/mjvm-host: jvm/mjvm.c jvm/plat_host.c jvm/plat.h | $(BUILD)
	$(HOSTCC) $(HOST_CFLAGS) -pthread -o $@ jvm/mjvm.c jvm/plat_host.c

$(BUILD)/mjvm.o: jvm/mjvm.c jvm/plat.h | $(BUILD)
	$(CC) $(XV6_CFLAGS) -c -o $@ $<

$(BUILD)/plat_xv6.o: jvm/plat_xv6.c jvm/plat.h | $(BUILD)
	$(CC) $(XV6_CFLAGS) -c -o $@ $<

$(XV6_LIBC) $(XV6_LDSO):
	$(MAKE) -C $(TOP) user/libc.so user/ld-xv6.so

$(XV6_JVM): $(BUILD)/mjvm.o $(BUILD)/plat_xv6.o $(XV6_LIBC) $(XV6_LDSO)
	$(LD) $(XV6_LDFLAGS) -o $@ $(BUILD)/mjvm.o $(BUILD)/plat_xv6.o $(XV6_LIBC)
	$(OBJDUMP) -S $@ > $(BUILD)/mjvm.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $(BUILD)/mjvm.sym

.PHONY: javac-classes
javac-classes: $(JCLASSDIR)/.stamp

$(JCLASSDIR)/.stamp: $(JAVA_SRCS)
	@command -v $(JAVAC) >/dev/null 2>&1 || { echo "$(JAVAC): Java compiler not found"; exit 1; }
	mkdir -p $(JCLASSDIR)
	$(JAVAC) --release 8 -d $(JCLASSDIR) $(JAVA_SRCS)
	touch $@

.PHONY: host-test
host-test: all
	@set -e; \
	if command -v $(JAVA) >/dev/null 2>&1; then \
	  for c in $(MAIN_CLASSES); do \
	    echo "== HotSpot: $$c =="; \
	    $(JAVA) -Xverify:all -cp $(CLASSDIR) $$c; \
	  done; \
	else \
	  echo "$(JAVA) not found; skipping HotSpot runs"; \
	fi; \
	for c in $(MAIN_CLASSES); do \
	  echo "== mjvm(host): $$c =="; \
	  $(BUILD)/mjvm-host -cp $(CLASSDIR) $$c; \
	done

.PHONY: javap
javap: $(CLASSDIR)/.stamp
	@test -n "$(C)" || { echo "usage: make javap C=ClassName"; exit 2; }
	$(JAVAP) -c -v -p -classpath $(CLASSDIR) $(C)

EXT2_IMAGE ?= $(TOP)/fs_ext2.img
DEBUGFS ?= $(shell if command -v debugfs >/dev/null 2>&1; then command -v debugfs; \
	elif test -x /opt/homebrew/opt/e2fsprogs/sbin/debugfs; then echo /opt/homebrew/opt/e2fsprogs/sbin/debugfs; \
	elif test -x /usr/local/opt/e2fsprogs/sbin/debugfs; then echo /usr/local/opt/e2fsprogs/sbin/debugfs; \
	else echo debugfs; fi)

.PHONY: install
install: all
	@test -f $(EXT2_IMAGE) || { \
	  echo "$(EXT2_IMAGE) is missing; run 'make -C $(TOP) ext2root-image' first"; \
	  exit 1; \
	}
	@{ \
	  echo "rm bin/mjvm"; \
	  echo "write $(XV6_JVM) bin/mjvm"; \
	  echo "mkdir root/classes"; \
	  find $(CLASSDIR) -name '*.class' -type f -print | sort | while read f; do \
	    n=$${f#$(CLASSDIR)/}; \
	    echo "rm root/classes/$$n"; \
	    echo "write $$f root/classes/$$n"; \
	  done; \
	} > $(BUILD)/install.debugfs
	$(DEBUGFS) -w -f $(BUILD)/install.debugfs $(EXT2_IMAGE) > $(BUILD)/install.log 2>&1
	@echo "installed $(XV6_JVM) -> $(EXT2_IMAGE):/bin/mjvm"
	@echo "installed mjavac classes -> $(EXT2_IMAGE):/root/classes"

KERNEL_IMAGE ?= kernel/kernel8-xv6_wifi.img
QEMU_SD_IMAGE ?= build/qemu-rpi3-sd.img

.PHONY: sd
sd: all
	$(MAKE) -C $(TOP) ext2root-image $(KERNEL_IMAGE)
	$(MAKE) install
	cd $(TOP) && python3 tools/mkqemu_sd.py --output $(QEMU_SD_IMAGE) \
	  --xv6 fs.img --ext2 fs_ext2.img \
	  --boot-file config.txt=CONFIG.TXT \
	  --boot-file cmdline_ext2.txt=CMDLINE.TXT \
	  --boot-file $(KERNEL_IMAGE)=$(notdir $(KERNEL_IMAGE))

.PHONY: qemu
qemu: sd
	cd $(TOP) && qemu-system-aarch64 -machine raspi3b -kernel $(KERNEL_IMAGE) \
	  -display none -chardev stdio,id=xv6uart,signal=off \
	  -serial null -serial chardev:xv6uart -monitor none \
	  -drive file=$(QEMU_SD_IMAGE),if=sd,format=raw \
	  -dtb firmware/bcm2710-rpi-3-b.dtb

.PHONY: clean
clean:
	rm -rf $(BUILD)
