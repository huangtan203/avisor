// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor
 *
 * A Tiny Hypervisor for IoT Development
 *
 * Copyright (c) 2022 Deng Jie (mr.dengjie@gmail.com).
 */

#pragma once

/* ========== 通用宏定义（所有平台共享） ========== */

#define PAGE_MASK     0xfffffffffffff000
#define PAGE_SHIFT    12
#define TABLE_SHIFT   9
#define SECTION_SHIFT (PAGE_SHIFT + TABLE_SHIFT)

#define PAGE_SIZE    (1 << PAGE_SHIFT)
#define SECTION_SIZE (1 << SECTION_SHIFT)

#define PTRS_PER_TABLE (1 << TABLE_SHIFT)

#define PGD_SHIFT (PAGE_SHIFT + 3 * TABLE_SHIFT)
#define PUD_SHIFT (PAGE_SHIFT + 2 * TABLE_SHIFT)
#define PMD_SHIFT (PAGE_SHIFT + TABLE_SHIFT)

#define LV1_SHIFT (PAGE_SHIFT + 2 * TABLE_SHIFT)
#define LV2_SHIFT (PAGE_SHIFT + TABLE_SHIFT)

#define PG_DIR_SIZE (5 * PAGE_SIZE)

/*
 * ========== 平台相关的内存布局定义 ==========
 *
 * 学习要点：
 *   不同 ARM 平台的内存布局完全不同：
 *   - 树莓派 3B: RAM 从 0x0 开始，外设从 0x3F000000 开始
 *   - QEMU virt: RAM 从 0x40000000 开始，外设从 0x08000000 开始
 *   - AGX Orin:   RAM 从 0x80000000 开始（具体看设备树）
 *
 *   所以 Hypervisor 需要为每个平台定义自己的内存布局。
 *   平台相关的宏包括：
 *     VA_START, PHYS_MEMORY_SIZE, LOW_MEMORY, HIGH_MEMORY,
 *     PAGING_MEMORY, PAGING_PAGES, DEVICE_BASE, PBASE 等
 */
#ifdef PLATFORM_VIRT
#include "boards/virt/virt_mm.h"
#else
#include "boards/raspi/base.h"

#define VA_START 0x0000000000000000

#define PHYS_MEMORY_SIZE 0x40000000

#define LOW_MEMORY  (2 * SECTION_SIZE)
#define HIGH_MEMORY DEVICE_BASE

#define PAGING_MEMORY (HIGH_MEMORY - LOW_MEMORY)
#define PAGING_PAGES  (PAGING_MEMORY / PAGE_SIZE)
#endif

#ifndef __ASSEMBLER__
#include "sched.h"
#include "types.h"
#include <inttypes.h>

/*
 * 注意：在 PLATFORM_VIRT 模式下，
 * paddr_t/vaddr_t/TO_VADDR/TO_PADDR 等在 boards/virt/virt_mm.h 中定义。
 * 这里只定义非 virt 平台的版本（即原始 raspi 版本）。
 */
#ifndef PLATFORM_VIRT
typedef uint64_t paddr_t;
typedef uint64_t vaddr_t;

#define TO_VADDR(pa) ((vaddr_t)pa + VA_START)
#define TO_PADDR(pa) ((paddr_t)pa - VA_START)
#endif

#define PAGE_POOL_MAX_RANGES 8

struct page_pool_range {
	paddr_t start_addr;
	uint64_t page_nr;
	uint8_t *memap;
	uint64_t last_page_id;
};

struct page_pool {
	paddr_t start_addr;
	uint64_t page_nr;
	uint8_t *memap;
	uint64_t last_page_id;
	struct page_pool_range ranges[PAGE_POOL_MAX_RANGES];
	unsigned int range_count;
	unsigned int next_range_id;
};

void map_stage2_page(struct task_struct *task, vaddr_t va, paddr_t page,
		     uint64_t flags);
int unmap_stage2_page(struct task_struct *task, vaddr_t va, int do_free);
int mprotect_stage2_page(struct task_struct *task, vaddr_t va, uint64_t new_ap);

/* Phase 22: 2MB 大页（block mapping）接口 */
int map_stage2_block(struct task_struct *task, vaddr_t va, paddr_t pa,
		     uint64_t flags);
int unmap_stage2_block(struct task_struct *task, vaddr_t va, int do_free);
int mprotect_stage2_block(struct task_struct *task, vaddr_t va, uint64_t new_ap);

/* Phase 22: VM 内存配额 */
int set_task_memory_quota(struct task_struct *task, unsigned long max_bytes);
unsigned long get_task_memory_usage(struct task_struct *task);
int quota_check(struct task_struct *task, unsigned long add_bytes);

/* Phase 22: 连续物理页分配（用于 2MB 大页） */
paddr_t allocate_block_pages(int npages);
void free_block_pages(paddr_t block_pa, int npages);

/* Phase 23: 页面回收（Page Reclaim） */
/* Access Flag Fault 处理：设置 AF 位，标记页面已访问 */
int handle_access_flag_fault(struct task_struct *task, vaddr_t ipa);

/* 大页拆分：将 2MB block 拆成 512 个 4KB page */
int split_stage2_block(struct task_struct *task, vaddr_t va);

/* 页面回收：尝试回收 nr_pages 个页面，返回实际回收数量 */
int reclaim_pages(struct task_struct *task, int nr_pages);

/* 回收统计查询 */
int get_reclaim_stats(struct task_struct *task, int *reclaim_count,
		      int *reclaim_fail_count);

/*
 * Phase 34: 销毁 VM 的全部 Stage-2 映射与物理页。
 *
 * 遍历 L1->L2->L3 全表，释放：
 *   - 普通用户页（4KB / 2MB 大页）
 *   - L2/L3 页表页
 *   - first_table 根页
 *
 * 共享内存（ivshmem shmem / 跨 VM shm / KSM 共享页）不在此释放，
 * 由各自的 detach/drop 接口在回收编排中先行处理。
 * 返回释放的总页数（普通用户页 + 页表页，4KB 计），-1 失败。
 */
int destroy_vm_pages(struct task_struct *task);

void *allocate_page(void);
void deallocate_page(void *);
void *allocate_task_page(struct task_struct *task, vaddr_t va);
bool check_task_page_mapped(struct task_struct *task, vaddr_t va);
void set_task_page_notaccessable(struct task_struct *task, vaddr_t va);
int handle_mem_abort(vaddr_t addr, uint64_t esr);

extern paddr_t pg_dir;

#endif
