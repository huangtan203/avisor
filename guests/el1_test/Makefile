# aVisor Guest - EL1 测试程序构建脚本
#
# 输出 el1_test.bin（纯二进制），由 Hypervisor 的
# hypervisor/common/guest_image.S 通过 .incbin 嵌入主镜像。

GNU_TOOLS ?= aarch64-linux-gnu
GNU_GCC = $(GNU_TOOLS)-gcc
GNU_LD = $(GNU_TOOLS)-ld
GNU_OBJCOPY = $(GNU_TOOLS)-objcopy

C_OPS = -Wall -nostdlib -nostartfiles -ffreestanding -mgeneral-regs-only \
        -I. -O2 -g
ASM_OPS = -I. -g

OBJS = start_s.o vectors_s.o main_c.o

.PHONY: all
all: el1_test.bin

%.o: %.c
	$(GNU_GCC) $(C_OPS) -c $< -o $@

%_c.o: %.c
	$(GNU_GCC) $(C_OPS) -c $< -o $@

%_s.o: %.S
	$(GNU_GCC) $(ASM_OPS) -c $< -o $@

el1_test.elf: linker.ld $(OBJS)
	$(GNU_LD) -T linker.ld -o el1_test.elf $(OBJS)

el1_test.bin: el1_test.elf
	$(GNU_OBJCOPY) el1_test.elf -O binary el1_test.bin
	$(GNU_TOOLS)-objdump -d el1_test.elf > el1_test.list
	@echo ""
	@echo "=== Guest build complete ==="
	@echo "Image: el1_test.bin"
	@echo "Size:  $$(ls -l el1_test.bin | awk '{print $$5}') bytes"

.PHONY: clean
clean:
	rm -f *.o *.elf *.bin *.list
