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
#define FDT_INTERRUPTS "interrupts"
#define FDT_REDIST_STRIDE "redistributor-stride"

#define GIC_PPI_TYPE 1U
#define GIC_PPI_BASE 16U
#define TIMER_VIRT_INDEX 2U
#define TIMER_HYP_INDEX 3U
#define TIMER_IRQ_CELLS 3U
#define GITS_TRANSLATER_END 0x10044UL

struct platform_info platform_current;
unsigned long virt_uart_base = VA_START + VIRT_UART_BASE;
unsigned long virt_gicd_base = VA_START + VIRT_GICD_BASE;
unsigned long virt_gicr_base = VA_START + VIRT_GICR_BASE;
unsigned long virt_gits_base = VA_START + VIRT_GITS_BASE;
unsigned long platform_dtb_address;
unsigned long platform_dtb_address_alt;
unsigned long platform_cpu_mpidr[MAX_CPUS];

extern char image_end[];

static unsigned int be32(const void *ptr)
{
	const unsigned char *p = ptr;

	return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
	       ((unsigned int)p[2] << 8) | p[3];
}

static unsigned long be64(const void *ptr)
{
	const unsigned char *p = ptr;

	return ((unsigned long)be32(p) << 32) | be32(p + 4);
}

static unsigned long align4(unsigned long value)
{
	return (value + 3) & ~3UL;
}

