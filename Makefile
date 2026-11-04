# Building the RTL8156 driver plugin, and running the host tests.
#
#   make                 build the package build/xedrivers/: xedrivers.xex, the plugin
#                        for people (no tuning), and firmware/ (FIRMWARE below:
#                        fetched, pinned by hash, laid out as linux-firmware)
#   make bench=history,stack   ... with the tuning tools named (src/bench/<name>):
#                        history, stack, debug, prof, tcp, or bench=all
#   make MODE=xnet ADVERTISE=2500 IP=10.36.0.2
#                        choose how it runs (see "Modes" below)
#   make test            native unit tests of the pure parts (cdc, stack checksum)
#   make lint            cppcheck / clang-tidy over src/, if installed
#   make push            put the plugin on the Orange Pi's mass-storage image
#                        (Usb:\plugin\xedrivers\)
#   make push-fw         put the package's firmware/ beside it
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

# Modes. MODE: who gets the frames -- standalone (the built-in responder on IP),
# xnet (the console's own network) or drop. ADVERTISE: the fastest link the PHY
# offers -- 2500, 1000, 100, or default (the card's firmware choice, 10/100 in
# CDC-ECM). All three can also be changed on a running console: tools/xectl.py.
MODE      ?= xnet
ADVERTISE ?= 2500
IP        ?= 10.36.0.2
RX_COUNT  ?= 32
TX_COUNT  ?= 64
NCM_RX_COUNT ?= 16
NCM_TX_COUNT ?= 16
NTB_SIZE  ?= 16384
NTB_IN_SIZE ?= 8192
TRANSPORT ?= ecm
transport_ecm := 0
transport_ncm := 1
TRANSPORT_VALUE := $(or $(transport_$(TRANSPORT)),$(error TRANSPORT must be ecm or ncm))

mode_standalone := 1
mode_xnet       := 2
mode_drop       := 0
adv_2500        := 0x6F
adv_1000        := 0x2F
adv_100         := 0x0F
adv_default     := 0
MODE_VALUE := $(or $(mode_$(MODE)),$(error MODE must be standalone, xnet or drop))
ADV_VALUE  := $(or $(adv_$(ADVERTISE)),$(error ADVERTISE must be 2500, 1000, 100 or default))
IP_VALUE   := $(shell python3 -c "import ipaddress,sys; print(hex(int(ipaddress.IPv4Address('$(IP)'))))")

# bench= names the tuning tools to build in, comma-separated, each a directory of
# src/bench/: history (flight recorder), stack (MODE=standalone and its UDP
# generator), debug (commands 15-17), prof (stage timers), tcp (the throughput
# server -- the plugin's only imports from xam); `all` for every one. Empty, the
# default, is the plugin for people: none of src/bench/ is compiled in.
comma      := ,
empty      :=
space      := $(empty) $(empty)
BENCH_ALL  := history stack debug prof tcp trace
BENCH_ASKED := $(subst $(comma), ,$(bench))
BENCH_LIST := $(sort $(if $(filter all,$(BENCH_ASKED)),$(BENCH_ALL),$(BENCH_ASKED)))
$(if $(filter-out $(BENCH_ALL),$(BENCH_LIST)),\
    $(error bench= knows $(BENCH_ALL) and all, not $(filter-out $(BENCH_ALL),$(BENCH_LIST))))
