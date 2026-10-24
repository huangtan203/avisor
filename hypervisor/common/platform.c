// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/platform.h"
#include "boards/virt/base.h"
#include "common/printf.h"
#include "common/utils.h"

#define FDT_MAGIC 0xd00dfeedU
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_NOP 4
#define FDT_END 9

#define FDT_COMPATIBLE "compatible"
#define FDT_MODEL "model"
#define FDT_DEVICE_TYPE "device_type"
#define FDT_REG "reg"

struct platform_info platform_current;
unsigned long platform_dtb_address;
unsigned long platform_dtb_address_alt;

static unsigned int be32(const void *ptr)
{
	const unsigned char *p = ptr;

	return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
	       ((unsigned int)p[2] << 8) | p[3];
}

static unsigned long align4(unsigned long value)
{
	return (value + 3) & ~3UL;
}

static unsigned long read_cells(const unsigned char *value, unsigned int cells)
{
	unsigned int i;
	unsigned long result = 0;

	for (i = 0; i < cells; i++)
		result = (result << 32) | be32(value + i * 4);
	return result;
}

static int text_has(const char *data, unsigned int len, const char *needle)
{
	unsigned int i;
	unsigned int n = 0;

	while (needle[n])
		n++;
	if (!n || len < n)
		return 0;

	for (i = 0; i + n <= len; i++) {
		unsigned int j;

		for (j = 0; j < n && data[i + j] == needle[j]; j++)
			;
		if (j == n)
			return 1;
	}
	return 0;
}

static const char *fdt_string(const unsigned char *strings, unsigned int size,
			      unsigned int offset)
{
	if (offset >= size)
		return NULL;
	return (const char *)(strings + offset);
}

static int fdt_parse(unsigned long dtb)
{
	const unsigned char *base = (const unsigned char *)dtb;
	const unsigned char *structs;
	const unsigned char *strings;
	unsigned int totalsize, struct_off, strings_off, struct_size, strings_size;
	unsigned int address_cells = 2, size_cells = 1;
	unsigned long cursor;
	int depth = -1;
	int memory_node = 0;
	int cpu_node = 0;
	int uart_node = 0;
	int gic_node = 0;

	if (!dtb || be32(base) != FDT_MAGIC)
		return -1;

	totalsize = be32(base + 4);
	struct_off = be32(base + 8);
	strings_off = be32(base + 12);
	struct_size = be32(base + 36);
	strings_size = be32(base + 32);
	if (totalsize < 40 || struct_off >= totalsize || strings_off >= totalsize ||
	    struct_size > totalsize - struct_off ||
	    strings_size > totalsize - strings_off)
		return -1;

	structs = base + struct_off;
	strings = base + strings_off;
	cursor = 0;
	while (cursor < struct_size) {
		unsigned int token = be32(structs + cursor);
		cursor += 4;

		if (token == FDT_BEGIN_NODE) {
			const char *name = (const char *)(structs + cursor);
			unsigned int len = 0;

			while (cursor + len < struct_size && name[len])
				len++;
			cursor = align4(cursor + len + 1);
			depth++;
			memory_node = text_has(name, len, "memory");
			cpu_node = depth >= 1 && text_has(name, len, "cpu");
			uart_node = text_has(name, len, "pl011");
			gic_node = text_has(name, len, "intc") || text_has(name, len, "gic");
			continue;
		}
		if (token == FDT_END_NODE) {
			depth--;
			memory_node = 0;
			cpu_node = 0;
			continue;
		}
		if (token == FDT_NOP)
			continue;
		if (token == FDT_END)
			break;
		if (token == FDT_PROP) {
			unsigned int len, nameoff;
			const char *prop;
			const unsigned char *value;

			if (cursor + 8 > struct_size)
				return -1;
			len = be32(structs + cursor);
			nameoff = be32(structs + cursor + 4);
			cursor += 8;
			if (cursor + len > struct_size)
				return -1;
			prop = fdt_string(strings, strings_size, nameoff);
			value = structs + cursor;
			if (!prop)
				return -1;

			if (depth == 0 && !strcmp(prop, FDT_COMPATIBLE)) {
				if (text_has((const char *)value, len, "nvidia,tegra234") ||
				    text_has((const char *)value, len, "jetson-agx-orin")) {
					platform_current.kind = PLATFORM_KIND_AGX_ORIN;
					platform_current.name = "NVIDIA Jetson AGX Orin";
				}
				if (text_has((const char *)value, len, "linux,dummy-virt")) {
					platform_current.kind = PLATFORM_KIND_QEMU_VIRT;
					platform_current.name = "QEMU virt";
				}
			}
			if (depth == 0 && !strcmp(prop, "#address-cells") && len == 4)
				address_cells = be32(value);
			if (depth == 0 && !strcmp(prop, "#size-cells") && len == 4)
				size_cells = be32(value);
			if (depth >= 2 && cpu_node && !strcmp(prop, FDT_DEVICE_TYPE) &&
			    text_has((const char *)value, len, "cpu"))
				platform_current.cpu_count++;
			if (depth >= 1 && uart_node && !strcmp(prop, FDT_REG) &&
			    len >= (address_cells + size_cells) * 4)
				platform_current.uart_base = read_cells(value, address_cells);
			if (depth >= 1 && gic_node && !strcmp(prop, FDT_REG) &&
			    len >= (address_cells + size_cells) * 4)
				platform_current.gicd_base = read_cells(value, address_cells);
			if (depth == 1 && memory_node && !strcmp(prop, FDT_REG) &&
			    len >= (address_cells + size_cells) * 4) {
				unsigned int i;
				unsigned long address = 0, size = 0;
				for (i = 0; i < address_cells; i++)
					address = (address << 32) | be32(value + i * 4);
				for (i = 0; i < size_cells; i++)
					size = (size << 32) | be32(value + (address_cells + i) * 4);
				platform_current.memory_base = address;
				platform_current.memory_size = size;
			}
			cursor = align4(cursor + len);
			continue;
		}
		return -1;
	}
	return 0;
}