static unsigned long align_up(unsigned long value, unsigned long alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

static unsigned long align_down(unsigned long value, unsigned long alignment)
{
	return value & ~(alignment - 1);
}

static unsigned long read_cells(const unsigned char *value, unsigned int cells)
{
	unsigned int i;
	unsigned long result = 0;

	for (i = 0; i < cells; i++)
		result = (result << 32) | be32(value + i * 4);
	return result;
}

static int range_within(unsigned long base, unsigned long size,
			unsigned long window_base, unsigned long window_size)
{
	return size && base >= window_base && base - window_base < window_size &&
	       size <= window_size - (base - window_base);
}

static int range_valid(unsigned long base, unsigned long size)
{
	return size && size <= ~0UL - base;
}

static int ranges_overlap(unsigned long base1, unsigned long size1,
			  unsigned long base2, unsigned long size2)
{
	if (!range_valid(base1, size1) || !range_valid(base2, size2))
		return 0;
	return base1 < base2 + size2 && base2 < base1 + size1;
}

static void add_reserved_range(unsigned long base, unsigned long size)
{
	struct platform_reserved_range *range;

	if (!range_valid(base, size)) {
		platform_current.reserved_overflow++;
		return;
	}
	if (platform_current.reserved_count >= PLATFORM_MAX_RESERVED_RANGES) {
		platform_current.reserved_overflow++;
		return;
	}
	range = &platform_current.reserved[platform_current.reserved_count++];
	range->base = base;
	range->size = size;
	range->type = PLATFORM_MEMORY_FIRMWARE;
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

static int compatible_has(const unsigned char *data, unsigned int len,
			  const char *needle)
{
	unsigned int pos = 0;

	while (pos < len) {
		unsigned int end = pos;

		while (end < len && data[end])
			end++;
		if (end - pos == strlen(needle) &&
		    !memcmp(data + pos, needle, end - pos))
			return 1;
		pos = end + 1;
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

static void add_occupied_range(unsigned long base, unsigned long size,
			       enum platform_memory_range_type type)
{
	unsigned long memory_start = platform_current.memory_base;
	unsigned long memory_end = memory_start + platform_current.memory_size;
	unsigned long end;
	struct platform_reserved_range *range;

	if (!range_valid(base, size) || base >= memory_end || base + size <= memory_start)
		return;
	end = base + size;
	if (base < memory_start)
		base = memory_start;
	if (end > memory_end)
		end = memory_end;
	if (platform_current.occupied_count >= PLATFORM_MAX_MEMORY_RANGES) {
		platform_current.memory_map_overflow++;
		return;
	}
	range = &platform_current.occupied[platform_current.occupied_count++];
	range->base = base;
	range->size = end - base;
	range->type = type;
}

static void normalize_occupied_ranges(void)
{
	unsigned int i, j;

	for (i = 1; i < platform_current.occupied_count; i++) {
		struct platform_reserved_range item = platform_current.occupied[i];

		for (j = i; j && platform_current.occupied[j - 1].base > item.base; j--)
			platform_current.occupied[j] = platform_current.occupied[j - 1];
		platform_current.occupied[j] = item;
	}
	for (i = 0, j = 0; i < platform_current.occupied_count; i++) {
		struct platform_reserved_range *entry = &platform_current.occupied[i];

		if (j && entry->base <= platform_current.occupied[j - 1].base +
					   platform_current.occupied[j - 1].size) {
			unsigned long end = entry->base + entry->size;
			unsigned long previous_end = platform_current.occupied[j - 1].base +
						     platform_current.occupied[j - 1].size;

			if (end > previous_end)
					platform_current.occupied[j - 1].size =
					end - platform_current.occupied[j - 1].base;
		} else {
			platform_current.occupied[j++] = *entry;
		}
	}
	platform_current.occupied_count = j;
}

static void add_free_range(unsigned long base, unsigned long end)
{
	struct platform_reserved_range *range;

	base = align_up(base, PAGE_SIZE);
	end = align_down(end, PAGE_SIZE);
	if (end <= base)
		return;
	if (platform_current.free_count >= PLATFORM_MAX_MEMORY_RANGES) {
		platform_current.memory_map_overflow++;
		return;
	}
	range = &platform_current.free[platform_current.free_count++];
	range->base = base;
	range->size = end - base;
	if (range->size > platform_current.candidate_pool_size) {
		platform_current.candidate_pool_base = base;
		platform_current.candidate_pool_size = range->size;
	}
}

static void build_memory_map(void)
{
	unsigned long cursor = platform_current.memory_base;
	unsigned long memory_end = cursor + platform_current.memory_size;
	unsigned int i;

	platform_current.image_base = VIRT_DRAM_BASE + 0x80000UL;
	platform_current.image_size = (unsigned long)image_end - platform_current.image_base;
	if (platform_current.dtb_valid)
		platform_current.dtb_size = be32((const void *)(platform_current.dtb + 4));

	add_occupied_range(platform_current.image_base, platform_current.image_size,
			   PLATFORM_MEMORY_IMAGE);
	add_occupied_range(LOW_MEMORY - MAX_CPUS * THREAD_SIZE,
			   MAX_CPUS * THREAD_SIZE, PLATFORM_MEMORY_STACK);
	if (platform_current.dtb_valid)
		add_occupied_range(platform_current.dtb, platform_current.dtb_size,
				   PLATFORM_MEMORY_DTB);
	for (i = 0; i < platform_current.reserved_count; i++)
		add_occupied_range(platform_current.reserved[i].base,
				   platform_current.reserved[i].size,
				   PLATFORM_MEMORY_FIRMWARE);
	normalize_occupied_ranges();

	for (i = 0; i < platform_current.occupied_count; i++) {
		struct platform_reserved_range *range = &platform_current.occupied[i];

		add_free_range(cursor, range->base);
		cursor = range->base + range->size;
	}
	add_free_range(cursor, memory_end);
}

static void validate_reserved_memory(void)
{
	unsigned int i;

	build_memory_map();
	platform_current.reserved_config_valid =
		platform_current.dtb_valid && !platform_current.reserved_overflow &&
		!platform_current.memory_map_overflow;
	platform_current.page_pool_conflict = 0;
	for (i = 0; i < platform_current.reserved_count; i++) {
		struct platform_reserved_range *range = &platform_current.reserved[i];

		if (ranges_overlap(range->base, range->size,
				   LOW_MEMORY, PAGING_MEMORY))
			platform_current.page_pool_conflict = 1;
	}
	platform_current.usable_memory_base = platform_current.candidate_pool_base;
	platform_current.usable_memory_size = platform_current.candidate_pool_size;
	platform_current.candidate_pool_covers_fixed =
		platform_current.candidate_pool_base <= LOW_MEMORY &&
		platform_current.candidate_pool_base + platform_current.candidate_pool_size >=
			HIGH_MEMORY;
	if (platform_current.page_pool_conflict)
		platform_current.reserved_config_valid = 0;
	platform_current.candidate_pool_safe =
		platform_current.reserved_config_valid &&
		platform_current.candidate_pool_covers_fixed &&
		!platform_current.page_pool_conflict;
	platform_current.page_pool_plan_safe =
		platform_current.dtb_valid &&
		!platform_current.reserved_overflow &&
		platform_current.memory_map_overflow == 0;
}

static int fdt_parse(unsigned long dtb)
{
	const unsigned char *base = (const unsigned char *)dtb;
	const unsigned char *structs;
	const unsigned char *strings;
	unsigned int totalsize, struct_off, strings_off, reserve_off;
	unsigned int struct_size, strings_size;
	unsigned int address_cells = 2, size_cells = 1;
	unsigned int reserved_address_cells = 2, reserved_size_cells = 1;
	unsigned long cursor;
	int reserve_terminated = 0;
	int depth = -1;
	int reserved_depth = -1;
	int reserved_child = 0;
	int memory_node = 0;
	int cpu_node = 0;
	int uart_node = 0;
	int gic_node = 0;
	int its_node = 0;
	int timer_node = 0;

	if (!dtb || be32(base) != FDT_MAGIC)
		return -1;

	totalsize = be32(base + 4);
	struct_off = be32(base + 8);
	strings_off = be32(base + 12);
	reserve_off = be32(base + 16);
	struct_size = be32(base + 36);
	strings_size = be32(base + 32);
	if (totalsize < 40 || struct_off >= totalsize || strings_off >= totalsize ||
	    reserve_off >= totalsize || struct_size > totalsize - struct_off ||
	    strings_size > totalsize - strings_off)
		return -1;

	cursor = reserve_off;
	while (cursor + 16 <= totalsize) {
		unsigned long address = be64(base + cursor);
		unsigned long size = be64(base + cursor + 8);

		cursor += 16;
		if (!address && !size) {
			reserve_terminated = 1;
			break;
		}
		add_reserved_range(address, size);
	}
	if (!reserve_terminated)
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
			if (depth == 1 && text_has(name, len, "reserved-memory"))
				reserved_depth = depth;
			reserved_child = reserved_depth >= 0 && depth == reserved_depth + 1;
			memory_node = text_has(name, len, "memory");
			cpu_node = depth >= 1 && text_has(name, len, "cpu");
			uart_node = text_has(name, len, "pl011");
			gic_node = text_has(name, len, "intc");
			its_node = text_has(name, len, "its");
			timer_node = depth == 1 && text_has(name, len, "timer");
			continue;
		}
		if (token == FDT_END_NODE) {
			if (depth == reserved_depth)
				reserved_depth = -1;
			depth--;
			reserved_child = reserved_depth >= 0 && depth == reserved_depth + 1;
			memory_node = 0;
			cpu_node = 0;
			uart_node = 0;
			gic_node = 0;
			its_node = 0;
			timer_node = 0;
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

			if (gic_node && !strcmp(prop, FDT_COMPATIBLE) &&
			    compatible_has(value, len, "arm,gic-v3"))
				platform_current.gicv3_discovered = 1;
			if (its_node && !strcmp(prop, FDT_COMPATIBLE) &&
			    compatible_has(value, len, "arm,gic-v3-its"))
				platform_current.gits_compatible = 1;
			if (timer_node && !strcmp(prop, FDT_COMPATIBLE) &&
			    compatible_has(value, len, "arm,armv8-timer"))
				platform_current.timer_discovered = 1;
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
			if (depth == reserved_depth && !strcmp(prop, "#address-cells") && len == 4)
				reserved_address_cells = be32(value);
			if (depth == reserved_depth && !strcmp(prop, "#size-cells") && len == 4)
				reserved_size_cells = be32(value);
			if (reserved_child && !strcmp(prop, FDT_REG) &&
			    len >= (reserved_address_cells + reserved_size_cells) * 4) {
				unsigned int tuple_cells = reserved_address_cells + reserved_size_cells;
				unsigned int offset;

				for (offset = 0; offset + tuple_cells * 4 <= len;
				     offset += tuple_cells * 4)
					add_reserved_range(
						read_cells(value + offset, reserved_address_cells),
						read_cells(value + offset + reserved_address_cells * 4,
							   reserved_size_cells));
			}
			if (depth >= 2 && cpu_node && !strcmp(prop, FDT_DEVICE_TYPE) &&
			    text_has((const char *)value, len, "cpu"))
				platform_current.cpu_count++;
			if (depth >= 1 && uart_node && !strcmp(prop, FDT_REG) &&
			    len >= (address_cells + size_cells) * 4) {
				platform_current.uart_base = read_cells(value, address_cells);
				platform_current.uart_discovered = 1;
			}
			if (depth >= 1 && gic_node && !strcmp(prop, FDT_REG) &&
			    len >= (address_cells + size_cells) * 4) {
				unsigned int tuple_cells = address_cells + size_cells;
				unsigned int tuples = len / (tuple_cells * 4);

				platform_current.gicd_base = read_cells(value, address_cells);
				platform_current.gicd_size =
					read_cells(value + address_cells * 4, size_cells);
				if (tuples > 1) {
					platform_current.gicr_base =
						read_cells(value + tuple_cells * 4, address_cells);
					platform_current.gicr_size =
						read_cells(value + (tuple_cells + address_cells) * 4,
							   size_cells);
					platform_current.gicr_discovered = 1;
				}
				platform_current.gic_discovered = 1;
			}
			if (gic_node && !strcmp(prop, FDT_REDIST_STRIDE) &&
			    len >= 4)
				platform_current.gicr_stride = read_cells(value, 1);
			if (its_node && !strcmp(prop, FDT_REG) &&
			    len >= (address_cells + size_cells) * 4) {
				platform_current.gits_base = read_cells(value, address_cells);
				platform_current.gits_size =
					read_cells(value + address_cells * 4, size_cells);
				platform_current.gits_discovered = 1;
			}
			if (timer_node && !strcmp(prop, FDT_INTERRUPTS) &&
			    len >= 4 * TIMER_IRQ_CELLS * 4) {
				unsigned int virt_offset = TIMER_VIRT_INDEX * TIMER_IRQ_CELLS * 4;
				unsigned int hyp_offset = TIMER_HYP_INDEX * TIMER_IRQ_CELLS * 4;

				if (be32(value + virt_offset) == GIC_PPI_TYPE &&
				    be32(value + hyp_offset) == GIC_PPI_TYPE) {
					platform_current.timer_virt_ppi =
						GIC_PPI_BASE + be32(value + virt_offset + 4);
					platform_current.timer_hyp_ppi =
						GIC_PPI_BASE + be32(value + hyp_offset + 4);
					platform_current.timer_interrupts_discovered = 1;
				}
			}

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
				platform_current.memory_discovered = 1;
			}
			cursor = align4(cursor + len);
			continue;
		}
		return -1;
	}
	return 0;
}

unsigned long platform_uart_base(void)
{
	return platform_current.uart_base;
}

unsigned long platform_gicd_base(void)
{
	return platform_current.gicd_base;
}

unsigned long platform_gicr_base(int cpu_id)
{
	return platform_current.gicr_base +
	       (unsigned long)cpu_id * platform_current.gicr_stride;
}

unsigned long platform_gits_base(void)
{
	return platform_current.gits_base;
}

unsigned int platform_timer_hyp_ppi(void)
{
	return platform_current.timer_hyp_ppi;
}

unsigned int platform_timer_virt_ppi(void)
{
	return platform_current.timer_virt_ppi;
}

int platform_runtime_config_valid(void)
{
	return platform_current.dtb_valid && platform_current.kind == PLATFORM_KIND_QEMU_VIRT &&
	       platform_current.memory_config_valid && platform_current.mmio_config_valid;
}

int platform_memory_config_valid(void)
{
	return platform_current.memory_config_valid;
}

int platform_mmio_config_valid(void)
{
	return platform_current.mmio_config_valid;
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
	platform_current.gicd_size = 0x10000UL;
	platform_current.gicr_base = VIRT_GICR_BASE;
	platform_current.gicr_size = MAX_CPUS * VIRT_GICR_STRIDE;
	platform_current.gicr_stride = VIRT_GICR_STRIDE;
	platform_current.gits_base = VIRT_GITS_BASE;
	platform_current.gits_size = VIRT_GITS_SIZE;
	platform_current.timer_hyp_ppi = 26;
	platform_current.timer_virt_ppi = 27;
	platform_current.memory_config_valid = 0;
	platform_current.mmio_config_valid = 0;
	platform_current.cpu_count = 0;
	platform_current.dtb_valid = fdt_parse(platform_current.dtb) == 0;
	if (platform_current.dtb_valid && !platform_current.cpu_count)
		platform_current.cpu_count = MAX_CPUS;
	validate_reserved_memory();
	if (platform_current.dtb_valid && platform_current.kind == PLATFORM_KIND_QEMU_VIRT) {
		platform_current.memory_config_valid =
			platform_current.memory_discovered &&
			platform_current.memory_base == VIRT_DRAM_BASE &&
			platform_current.memory_size == PHYS_MEMORY_SIZE &&
			platform_current.reserved_config_valid;
		platform_current.mmio_config_valid =
			platform_current.uart_discovered &&
			platform_current.gic_discovered &&
			platform_current.gicv3_discovered &&
			platform_current.gicr_discovered &&
			platform_current.gits_discovered &&
			platform_current.gits_compatible &&
			platform_current.timer_discovered &&
			platform_current.timer_interrupts_discovered &&
			platform_current.gicr_stride >= 0x20000UL &&
			platform_current.gicr_size >=
				platform_current.cpu_count * platform_current.gicr_stride &&
			platform_current.gits_size >= GITS_TRANSLATER_END &&
			platform_current.timer_hyp_ppi == 26 &&
			platform_current.timer_virt_ppi == 27 &&
			range_within(platform_current.uart_base, 1, VIRT_MMIO_BASE,
				     VIRT_MMIO_SIZE) &&
			range_within(platform_current.gicd_base,
				     platform_current.gicd_size, VIRT_MMIO_BASE,
				     VIRT_MMIO_SIZE) &&
			range_within(platform_current.gicr_base,
				     platform_current.gicr_size, VIRT_MMIO_BASE,
				     VIRT_MMIO_SIZE) &&
			range_within(platform_current.gits_base,
				     platform_current.gits_size, VIRT_MMIO_BASE,
				     VIRT_MMIO_SIZE);
		if (platform_current.memory_config_valid && platform_current.mmio_config_valid) {
			virt_uart_base = VA_START + platform_current.uart_base;
			virt_gicd_base = VA_START + platform_current.gicd_base;
			virt_gicr_base = VA_START + platform_current.gicr_base;
			virt_gits_base = VA_START + platform_current.gits_base;
		}
	}

	if (!platform_runtime_config_valid()) {
		platform_current.cpu_count = platform_current.cpu_count ?: MAX_CPUS;
		platform_current.memory_base = VIRT_DRAM_BASE;
		platform_current.memory_size = PHYS_MEMORY_SIZE;
		platform_current.uart_base = VIRT_UART_BASE;
		platform_current.gicd_base = VIRT_GICD_BASE;
		platform_current.gicd_size = 0x10000UL;
		platform_current.gicr_base = VIRT_GICR_BASE;
		platform_current.gicr_size = MAX_CPUS * VIRT_GICR_STRIDE;
		platform_current.gicr_stride = VIRT_GICR_STRIDE;
		platform_current.gits_base = VIRT_GITS_BASE;
		platform_current.gits_size = VIRT_GITS_SIZE;
		platform_current.timer_hyp_ppi = 26;
		platform_current.timer_virt_ppi = 27;
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
	printf("[platform] gicr-size=%p stride=%p its=%p+%p timer-ppi=%u/%u\n",
	       (void *)platform_current.gicr_size,
	       (void *)platform_current.gicr_stride,
	       (void *)platform_current.gits_base,
	       (void *)platform_current.gits_size,
	       platform_current.timer_hyp_ppi,
	       platform_current.timer_virt_ppi);
	printf("[platform] reserved=%u overflow=%u pool-conflict=%s usable=%p+%luMB\n",
	       platform_current.reserved_count,
	       platform_current.reserved_overflow,
	       platform_current.page_pool_conflict ? "yes" : "no",
	       (void *)platform_current.usable_memory_base,
	       platform_current.usable_memory_size >> 20);
	printf("[platform] memory-map occupied=%u free=%u overflow=%u image=%p+%luKB dtb=%p+%luKB\n",
	       platform_current.occupied_count,
	       platform_current.free_count,
	       platform_current.memory_map_overflow,
	       (void *)platform_current.image_base,
	       platform_current.image_size >> 10,
	       (void *)platform_current.dtb,
	       platform_current.dtb_size >> 10);
	printf("[platform] candidate=%p+%luMB covers-fixed=%s safe=%s\n",
	       (void *)platform_current.candidate_pool_base,
	       platform_current.candidate_pool_size >> 20,
	       platform_current.candidate_pool_covers_fixed ? "yes" : "no",
	       platform_current.candidate_pool_safe ? "yes" : "no");
	printf("[platform] memory-config=%s mmio-config=%s runtime=%s\n",
	       platform_current.memory_config_valid ? "valid" : "fallback",
	       platform_current.mmio_config_valid ? "valid" : "fallback",
	       platform_runtime_config_valid() ? "enabled" : "fixed-layout");
	if (platform_current.kind == PLATFORM_KIND_AGX_ORIN)
		printf("[platform] AGX Orin recognized: hardware drivers not enabled\n");
}
