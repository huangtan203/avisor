// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "common/types.h"

enum platform_kind {
	PLATFORM_KIND_UNKNOWN = 0,
	PLATFORM_KIND_QEMU_VIRT,
	PLATFORM_KIND_AGX_ORIN,
};

enum platform_uart_kind {
	PLATFORM_UART_UNKNOWN = 0,
	PLATFORM_UART_PL011,
	PLATFORM_UART_TEGRA_HSUART,
};

enum platform_psci_method {
	PLATFORM_PSCI_UNKNOWN = 0,
	PLATFORM_PSCI_SMC,
	PLATFORM_PSCI_HVC,
};

#define PLATFORM_MAX_RESERVED_RANGES 16
#define PLATFORM_MAX_MEMORY_RANGES 24
#define PLATFORM_MAX_MEMORY_BANKS 8

enum platform_memory_range_type {
	PLATFORM_MEMORY_IMAGE = 0,
	PLATFORM_MEMORY_STACK,
	PLATFORM_MEMORY_DTB,
	PLATFORM_MEMORY_FIRMWARE,
};

struct platform_reserved_range {
	unsigned long base;
	unsigned long size;
	enum platform_memory_range_type type;
};

struct platform_info {
	enum platform_kind kind;
	const char *name;
	unsigned long dtb;
	unsigned long cpu_count;
	unsigned long memory_base;
	unsigned long memory_size;
	struct platform_reserved_range memory_banks[PLATFORM_MAX_MEMORY_BANKS];
	unsigned int memory_bank_count;
	unsigned int memory_bank_overflow;
	unsigned long uart_base;
	unsigned long uart_size;
	enum platform_uart_kind uart_kind;
	unsigned long uart_reg_shift;
	unsigned long uart_clock_hz;
	unsigned long uart_baudrate;
	int uart_compatible;
	unsigned long gicd_base;
	unsigned long gicd_size;
	unsigned long gicr_base;
	unsigned long gicr_size;
	unsigned long gicr_stride;
	unsigned long gits_base;
	unsigned long gits_size;
	unsigned int timer_hyp_ppi;
	unsigned int timer_virt_ppi;
	enum platform_psci_method psci_method;
	int psci_discovered;
	int interrupt_config_valid;
	struct platform_reserved_range reserved[PLATFORM_MAX_RESERVED_RANGES];
	unsigned int reserved_count;
	unsigned int reserved_overflow;
	struct platform_reserved_range occupied[PLATFORM_MAX_MEMORY_RANGES];
	unsigned int occupied_count;
	struct platform_reserved_range free[PLATFORM_MAX_MEMORY_RANGES];
	unsigned int free_count;
	unsigned int memory_map_overflow;
	unsigned long image_base;
	unsigned long image_size;
	unsigned long dtb_size;
	unsigned long candidate_pool_base;
	unsigned long candidate_pool_size;
	int candidate_pool_covers_fixed;
	int candidate_pool_safe;
	int page_pool_plan_safe;
	unsigned long usable_memory_base;
	unsigned long usable_memory_size;
	int reserved_config_valid;
	int page_pool_conflict;
	int dtb_valid;
	int memory_config_valid;
	int mmio_config_valid;
	int memory_discovered;
	int uart_discovered;
	int gic_discovered;
	int gicv3_discovered;
	int gicr_discovered;
	int gits_discovered;
	int gits_compatible;
	int timer_discovered;
	int timer_interrupts_discovered;
	unsigned int runtime_table_count;
	int runtime_mapping_valid;
};

extern struct platform_info platform_current;
extern unsigned long platform_dtb_address;
extern unsigned long platform_dtb_address_alt;
extern unsigned long platform_entry_el;
extern unsigned long platform_dtb_source;
extern unsigned long platform_cpu_mpidr[];

void platform_early_probe(void);
const char *platform_kind_name(enum platform_kind kind);
unsigned long platform_uart_base(void);
unsigned long platform_gicd_base(void);
unsigned long platform_gicr_base(int cpu_id);
unsigned long platform_gits_base(void);
unsigned int platform_timer_hyp_ppi(void);
unsigned int platform_timer_virt_ppi(void);
int platform_runtime_config_valid(void);
int platform_full_bringup_supported(void);
int platform_interrupt_config_valid(void);
int platform_psci_cpu_on(unsigned long mpidr, unsigned long entry_point,
			 unsigned long context_id);
int platform_memory_config_valid(void);
int platform_mmio_config_valid(void);
int platform_apply_runtime_mappings(void);