const char *platform_kind_name(enum platform_kind kind)
{
	if (kind == PLATFORM_KIND_QEMU_VIRT)
		return "QEMU virt";
	if (kind == PLATFORM_KIND_AGX_ORIN)
		return "NVIDIA Jetson AGX Orin";
	return "unknown";
}

void platform_early_probe(void)
{
	memzero(&platform_current, sizeof(platform_current));
	platform_current.kind = PLATFORM_KIND_QEMU_VIRT;
	platform_current.name = "QEMU virt (fixed-layout fallback)";
	platform_current.dtb = platform_dtb_address;
	if (!platform_current.dtb || be32((const void *)platform_current.dtb) != FDT_MAGIC)
		platform_current.dtb = platform_dtb_address_alt;
	platform_current.cpu_count = MAX_CPUS;
	platform_current.memory_base = VIRT_DRAM_BASE;
	platform_current.memory_size = PHYS_MEMORY_SIZE;
	platform_current.uart_base = VIRT_UART_BASE;
	platform_current.gicd_base = VIRT_GICD_BASE;
	platform_current.gicr_base = VIRT_GICR_BASE;
	platform_current.cpu_count = 0;
	platform_current.dtb_valid = fdt_parse(platform_current.dtb) == 0;
	if (platform_current.dtb_valid && !platform_current.cpu_count)
		platform_current.cpu_count = MAX_CPUS;
	if (!platform_current.dtb_valid) {
		platform_current.cpu_count = MAX_CPUS;
		platform_current.memory_base = VIRT_DRAM_BASE;
		platform_current.memory_size = PHYS_MEMORY_SIZE;
		platform_current.uart_base = VIRT_UART_BASE;
		platform_current.gicd_base = VIRT_GICD_BASE;
		platform_current.gicr_base = VIRT_GICR_BASE;
	}

	printf("[platform] %s, dtb=%p (%s)\n", platform_current.name,
	       (void *)platform_current.dtb,
	       platform_current.dtb_valid ? "valid" : "fallback");
	printf("[platform] cpu=%lu memory=%p+%luMB uart=%p gicd=%p gicr=%p\n",
	       platform_current.cpu_count,
	       (void *)platform_current.memory_base,
	       platform_current.memory_size >> 20,
	       (void *)platform_current.uart_base,
	       (void *)platform_current.gicd_base,
	       (void *)platform_current.gicr_base);
	if (platform_current.kind == PLATFORM_KIND_AGX_ORIN)
		printf("[platform] AGX Orin recognized: hardware drivers not enabled\n");
}
