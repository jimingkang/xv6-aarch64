ifneq ($(filter tiny210 tiny210-qemu tiny210-check tiny210-clean tiny210-bl1 tiny210-sdimg,$(MAKECMDGOALS)),)
# FriendlyARM Tiny210 (Samsung S5PV210): see ports/tiny210/README.md
.PHONY: tiny210 tiny210-qemu tiny210-check tiny210-clean tiny210-bl1 tiny210-sdimg clean
ifneq ($(filter clean,$(MAKECMDGOALS)),)
tiny210 tiny210-qemu tiny210-check tiny210-bl1 tiny210-sdimg: clean
endif
clean:
	$(MAKE) -f ports/tiny210/Makefile BUILD=build/tiny210 clean
	$(MAKE) -f ports/tiny210/Makefile BUILD=build/tiny210-qemu clean
tiny210-clean: clean
tiny210:
	$(MAKE) -f ports/tiny210/Makefile
tiny210-qemu:
	$(MAKE) -f ports/tiny210/Makefile BUILD=build/tiny210-qemu QEMU_TEST=1 qemu
tiny210-check:
	$(MAKE) -f ports/tiny210/Makefile check
tiny210-bl1:
	$(MAKE) -f ports/tiny210/Makefile bl1
tiny210-sdimg:
	$(MAKE) -f ports/tiny210/Makefile sdimg
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
# Default: FriendlyARM Tiny210 (S5PV210), see ports/tiny210/README.md.
# Other targets: tiny210-check, tiny210-qemu, tiny210-clean, t113*, am335x*.
.PHONY: all clean check
.DEFAULT_GOAL := all
all:
	$(MAKE) -f ports/tiny210/Makefile
check:
	$(MAKE) -f ports/tiny210/Makefile check
clean:
	$(MAKE) -f ports/tiny210/Makefile BUILD=build/tiny210 clean
	$(MAKE) -f ports/tiny210/Makefile BUILD=build/tiny210-qemu clean
endif
