# Out-of-tree build of the evergo hot-jump kernel module.
#
# The module must be built against the Android 5.10 kernel tree that ships with
# this device, with LTO and CFI enabled: the vendor kernel CFI-checks the
# mod->init indirect call, and a module built without it is rejected at load
# time (or faults). See stage1/README.md for the full procedure.
#
# Usage:
#   make KDIR=~/kernel_xiaomi_evergo \
#        CLANG_DIR=~/android-ndk-r30/toolchains/llvm/prebuilt/linux-x86_64/bin
#
# KDIR must be a *prepared* kernel tree: configured, and made ready for modules
# (see stage1/README.md). A pristine source checkout is not enough.

KDIR ?=
CLANG_DIR ?=

export PATH := $(CLANG_DIR):$(PATH)

obj-m += evergo_stage1.o

# The vendor kernel CFI-checks the mod->init indirect call, so LTO and CFI must
# be on the make command line: CONFIG_LTO_CLANG=y CONFIG_CFI_CLANG=y.
all:
	@test -n "$(KDIR)" || { echo "KDIR is not set: path to the prepared evergo kernel tree"; exit 1; }
	@test -n "$(CLANG_DIR)" || { echo "CLANG_DIR is not set: path to the NDK llvm bin directory"; exit 1; }
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=arm64 \
		CC=$(CLANG_DIR)/clang \
		LD=$(CLANG_DIR)/ld.lld \
		AR=$(CLANG_DIR)/llvm-ar \
		NM=$(CLANG_DIR)/llvm-nm \
		OBJCOPY=$(CLANG_DIR)/llvm-objcopy \
		STRIP=$(CLANG_DIR)/llvm-strip \
		CLANG_TRIPLE=aarch64-linux-gnu- \
		CROSS_COMPILE=aarch64-linux-gnu- \
		CONFIG_LTO_CLANG=y CONFIG_CFI_CLANG=y \
		modules

clean:
	@test -n "$(KDIR)" || { echo "KDIR is not set"; exit 1; }
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
