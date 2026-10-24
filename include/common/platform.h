// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "common/types.h"

enum platform_kind {
	PLATFORM_KIND_UNKNOWN = 0,
	PLATFORM_KIND_QEMU_VIRT,
	PLATFORM_KIND_AGX_ORIN,
};

struct platform_info {
	enum platform_kind kind;
	const char *name;
	unsigned long dtb;
	unsigned long cpu_count;
	unsigned long memory_base;
	unsigned long memory_size;
	unsigned long uart_base;
	unsigned long gicd_base;
	unsigned long gicd_size;
	unsigned long gicr_base;
	unsigned long gicr_size;
	unsigned long gicr_stride;
	unsigned long gits_base;
	unsigned long gits_size;
	unsigned int timer_hyp_ppi;
	unsigned int timer_virt_ppi;
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
};

extern struct platform_info platform_current;
extern unsigned long platform_dtb_address;
extern unsigned long platform_dtb_address_alt;
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
int platform_memory_config_valid(void);
int platform_mmio_config_valid(void);
