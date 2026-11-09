ARCH              = arm64
TARGET            = aarch64-linux-gnu
CC                = clang
CFLAGS            = --target=$(TARGET)
OBJCOPY           = llvm-objcopy

all: BootShim.elf BootShim.bin

BootShim.bin: BootShim.elf
	$(OBJCOPY) -O binary $< $@

BootShim.elf: BootShim.S
	$(CC) ${CFLAGS} -c $< -o $@ -DREQUIRES_KERNEL_HEADER=$(REQUIRES_KERNEL_HEADER) -DFD_BASE=$(FD_BASE) -DFD_SIZE=$(FD_SIZE) \
	  $(if $(PAYLOAD_BASE),-DPAYLOAD_BASE=$(PAYLOAD_BASE)) $(if $(PAYLOAD_SIZE),-DPAYLOAD_SIZE=$(PAYLOAD_SIZE))

BootShim.S:
