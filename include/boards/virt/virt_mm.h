// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - QEMU virt 平台内存布局定义
 *
 * 学习要点：
 *   注意这里和树莓派版本的关键区别：
 *   - 树莓派：DRAM 从 0x0 开始，外设从 0x3F000000 开始
 *   - QEMU virt：DRAM 从 0x40000000 开始，外设从 0x08000000 开始
 *   这就是"平台差异"最直观的体现。
 *
 * 内存布局（物理地址）：
 *   0x4000_0000  --+
 *                  |   Hypervisor 代码段 + 数据段 + BSS + 页表 + 栈
 *   LOW_MEMORY   --+  (~16MB 位置)
 *                  |
 *                  |   可分配页面池 (page pool)
 *                  |
 *   HIGH_MEMORY  --+  (~1GB 位置，DRAM 结束)
 *
 *   0x0800_0000  --+  MMIO 外设区域 (GIC, UART, RTC 等)
 *   0x0A00_0000  --+
 */

#pragma once

#include "boards/virt/base.h"

/* 虚拟地址起始（等值映射，VA = PA，简单清晰） */
#define VA_START            0x0000000000000000

/* 物理内存大小（先映射 1GB，QEMU 默认就是 1GB） */
#define PHYS_MEMORY_SIZE    0x40000000UL   /* 1 GB */

/*
 * 低内存边界：代码 + 数据 + BSS + 页表 + 初始栈之后
 * 留 8 个 2MB 段（16MB）给 Hypervisor 自身使用
 */
#define LOW_MEMORY          (VIRT_DRAM_BASE + 8 * SECTION_SIZE)

/* 高内存边界：DRAM 结束位置 */
#define HIGH_MEMORY         (VIRT_DRAM_BASE + PHYS_MEMORY_SIZE)

/* 可分页内存总量和页数 */
#define PAGING_MEMORY       (HIGH_MEMORY - LOW_MEMORY)
#define PAGING_PAGES        (PAGING_MEMORY / PAGE_SIZE)

#ifndef __ASSEMBLER__
#include "common/sched.h"
#include "common/types.h"
#include <inttypes.h>

typedef uint64_t paddr_t;
typedef uint64_t vaddr_t;

/* 地址转换（等值映射，VA = PA） */
#define TO_VADDR(pa)        ((vaddr_t)(pa) + VA_START)
#define TO_PADDR(va)        ((paddr_t)(va) - VA_START)

/* 函数声明（这些由平台的 mm.c 实现） */
void map_stage2_page(struct task_struct *task, vaddr_t va, paddr_t page,
                     uint64_t flags);
void *allocate_page(void);
void deallocate_page(void *);
void *allocate_task_page(struct task_struct *task, vaddr_t va);
bool check_task_page_mapped(struct task_struct *task, vaddr_t va);
void set_task_page_notaccessable(struct task_struct *task, vaddr_t va);
int handle_mem_abort(vaddr_t addr, uint64_t esr);

extern paddr_t pg_dir;

#endif /* __ASSEMBLER__ */