$(if $(filter standalone,$(MODE)),$(if $(filter stack,$(BENCH_LIST)),,\
    $(error MODE=standalone is the bench's responder: build with bench=stack)))
bench_on    = $(if $(filter $(1),$(BENCH_LIST)),1,0)
BENCH_FLAGS := -D XE_BENCH=$(if $(BENCH_LIST),1,0) \
               -D XE_BENCH_HISTORY=$(call bench_on,history) \
-D XE_BENCH_TRACE=$(call bench_on,trace) \
               -D XE_BENCH_STACK=$(call bench_on,stack) \
               -D XE_BENCH_DEBUG=$(call bench_on,debug) \
               -D XE_PROF=$(call bench_on,prof) -D XE_TCP=$(call bench_on,tcp)

BUILD    ?= build

# The core, every driver directory in DRIVERS (all of src/drivers/ by default),
# and the bench's dispatcher and each tool named in bench=. Each driver directory defines one XE_DRIVER named
# xe_driver_<directory>; the table of them is generated, so nothing else names a
# driver (src/core/driver.h).
#
# src/drivers/common/ holds code shared by drivers, not drivers: built only when a
# driver in DRIVERS needs it. NEEDS_<driver> names what a driver uses besides its
# own directory (common/<name>, or another driver's code, as rtl8156 runs on cdc's).
NEEDS_rtl8156 := cdc common/realtek
NEEDS_rtl8153 := cdc common/realtek
NEEDS_ax88179 := common/asix
NEEDS_ax88178 := common/asix
DRIVERS  ?= $(filter-out common,$(notdir $(patsubst %/,%,$(wildcard src/drivers/*/))))
DRIVER_DIRS := $(sort $(DRIVERS) $(foreach d,$(DRIVERS),$(NEEDS_$(d))))
SRC_DIRS := src/core src/net src/wireless src/wireless/softmac \
            $(addprefix src/drivers/,$(DRIVER_DIRS)) \
            $(if $(BENCH_LIST),src/bench $(addprefix src/bench/,$(BENCH_LIST)))
SRC      := $(foreach d,$(SRC_DIRS),$(wildcard $(d)/*.c)) $(BUILD)/drivers.generated.c
HEADERS  := $(wildcard src/*/*.h src/*/*/*.h src/*/*/*/*.h)
INCLUDES := $(addprefix -I ,$(SRC_DIRS))
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

PKG := $(BUILD)/$(NAME)
XEX := $(PKG)/$(NAME).xex
FW_DIR := $(PKG)/firmware

# The firmware the drivers load: its path under firmware/ (as in linux-firmware,
# src/core/file.h), SHA-256, and where it comes from, at a fixed commit. Nothing is
# taken from this machine's /lib/firmware, so every build packs the same bytes.
# These are the files the drivers were tested with:
# - RTL8822BU: linux-firmware a50c068b ("rtw88: 8822b: Update firmware to
#   v30.20.0"). A/B against v27.2.0 (338684a0) on the console, 2026-11-02: the same
#   within 2% each way, TCP and FTP.
# - AIC8800D80: aicsemi's USB driver package as radxa-pkg/aic8800 keeps it
#   (src/USB/driver_fw/fw); the bytes the driver's usbmon checks were made
#   against. Not in linux-firmware.
LINUX_FW := https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain
LINUX_FW_COMMIT := a50c068b4977be0845596e8294b47199ed2c5e2b
AIC_FW   := https://raw.githubusercontent.com/radxa-pkg/aic8800/d9c72efd382ccdbf2f1fdb09d2d4f3cc212edd9d/src/USB/driver_fw/fw
define FIRMWARE
rtw88/rtw8822b_fw.bin a72da690597bfa99d8eb6fc2ab090d18d8ad92ac2befd35db1c9e3662d8d8418 $(LINUX_FW)/rtw88/rtw8822b_fw.bin?id=$(LINUX_FW_COMMIT)
aic8800D80/fw_patch_table_8800d80_u02.bin 9decb77435b7e9713e33e32da483d683b7329ed93b672b2d1b134031d7da5f67 $(AIC_FW)/aic8800D80/fw_patch_table_8800d80_u02.bin
aic8800D80/fw_adid_8800d80_u02.bin a526cbd02fcdc495f049f3ad6b5933cb08cd984b16790c716a060d582fee1a56 $(AIC_FW)/aic8800D80/fw_adid_8800d80_u02.bin
aic8800D80/fw_patch_8800d80_u02.bin f0e2f5bbc17bc327ca7f1574ff55370dfd863d931514347bb4abc18a74f6218f $(AIC_FW)/aic8800D80/fw_patch_8800d80_u02.bin
aic8800D80/fw_patch_8800d80_u02_ext0.bin ca738f5ea5aca6021c4f48b60ff35e835dfdaf072a8490e3ba8ba0a42440e3e5 $(AIC_FW)/aic8800D80/fw_patch_8800d80_u02_ext0.bin
aic8800D80/fmacfw_8800d80_u02.bin 23f2bed2ac0e8f7bda8f36df7defd30d9b928c97867f6f0f01f28d89dd78d670 $(AIC_FW)/aic8800D80/fmacfw_8800d80_u02.bin
aic8800D80/fmacfw_8800d80_h_u02.bin c5c8c774dfae93aed71f8da7489e35159192135e4929591ea8a945b76deba6bd $(AIC_FW)/aic8800D80/fmacfw_8800d80_h_u02.bin
endef
export FIRMWARE

# The commit the plugin is built from and whether tracked files differed from it,
# kept in the image (xe_state.build_commit / build_dirty) so what runs on the
# console says what it was built from (tools/xectl.py status, bench --record).
GIT_COMMIT := $(or $(shell git rev-parse HEAD 2>/dev/null | cut -c1-8),0)
GIT_DIRTY  := $(if $(shell git status --porcelain --untracked-files=no 2>/dev/null),1,0)
FLAGS_LINE := COMMIT=$(GIT_COMMIT)/$(GIT_DIRTY) DRIVERS=$(DRIVERS) DEBUG=$(DEBUG) BASE=$(BASE) ORD=$(ORDINALS) MODE=$(MODE_VALUE) ADV=$(ADV_VALUE) IP=$(IP_VALUE) RX=$(RX_COUNT) TX=$(TX_COUNT) NRX=$(NCM_RX_COUNT) NTX=$(NCM_TX_COUNT) NTB=$(NTB_SIZE)/$(NTB_IN_SIZE) T=$(TRANSPORT_VALUE) bench=$(subst $(space),$(comma),$(BENCH_LIST))

.PHONY: all firmware clean test lint kit push push-fw deploy

# The package: what goes into Usb:\plugin\xedrivers\ (or Hdd:), and what a release
# ships. The firmware is checked first and fetched where missing or wrong.
all: firmware $(XEX)

# A file in place with the right hash is kept; any other is fetched and checked.
# (git.kernel.org refuses some clients without a User-Agent of their own.)
firmware:
	@echo "$$FIRMWARE" | while read path sum url; do \
	    f=$(FW_DIR)/$$path; \
	    echo "$$sum  $$f" | sha256sum -c --status 2>/dev/null && continue; \
	    echo "  fetch $$path"; mkdir -p $$(dirname $$f); \
	    curl -fsSL -A xedrivers -o $$f.part "$$url" || { echo "cannot fetch $$url"; exit 1; }; \
	    echo "$$sum  $$f.part" | sha256sum -c --status \
	        || { echo "$$url: not the pinned sha256 $$sum"; rm -f $$f.part; exit 1; }; \
	    mv $$f.part $$f; \
	done

# Which flags the last build used, so `make DEBUG=1` after a plain build rebuilds
# rather than handing back the binary it was meant to replace.
$(BUILD)/flags: FORCE
	@mkdir -p $(BUILD)
	@echo "$(FLAGS_LINE)" | cmp -s - $@ || echo "$(FLAGS_LINE)" > $@
FORCE:

# One ordinal table per module the plugin may import from, each naming its module
# and identity in its header; the packer looks every import up in all of them and
# gives a module a record only when something is imported from it (xam: bench=tcp).
TABLES := $(KIT)/ordinals-xboxkrnl-$(ORDINALS).txt $(KIT)/ordinals-xam-$(ORDINALS).txt

$(BUILD)/drivers.generated.c: $(BUILD)/flags
	@mkdir -p $(BUILD)
	@{ echo '/* Generated by the Makefile from DRIVERS; do not edit. */'; \
	   echo '#include "driver.h"'; \
	   for d in $(DRIVERS); do echo "extern const XE_DRIVER xe_driver_$$d;"; done; \
	   echo 'const XE_DRIVER *const xe_drivers[] = {'; \
	   for d in $(DRIVERS); do echo "    &xe_driver_$$d,"; done; \
	   echo '};'; \
	   echo 'const u32 xe_driver_count = sizeof(xe_drivers) / sizeof(xe_drivers[0]);'; \
	} > $@

$(XEX): $(SRC) $(HEADERS) $(BUILD)/flags Makefile $(TABLES) \
        $(KIT)/plugin.py $(KIT)/write.py $(KIT)/kcall.S
	@mkdir -p $(BUILD) $(PKG)
	$(PYTHON) $(KIT)/plugin.py $(SRC) $(addprefix --ordinals ,$(TABLES)) \
	    --base $(BASE) \
	    $(INCLUDES) -D XE_DEBUG=$(DEBUG) -D XE_MODE=$(MODE_VALUE)u \
	    -D XE_ADVERTISE=$(ADV_VALUE)u -D XE_IP=$(IP_VALUE)u \
	    -D RX_COUNT=$(RX_COUNT) -D TX_COUNT=$(TX_COUNT) -D NTB_SIZE=$(NTB_SIZE) -D NTB_IN_SIZE=$(NTB_IN_SIZE) \
	    -D NCM_RX_COUNT=$(NCM_RX_COUNT) -D NCM_TX_COUNT=$(NCM_TX_COUNT) \
	    -D XE_TRANSPORT=$(TRANSPORT_VALUE)u $(BENCH_FLAGS) \
	    -D XE_BUILD_COMMIT=0x$(GIT_COMMIT)u -D XE_BUILD_DIRTY=$(GIT_DIRTY)u \
	    --name $(NAME).dll --into $(BUILD)
	@mv $(BUILD)/plugin.xex $@
	@echo "  -> $@"

# Native tests. cdc.c and stack.c are pure byte code; kernel.h's inline asm is not
# reached by the test translation units, so they build with the host compiler.
# Each test sits beside the code it tests: tests/drivers/<driver>/, tests/bench/.
test: firmware $(BUILD)/test_cdc $(BUILD)/test_stack $(BUILD)/test_rtl $(BUILD)/test_rndis \
      $(BUILD)/test_asix $(BUILD)/test_ax88179 $(BUILD)/test_ax88178 \
      $(BUILD)/test_aic $(BUILD)/test_wpa $(BUILD)/test_reorder \
      $(BUILD)/test_bridge $(BUILD)/test_omni
	@$(BUILD)/test_cdc
	@$(BUILD)/test_stack
	@$(BUILD)/test_rtl
	@$(BUILD)/test_rndis
	@$(BUILD)/test_asix
	@$(BUILD)/test_ax88179
	@$(BUILD)/test_ax88178
	@AIC_FW=$(FW_DIR)/aic8800D80 $(BUILD)/test_aic
	@$(BUILD)/test_wpa
	@$(BUILD)/test_reorder
	@$(BUILD)/test_bridge
	@$(BUILD)/test_omni

HOST_CC := gcc -std=c99 -Wall -Wextra -O2 -I src/core -I src/net -I src/wireless

$(BUILD)/test_cdc: tests/drivers/cdc/test_cdc.c src/drivers/cdc/cdc.c \
                   tests/drivers/cdc/fixtures.h $(HEADERS)
	@mkdir -p $(BUILD)
	$(HOST_CC) -I src/drivers/cdc -o $@ tests/drivers/cdc/test_cdc.c src/drivers/cdc/cdc.c

$(BUILD)/test_stack: tests/bench/stack/test_stack.c src/bench/stack/stack.c $(HEADERS)
	@mkdir -p $(BUILD)
	$(HOST_CC) -I src/bench/stack -o $@ tests/bench/stack/test_stack.c \
	    src/bench/stack/stack.c

$(BUILD)/test_rtl: tests/drivers/common/realtek/test_rtl.c src/drivers/common/realtek/rtl.c $(HEADERS)
	@mkdir -p $(BUILD)
	$(HOST_CC) -I src/drivers/common/realtek -o $@ tests/drivers/common/realtek/test_rtl.c \
	    src/drivers/common/realtek/rtl.c

$(BUILD)/test_asix: tests/drivers/common/asix/test_asix.c src/drivers/common/asix/asix.c $(HEADERS)
	@mkdir -p $(BUILD)
	$(HOST_CC) -I src/drivers/common/asix -o $@ tests/drivers/common/asix/test_asix.c src/drivers/common/asix/asix.c

$(BUILD)/test_ax88179: tests/drivers/ax88179/test_ax88179.c src/drivers/ax88179/ax88179_frames.c \
                       $(HEADERS)
	@mkdir -p $(BUILD)
	$(HOST_CC) -I src/drivers/ax88179 -o $@ tests/drivers/ax88179/test_ax88179.c \
	    src/drivers/ax88179/ax88179_frames.c

$(BUILD)/test_ax88178: tests/drivers/ax88178/test_ax88178.c src/drivers/ax88178/ax88178_frames.c \
                       $(HEADERS)
	@mkdir -p $(BUILD)
	$(HOST_CC) -I src/drivers/ax88178 -o $@ tests/drivers/ax88178/test_ax88178.c \
	    src/drivers/ax88178/ax88178_frames.c

$(BUILD)/test_rndis: tests/drivers/rndis/test_rndis.c src/drivers/rndis/rndis.c $(HEADERS)
	@mkdir -p $(BUILD)
	$(HOST_CC) -I src/drivers/rndis -o $@ tests/drivers/rndis/test_rndis.c \
	    src/drivers/rndis/rndis.c

$(BUILD)/test_aic: tests/drivers/aic8800/test_aic.c src/drivers/aic8800/aic_fw.c \
                   src/drivers/aic8800/aic_wifi.c src/drivers/aic8800/aic_data.c \
                   src/drivers/aic8800/aic_bt.c \
                   src/wireless/wireless.c src/wireless/wpa.c src/wireless/reorder.c \
                   src/wireless/bridge.c \
                   tests/drivers/aic8800/wifi-8d81.txt \
                   src/drivers/aic8800/aic_cmd.c tests/drivers/aic8800/load-8d80.txt $(HEADERS)
	@mkdir -p $(BUILD)
	$(HOST_CC) -I src/drivers/aic8800 -o $@ tests/drivers/aic8800/test_aic.c \
	    src/drivers/aic8800/aic_fw.c src/drivers/aic8800/aic_cmd.c \
	    src/drivers/aic8800/aic_wifi.c src/drivers/aic8800/aic_data.c \
	    src/drivers/aic8800/aic_bt.c src/wireless/wireless.c src/wireless/wpa.c \
	    src/wireless/reorder.c src/wireless/bridge.c

$(BUILD)/test_bridge: tests/wireless/test_bridge.c src/wireless/bridge.c \
                      src/wireless/wireless.c $(HEADERS)
	@mkdir -p $(BUILD)
	$(HOST_CC) -o $@ tests/wireless/test_bridge.c src/wireless/bridge.c \
	    src/wireless/wireless.c

$(BUILD)/test_omni: tests/wireless/test_omni.c src/wireless/omni.c src/net/txq.c \
                    src/wireless/wireless.c $(HEADERS)
	@mkdir -p $(BUILD)
	$(HOST_CC) -o $@ tests/wireless/test_omni.c src/net/txq.c src/wireless/wireless.c

$(BUILD)/test_reorder: tests/wireless/test_reorder.c src/wireless/reorder.c $(HEADERS)
	@mkdir -p $(BUILD)
	$(HOST_CC) -o $@ tests/wireless/test_reorder.c src/wireless/reorder.c

$(BUILD)/test_wpa: tests/wireless/test_wpa.c src/wireless/wpa.c $(HEADERS)
	@mkdir -p $(BUILD)
	$(HOST_CC) -o $@ tests/wireless/test_wpa.c src/wireless/wpa.c

lint:
	@command -v cppcheck >/dev/null && cppcheck --std=c99 --enable=warning,style \
	    --quiet --inline-suppr $(INCLUDES) src/ || echo "cppcheck not installed"

kit:
	@cd $(XERUNNER_HOME) && $(XERUNNER) alfred plugin-kit $(abspath $(KIT)) \
	    --kernel $(ORDINALS) --module xboxkrnl.exe --module xam.xex
	@git diff --stat -- $(KIT)

# Drop the gadget before writing the image -- an image edited while still served
# leaves the console reading the cached version -- then re-attach and report the
# USB link speed the board negotiated.
push: all
	ssh -o StrictHostKeyChecking=no $(PI) \
	    "rmmod g_mass_storage 2>/dev/null; losetup -d /dev/loop0 2>/dev/null; \
	     mkdir -p $(PI_MNT) && mount -o loop $(PI_IMG) $(PI_MNT) && \
	     mkdir -p $(PI_MNT)/plugin/$(NAME) && cat > $(PI_MNT)/plugin/$(NAME)/$(NAME).xex" < $(XEX)
	ssh -o StrictHostKeyChecking=no $(PI) \
	    "sync && md5sum $(PI_MNT)/plugin/$(NAME)/$(NAME).xex" > $(BUILD)/remote.md5
	@md5sum $(XEX) | cut -d' ' -f1 | grep -qf - $(BUILD)/remote.md5 \
	    || { echo "copy on the medium is not what was built"; exit 1; }
	ssh -o StrictHostKeyChecking=no $(PI) \
	    "umount $(PI_MNT) && losetup /dev/loop0 $(PI_IMG) && \
	     modprobe g_mass_storage file=/dev/loop0 removable=1 ro=0 stall=0 && sleep 2 && \
	     echo attached, link \$$(cat /sys/class/udc/*/current_speed)"

push-fw: firmware
	tar -C $(FW_DIR) -cf - . | ssh -o StrictHostKeyChecking=no $(PI) \
	    "rmmod g_mass_storage 2>/dev/null; losetup -d /dev/loop0 2>/dev/null; \
	     mkdir -p $(PI_MNT) && mount -o loop $(PI_IMG) $(PI_MNT) && \
	     mkdir -p $(PI_MNT)/plugin/$(NAME)/firmware && \
	     tar --no-same-owner -C $(PI_MNT)/plugin/$(NAME)/firmware -xf - && \
	     sync && find $(PI_MNT)/plugin/$(NAME)/firmware -type f && umount $(PI_MNT) && \
	     losetup /dev/loop0 $(PI_IMG) && \
	     modprobe g_mass_storage file=/dev/loop0 removable=1 ro=0 stall=0 && sleep 2 && \
	     echo attached, link \$$(cat /sys/class/udc/*/current_speed)"

deploy: push
	@marker=$$($(NM) $(BUILD)/plugin.elf | awk '$$3=="xe_state"{print "0x"$$1}'); \
	$(XERUNNER) console test-plugin $(SLOT) 'Usb:\plugin\$(NAME)\$(NAME).xex' \
	    --host $(HOST) --base $(BASE) --marker $$marker:0x40

clean:
	rm -rf $(BUILD)
