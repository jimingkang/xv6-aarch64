ifneq ($(filter x210 x210-qemu x210-check x210-clean x210-bl1 x210-sdimg,$(MAKECMDGOALS)),)
# S5PV210 (FriendlyARM Tiny210 default, 九鼎 X210): see ports/x210/README.md
.PHONY: x210 x210-qemu x210-check x210-clean x210-bl1 x210-sdimg clean
ifneq ($(filter clean,$(MAKECMDGOALS)),)
x210 x210-qemu x210-check x210-bl1 x210-sdimg: clean
endif
clean:
	$(MAKE) -f ports/x210/Makefile BUILD=build/x210 clean
	$(MAKE) -f ports/x210/Makefile BUILD=build/x210-qemu clean
x210-clean: clean
x210:
	$(MAKE) -f ports/x210/Makefile
x210-qemu:
	$(MAKE) -f ports/x210/Makefile BUILD=build/x210-qemu QEMU_TEST=1 qemu
x210-check:
	$(MAKE) -f ports/x210/Makefile check
x210-bl1:
	$(MAKE) -f ports/x210/Makefile bl1
x210-sdimg:
	$(MAKE) -f ports/x210/Makefile sdimg
else ifneq ($(filter am335x am335x-qemu am335x-check am335x-clean,$(MAKECMDGOALS)),)
# BeagleBone Black (TI AM335x): see ports/am335x/README.md
.PHONY: am335x am335x-qemu am335x-check am335x-clean clean
ifneq ($(filter clean,$(MAKECMDGOALS)),)
am335x am335x-qemu am335x-check: clean
endif
clean:
	$(MAKE) -f ports/am335x/Makefile BUILD=build/am335x clean
	$(MAKE) -f ports/am335x/Makefile BUILD=build/am335x-qemu clean
am335x-clean: clean
am335x:
	$(MAKE) -f ports/am335x/Makefile
am335x-qemu:
	$(MAKE) -f ports/am335x/Makefile BUILD=build/am335x-qemu QEMU_TEST=1 qemu
am335x-check:
	$(MAKE) -f ports/am335x/Makefile check
else ifneq ($(filter t113 t113-qemu t113-check t113-clean,$(MAKECMDGOALS)),)
.PHONY: t113 t113-qemu t113-check t113-clean clean
# Order cleanup before building even for `make -j t113 clean`.
ifneq ($(filter clean,$(MAKECMDGOALS)),)
t113 t113-qemu t113-check: clean
endif
clean:
	$(MAKE) -f ports/t113/Makefile BUILD=build/t113 clean
	$(MAKE) -f ports/t113/Makefile BUILD=build/t113-qemu clean
t113-clean: clean
t113:
	$(MAKE) -f ports/t113/Makefile
t113-qemu:
	$(MAKE) -f ports/t113/Makefile BUILD=build/t113-qemu QEMU_TEST=1 qemu
t113-check:
	$(MAKE) -f ports/t113/Makefile check
else
# Default: FriendlyARM Tiny210 (S5PV210), see ports/x210/README.md.
# Other targets: x210-check, x210-qemu, x210-clean, t113*, am335x*.
.PHONY: all clean check
.DEFAULT_GOAL := all
all:
	$(MAKE) -f ports/x210/Makefile
check:
	$(MAKE) -f ports/x210/Makefile check
clean:
	$(MAKE) -f ports/x210/Makefile BUILD=build/x210 clean
	$(MAKE) -f ports/x210/Makefile BUILD=build/x210-qemu clean
endif
