# Building the RTL8156 driver plugin, and running the host tests.
#
#   make                 build build/xedrivers.xex
#   make DEBUG=1 all     the same, with extra markers compiled in
#   make test            native unit tests of the pure parts (cdc, stack checksum)
#   make lint            cppcheck / clang-tidy over src/, if installed
#   make push            put the plugin on the Orange Pi's mass-storage image (Usb:)
#   make deploy          push, then arm a slot, reboot, read markers, disarm
#   make kit             regenerate kit/ from xerunner, and show what changed
#   make clean
#
# The build wants only powerpc-linux-gnu-gcc and python3: everything else is in kit/,
# copied from xerunner and committed. The load address is the one fact a registry
# would hold -- a single plugin, so one line.

NAME     := xedrivers
BASE     := 0x91C00000
ORDINALS := 21256.0

SRC      := $(wildcard src/*.c)
HEADERS  := $(wildcard src/*.h)
BUILD    ?= build
DEBUG    ?= 0

KIT      := kit
PYTHON   ?= python3 -B
NM       ?= powerpc-linux-gnu-nm

# The console and the delivery medium; overridable from the environment.
HOST     ?= 192.168.1.210
PI       ?= root@192.168.1.111
PI_IMG   ?= /root/plugin-usb.img
PI_MNT   ?= /mnt/img
SLOT     ?= 3

XERUNNER_HOME ?= $(abspath ../xerunner)
XERUNNER      ?= $(firstword $(wildcard $(XERUNNER_HOME)/.venv/bin/xerunner) xerunner)

XEX := $(BUILD)/$(NAME).xex

.PHONY: all clean test lint kit push deploy

all: $(XEX)

# Which flags the last build used, so `make DEBUG=1` after a plain build rebuilds
# rather than handing back the binary it was meant to replace.
$(BUILD)/flags: FORCE
	@mkdir -p $(BUILD)
	@echo "DEBUG=$(DEBUG) BASE=$(BASE) ORD=$(ORDINALS)" | cmp -s - $@ \
	    || echo "DEBUG=$(DEBUG) BASE=$(BASE) ORD=$(ORDINALS)" > $@
FORCE:

$(XEX): $(SRC) $(HEADERS) $(BUILD)/flags Makefile \
        $(KIT)/ordinals-$(ORDINALS).txt $(KIT)/plugin.py $(KIT)/write.py $(KIT)/kcall.S
	@mkdir -p $(BUILD)
	$(PYTHON) $(KIT)/plugin.py $(SRC) \
	    --ordinals $(KIT)/ordinals-$(ORDINALS).txt --base $(BASE) \
	    -I src -D XE_DEBUG=$(DEBUG) --name $(NAME).dll --into $(BUILD)
	@mv $(BUILD)/plugin.xex $@
	@echo "  -> $@"

# Native tests. cdc.c and stack.c are pure byte code; kernel.h's inline asm is not
# reached by the test translation units, so they build with the host compiler.
test: $(BUILD)/test_cdc $(BUILD)/test_stack
	@$(BUILD)/test_cdc
	@$(BUILD)/test_stack

$(BUILD)/test_cdc: tests/test_cdc.c src/cdc.c tests/fixtures.h $(HEADERS)
	@mkdir -p $(BUILD)
	gcc -std=c99 -Wall -Wextra -O2 -Isrc -o $@ tests/test_cdc.c src/cdc.c

$(BUILD)/test_stack: tests/test_stack.c src/stack.c $(HEADERS)
	@mkdir -p $(BUILD)
	gcc -std=c99 -Wall -Wextra -O2 -Isrc -o $@ tests/test_stack.c src/stack.c

lint:
	@command -v cppcheck >/dev/null && cppcheck --std=c99 --enable=warning,style \
	    --quiet --inline-suppr -Isrc src/ || echo "cppcheck not installed"

kit:
	@cd $(XERUNNER_HOME) && $(XERUNNER) alfred plugin-kit $(abspath $(KIT)) \
	    --kernel $(ORDINALS)
	@git diff --stat -- $(KIT)

# Drop the gadget before writing the image -- an image edited while still served
# leaves the console reading the cached version -- then re-attach and report the
# USB link speed the board negotiated.
push: $(XEX)
	ssh -o StrictHostKeyChecking=no $(PI) \
	    "rmmod g_mass_storage 2>/dev/null; losetup -d /dev/loop0 2>/dev/null; \
	     mkdir -p $(PI_MNT) && mount -o loop $(PI_IMG) $(PI_MNT) && \
	     mkdir -p $(PI_MNT)/plugin && cat > $(PI_MNT)/plugin/$(NAME).xex" < $(XEX)
	ssh -o StrictHostKeyChecking=no $(PI) \
	    "sync && md5sum $(PI_MNT)/plugin/$(NAME).xex" > $(BUILD)/remote.md5
	@md5sum $(XEX) | cut -d' ' -f1 | grep -qf - $(BUILD)/remote.md5 \
	    || { echo "copy on the medium is not what was built"; exit 1; }
	ssh -o StrictHostKeyChecking=no $(PI) \
	    "umount $(PI_MNT) && losetup /dev/loop0 $(PI_IMG) && \
	     modprobe g_mass_storage file=/dev/loop0 removable=1 ro=0 stall=0 && sleep 2 && \
	     echo attached, link \$$(cat /sys/class/udc/*/current_speed)"

deploy: push
	@marker=$$($(NM) $(BUILD)/plugin.elf | awk '$$3=="xe_state"{print "0x"$$1}'); \
	$(XERUNNER) console test-plugin $(SLOT) 'Usb:\plugin\$(NAME).xex' \
	    --host $(HOST) --base $(BASE) --marker $$marker:0x40

clean:
	rm -rf $(BUILD)
