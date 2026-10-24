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
	unsigned long gicr_base;
	int dtb_valid;
};

extern struct platform_info platform_current;
extern unsigned long platform_dtb_address;
extern unsigned long platform_dtb_address_alt;

void platform_early_probe(void);
const char *platform_kind_name(enum platform_kind kind);
unsigned long platform_uart_base(void);
unsigned long platform_gicd_base(void);
unsigned long platform_gicr_base(int cpu_id);
int platform_runtime_config_valid(void);
