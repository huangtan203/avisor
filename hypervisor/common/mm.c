// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor
 *
 * A Tiny Hypervisor for IoT Development
 *
 * Copyright (c) 2022 Deng Jie (mr.dengjie@gmail.com).
 */

#include "common/mm.h"
#include "arch/aarch64/mmu.h"
#include "arch/aarch64/sysregs.h"
#include "common/board.h"
#include "common/debug.h"
#include "common/task.h"
#include "common/utils.h"

/*
 * 平台相关的页池（page pool）获取函数
 *
 * 学习要点：
 *   不同平台的内存布局不同，可用内存的起始地址和大小也不同。
 *   我们通过一个统一的 get_page_pool() 接口来获取当前平台的页池，
 *   这样通用的内存管理代码就不需要知道具体平台的细节了。
 *
 *   这是"依赖倒置原则"（DIP）的一个简单应用：
 *   高层模块（通用 mm 代码）不依赖底层模块（平台具体实现），
 *   两者都依赖抽象（page_pool 接口）。
 */
#ifndef PLATFORM_VIRT
#include "boards/raspi/raspi3b.h"
static inline struct page_pool *get_page_pool(void)
{
    return get_page_pool();
}
#else
#include "boards/virt/virt_board.h"
static inline struct page_pool *get_page_pool(void)
{
    return get_virt_page_pool();
}
#endif

paddr_t get_free_page(struct page_pool *pool);
void free_page(struct page_pool *pool, paddr_t p);

/*
 * 页池自旋锁（SMP）
 *
 * 两个物理核可能并发进入 demand paging（各自的缺页异常都会
 * 调 allocate_task_page），不锁住的扫描/置位 memap 会把同一
 * 物理页发给两个 VM——内存隔离直接被破坏。
 *
 * 中断路径不会分配页面，锁内无需关中断。
 */
static volatile unsigned int page_pool_lock = 0;

static inline void page_pool_lock_acquire(void)
{
	unsigned int ok;

	asm volatile(
		"1: ldaxr w0, [%1]\n"
		"   cbnz w0, 1b\n"       /* 已锁：自旋等释放 */
		"   mov  w0, #1\n"
		"   stxr %w0, w0, [%1]\n" /* 独占写 1，状态写入 %0 */
		"   cbnz %w0, 1b\n"      /* 独占失败：重试 */
		: "=&r"(ok)
		: "r"(&page_pool_lock)
		: "x0", "memory");
}

static inline void page_pool_lock_release(void)
{
	/* stlr 带 release 语义，保证锁内写入先于解锁可见 */
	asm volatile("stlr wzr, [%0]" ::"r"(&page_pool_lock) : "memory");
}

void *allocate_page()
{
	paddr_t page;

	page_pool_lock_acquire();
	page = get_free_page(get_page_pool());
	page_pool_lock_release();

	if (page == 0) {
		return 0;
	}

	return (void *)TO_VADDR(page);
}

void deallocate_page(void *page)
{
	page_pool_lock_acquire();
	free_page(get_page_pool(), TO_PADDR(page));
	page_pool_lock_release();
}

void *allocate_task_page(struct task_struct *task, vaddr_t va)
{
	paddr_t page;

	page_pool_lock_acquire();
	page = get_free_page(get_page_pool());
	page_pool_lock_release();

	if (page == 0) {
		return 0;
	}

	map_stage2_page(task, va, page, MMU_STAGE2_PAGE_FLAGS);
	return (void *)TO_VADDR(page);
}

void set_task_page_notaccessable(struct task_struct *task, vaddr_t va)
{
	map_stage2_page(task, va, 0, MMU_STAGE2_MMIO_PAGE_FLAGS);
}

paddr_t get_free_page(struct page_pool *pool)
{
	uint64_t i, index;
	paddr_t page;

	for (i = pool->last_page_id; i < (pool->last_page_id + pool->page_nr);
	     i++) {
		index = i % pool->page_nr;
		if (pool->memap[index] == 0) {
			pool->memap[index] = 1;
			page = pool->start_addr + index * PAGE_SIZE;
			memzero((void *)TO_VADDR(page), PAGE_SIZE);
			pool->last_page_id = index;
			return page;
		}
	}

	PANIC("no free pages!\n");
	return 0;
}

void free_page(struct page_pool *pool, paddr_t p)
{
	unsigned long idx = (p - pool->start_addr) / PAGE_SIZE;
	if (p < pool->start_addr || idx >= pool->page_nr) {
		PANIC("free_page: invalid pa 0x%lx (start=0x%lx nr=%lu idx=%lu)",
		      p, pool->start_addr, pool->page_nr, idx);
	}
	pool->memap[idx] = 0;
}

void map_stage2_table_entry(vaddr_t pte, vaddr_t va, paddr_t pa, uint64_t flags)
{
	uint64_t index = va >> PAGE_SHIFT;

	index = index & (PTRS_PER_TABLE - 1);
	uint64_t entry = pa | flags;

	((uint64_t *)pte)[index] = entry;
}

static paddr_t map_stage2_table(vaddr_t table, uint64_t shift, vaddr_t va,
			 int *new_table)
{
	uint64_t index = va >> shift;

	index = index & (PTRS_PER_TABLE - 1);

	if (!((uint64_t *)table)[index]) {
		*new_table = 1;
		paddr_t next_level_table =
			get_free_page(get_page_pool());
		/*
		 * Stage-2 table descriptor 也需要 AF 位，否则首次访问
		 * 该表下的页面时会触发 table level AF fault。
		 * QEMU 可能把 table-level AF fault 误报为 translation fault。
		 */
		uint64_t entry = next_level_table | MM_TYPE_PAGE_TABLE |
				 MM_STAGE2_ACCESS;

		/* 清零新分配的页表页，避免垃圾数据导致异常 */
		memset((void *)TO_VADDR(next_level_table), 0, PAGE_SIZE);

		((uint64_t *)table)[index] = entry;
		return next_level_table;
	} else {
		*new_table = 0;
		/* 兼容旧的 table entry：确保 AF 位已设置 */
		uint64_t *ep = &((uint64_t *)table)[index];
		if ((*ep & MM_STAGE2_ACCESS) == 0)
			*ep |= MM_STAGE2_ACCESS;
	}

	return ((uint64_t *)table)[index] & PAGE_MASK;
}

bool check_task_page_mapped(struct task_struct *task, vaddr_t va)
{
	paddr_t lv1_table;

	if (!task->mm.first_table) {
		task->mm.first_table = get_free_page(get_page_pool());
		memset((void *)TO_VADDR(task->mm.first_table), 0, PAGE_SIZE);
		task->mm.kernel_pages_count++;
	}

	lv1_table = task->mm.first_table;
	int new_table;
	paddr_t lv2_table = map_stage2_table(TO_VADDR(lv1_table), LV1_SHIFT, va,
					     &new_table);

	if (new_table) {
		task->mm.kernel_pages_count++;
		return false;
	}

	map_stage2_table(TO_VADDR(lv2_table), LV2_SHIFT, va, &new_table);

	if (new_table) {
		task->mm.kernel_pages_count++;
		return false;
	}

	return true;
}

void map_stage2_page(struct task_struct *task, vaddr_t va, paddr_t page,
		     uint64_t flags)
{
	paddr_t lv1_table;

	if (!task->mm.first_table) {
		task->mm.first_table = get_free_page(get_page_pool());
		memset((void *)TO_VADDR(task->mm.first_table), 0, PAGE_SIZE);
		task->mm.kernel_pages_count++;
	}

	lv1_table = task->mm.first_table;
	int new_table;
	paddr_t lv2_table = map_stage2_table(TO_VADDR(lv1_table), LV1_SHIFT, va,
					     &new_table);

	if (new_table) {
		task->mm.kernel_pages_count++;
	}

	paddr_t lv3_table = map_stage2_table(TO_VADDR(lv2_table), LV2_SHIFT, va,
					     &new_table);

	if (new_table) {
		task->mm.kernel_pages_count++;
	}

	map_stage2_table_entry(TO_VADDR(lv3_table), va, page, flags);
	task->mm.user_pages_count++;
}

/*
 * Phase 20: 判断一个页表页是否全空（所有 entry 都是 0）
 *
 * 用于页表自底向上回收：unmap 一个 L3 entry 后，如果整个 L3 表全空，
 * 则回收 L3 表页本身，并继续检查 L2 表是否也变空了。
 */
static int page_table_empty(paddr_t table_pa)
{
	uint64_t *table = (uint64_t *)TO_VADDR(table_pa);
	for (int i = 0; i < PTRS_PER_TABLE; i++) {
		if (table[i] != 0)
			return 0;
	}
	return 1;
}

/*
 * Phase 20: 通用 Stage-2 单页取消映射
 *
 *   task: 目标 VM task
 *   va:   目标 IPA/VA（4KB 页对齐）
 *   free_page: 1 = 释放映射的物理页（普通内存页），
 *              0 = 不释放物理页（MMIO 映射 / 共享内存）
 *
 * 返回 0 成功，-1 未映射
 *
 * 特性：
 *   - 支持页表自底向上回收：L3 全空则回收 L3 表页，
 *     L2 全空则回收 L2 表页（L1 永远不回收，它是根）
 *   - 自动 TLB flush（vmalle1）
 *   - 递减 user_pages_count 和 kernel_pages_count
 */
int unmap_stage2_page(struct task_struct *task, vaddr_t va, int do_free)
{
	paddr_t lv1_table, lv2_table_pa = 0, lv3_table_pa = 0;
	uint64_t entry;
	uint64_t *l3_entry_p;
	int idx1, idx2, idx3;

	if (!task || !task->mm.first_table)
		return -1;

	lv1_table = task->mm.first_table;
	va &= PAGE_MASK;

	/* Level 1 */
	idx1 = (va >> LV1_SHIFT) & (PTRS_PER_TABLE - 1);
	entry = ((uint64_t *)TO_VADDR(lv1_table))[idx1];
	if ((entry & 0x3) != 3)   /* 不是 table descriptor */
		return -1;
	lv2_table_pa = entry & PAGE_MASK;

	/* Level 2 */
	idx2 = (va >> LV2_SHIFT) & (PTRS_PER_TABLE - 1);
	entry = ((uint64_t *)TO_VADDR(lv2_table_pa))[idx2];
	if ((entry & 0x3) != 3)   /* 不是 table descriptor */
		return -1;
	lv3_table_pa = entry & PAGE_MASK;

	/* Level 3: 找到目标 entry */
	idx3 = (va >> PAGE_SHIFT) & (PTRS_PER_TABLE - 1);
	l3_entry_p = &((uint64_t *)TO_VADDR(lv3_table_pa))[idx3];
	entry = *l3_entry_p;
	if ((entry & 0x3) != 3)   /* 未映射或非 page */
		return -1;

	/* 清零 L3 entry（先清映射再释放物理页） */
	*l3_entry_p = 0;
	task->mm.user_pages_count--;

	/*
	 * 自底向上回收空的页表页
	 *
	 * 顺序：先回收 L3 表（如果全空），再回收 L2 表（如果全空）。
	 * L1 表（first_table）不回收——它是 VM 页表的根，
	 * 生命周期与 task 一致。
	 */

	/* 回收 L3 表页 */
	if (page_table_empty(lv3_table_pa)) {
		page_pool_lock_acquire();
		free_page(get_page_pool(), lv3_table_pa);
		page_pool_lock_release();
		task->mm.kernel_pages_count--;

		/* 清 L2 中指向该 L3 的 entry */
		((uint64_t *)TO_VADDR(lv2_table_pa))[idx2] = 0;

		/* 回收 L2 表页 */
		if (page_table_empty(lv2_table_pa)) {
			page_pool_lock_acquire();
			free_page(get_page_pool(), lv2_table_pa);
			page_pool_lock_release();
			task->mm.kernel_pages_count--;

			/* 清 L1 中指向该 L2 的 entry */
			((uint64_t *)TO_VADDR(lv1_table))[idx1] = 0;
		}
	}

	/* 失效 TLB（当前 VMID） */
	asm volatile("tlbi vmalle1" ::: "memory");
	asm volatile("dsb ish" ::: "memory");

	return 0;
}

/*
 * Phase 21: 修改 Stage-2 页表中某一页的访问权限
 *
 *   task:   目标 VM task
 *   va:     目标 IPA/VA（4KB 页对齐）
 *   new_ap: 新的权限值（S2_AP_RW / S2_AP_RO / S2_AP_NONE）
 *
 * 返回 0 成功，-1 未映射
 *
 * 说明：
 *   - 只修改 AP 位（bit[7:6]），其他属性不变
 *   - 修改后自动 TLB flush（vmalle1）
 *   - 不改变页表层级结构，不涉及物理页分配/释放
 */
int mprotect_stage2_page(struct task_struct *task, vaddr_t va, uint64_t new_ap)
{
	paddr_t lv1_table, lv2_table_pa, lv3_table_pa;
	uint64_t entry;
	int idx1, idx2, idx3;
	uint64_t *l3_entry_p;

	if (!task || !task->mm.first_table)
		return -1;

	lv1_table = task->mm.first_table;
	va &= PAGE_MASK;
	new_ap &= S2_AP_MASK;

	/* Level 1 */
	idx1 = (va >> LV1_SHIFT) & (PTRS_PER_TABLE - 1);
	entry = ((uint64_t *)TO_VADDR(lv1_table))[idx1];
	if ((entry & 0x3) != 3)
		return -1;
	lv2_table_pa = entry & PAGE_MASK;

	/* Level 2 */
	idx2 = (va >> LV2_SHIFT) & (PTRS_PER_TABLE - 1);
	entry = ((uint64_t *)TO_VADDR(lv2_table_pa))[idx2];
	if ((entry & 0x3) != 3)
		return -1;
	lv3_table_pa = entry & PAGE_MASK;

	/* Level 3 — 找到 L3 entry 并修改 AP 位 */
	idx3 = (va >> PAGE_SHIFT) & (PTRS_PER_TABLE - 1);
	l3_entry_p = &((uint64_t *)TO_VADDR(lv3_table_pa))[idx3];
	entry = *l3_entry_p;

	if ((entry & 0x3) != 0x3)  /* 不是 page descriptor */
		return -1;

	/* 清除旧的 AP 位，设置新的 AP 位 */
	entry &= ~S2_AP_MASK;
	entry |= new_ap;
	*l3_entry_p = entry;

	/* 失效 TLB（当前 VMID） */
	asm volatile("tlbi vmalle1" ::: "memory");
	asm volatile("dsb ish" ::: "memory");
	asm volatile("isb" ::: "memory");

	return 0;
}

/*
 * Phase 22: Stage-2 2MB Block 映射
 *
 *   task:  目标 VM task
 *   va:    目标 IPA/VA（必须 2MB 对齐）
 *   pa:    物理地址（必须 2MB 对齐）
 *   flags: 映射标志（用 MMU_STAGE2_BLOCK_FLAGS 或变体）
 *
 * 返回 0 成功，-1 失败（已存在映射或参数错误）
 *
 * 说明：
 *   - 在 L2 level 直接建立 block descriptor（bit[1:0] = 01）
 *   - 如果目标 L2 entry 已经是 table（指向 L3 表），则返回失败
 *     （不自动拆分，避免复杂性；调用方先 unmap 再 block map）
 *   - 不做 TLB flush（由调用方按需处理）
 */
int map_stage2_block(struct task_struct *task, vaddr_t va, paddr_t pa,
		     uint64_t flags)
{
	paddr_t lv1_table;
	uint64_t entry;
	int idx1, idx2;

	if (!task)
		return -1;

	/* 2MB 对齐检查 */
	if ((va & (SECTION_SIZE - 1)) || (pa & (SECTION_SIZE - 1)))
		return -1;

	if (!task->mm.first_table) {
		task->mm.first_table = get_free_page(get_page_pool());
		memset((void *)TO_VADDR(task->mm.first_table), 0, PAGE_SIZE);
		task->mm.kernel_pages_count++;
	}

	lv1_table = task->mm.first_table;

	/* Level 1 — 遍历到 L2 表 */
	idx1 = (va >> LV1_SHIFT) & (PTRS_PER_TABLE - 1);
	entry = ((uint64_t *)TO_VADDR(lv1_table))[idx1];

	paddr_t lv2_table_pa;

	if ((entry & 0x3) == 0) {
		/* L1 entry 为空，分配一个新的 L2 表 */
		lv2_table_pa = get_free_page(get_page_pool());
		if (!lv2_table_pa)
			return -1;
		((uint64_t *)TO_VADDR(lv1_table))[idx1] =
			lv2_table_pa | MM_TYPE_PAGE_TABLE;
		task->mm.kernel_pages_count++;
	} else if ((entry & 0x3) == 3) {
		/* 已是 table descriptor */
		lv2_table_pa = entry & PAGE_MASK;
	} else {
		/* L1 是 block（1GB），不能再放 L2 表 */
		return -1;
	}

	/* Level 2 — 检查目标 entry 是否可用 */
	idx2 = (va >> LV2_SHIFT) & (PTRS_PER_TABLE - 1);
	entry = ((uint64_t *)TO_VADDR(lv2_table_pa))[idx2];

	if ((entry & 0x3) != 0) {
		/* 已有映射（可能是 table 或 block），不覆盖 */
		return -1;
	}

	/* 写入 block descriptor */
	((uint64_t *)TO_VADDR(lv2_table_pa))[idx2] = pa | flags;
	task->mm.huge_pages_count++;

	return 0;
}

/*
 * Phase 22: Stage-2 2MB Block 取消映射
 *
 *   task:     目标 VM task
 *   va:       目标 IPA/VA（2MB 对齐）
 *   do_free:  1 = 释放 2MB 物理内存（512 页），0 = 不释放
 *
 * 返回 0 成功，-1 未映射或不是 block
 *
 * 说明：
 *   - 仅当 L2 entry 是 block descriptor 时才处理
 *   - do_free=1 时，释放连续 512 个 4KB 物理页
 *   - 如果 L2 表变空，回收 L2 表页（自底向上回收）
 *   - 自动 TLB flush
 */
int unmap_stage2_block(struct task_struct *task, vaddr_t va, int do_free)
{
	paddr_t lv1_table, lv2_table_pa;
	uint64_t entry;
	int idx1, idx2;

	if (!task || !task->mm.first_table)
		return -1;

	if (va & (SECTION_SIZE - 1))
		return -1;

	lv1_table = task->mm.first_table;

	/* Level 1 */
	idx1 = (va >> LV1_SHIFT) & (PTRS_PER_TABLE - 1);
	entry = ((uint64_t *)TO_VADDR(lv1_table))[idx1];
	if ((entry & 0x3) != 3)   /* 不是 table descriptor */
		return -1;
	lv2_table_pa = entry & PAGE_MASK;

	/* Level 2 — 检查是否是 block */
	idx2 = (va >> LV2_SHIFT) & (PTRS_PER_TABLE - 1);
	entry = ((uint64_t *)TO_VADDR(lv2_table_pa))[idx2];
	if ((entry & 0x3) != MM_TYPE_BLOCK)
		return -1;

	/* 释放物理内存（2MB = 512 页） */
	if (do_free) {
		paddr_t block_pa = entry & PAGE_MASK;
		page_pool_lock_acquire();
		for (int i = 0; i < (1 << (LV2_SHIFT - PAGE_SHIFT)); i++) {
			free_page(get_page_pool(),
				  block_pa + i * PAGE_SIZE);
		}
		page_pool_lock_release();
	}

	/* 清零 L2 entry */
	((uint64_t *)TO_VADDR(lv2_table_pa))[idx2] = 0;
	task->mm.huge_pages_count--;

	/* 如果 L2 表变空，回收 L2 表页 */
	if (page_table_empty(lv2_table_pa)) {
		page_pool_lock_acquire();
		free_page(get_page_pool(), lv2_table_pa);
		page_pool_lock_release();
		task->mm.kernel_pages_count--;
		/* 清 L1 entry */
		((uint64_t *)TO_VADDR(lv1_table))[idx1] = 0;
	}

	/* 失效 TLB */
	asm volatile("tlbi vmalle1" ::: "memory");
	asm volatile("dsb ish" ::: "memory");
	asm volatile("isb" ::: "memory");

	return 0;
}

/*
 * Phase 22: 修改 2MB Block 的访问权限
 *
 * 返回 0 成功，-1 未映射或不是 block
 */
int mprotect_stage2_block(struct task_struct *task, vaddr_t va, uint64_t new_ap)
{
	paddr_t lv1_table, lv2_table_pa;
	uint64_t entry;
	int idx1, idx2;
	uint64_t *l2_entry_p;

	if (!task || !task->mm.first_table)
		return -1;

	if (va & (SECTION_SIZE - 1))
		return -1;

	new_ap &= S2_AP_MASK;

	lv1_table = task->mm.first_table;

	/* Level 1 */
	idx1 = (va >> LV1_SHIFT) & (PTRS_PER_TABLE - 1);
	entry = ((uint64_t *)TO_VADDR(lv1_table))[idx1];
	if ((entry & 0x3) != 3)
		return -1;
	lv2_table_pa = entry & PAGE_MASK;

	/* Level 2 — 找到 block entry */
	idx2 = (va >> LV2_SHIFT) & (PTRS_PER_TABLE - 1);
	l2_entry_p = &((uint64_t *)TO_VADDR(lv2_table_pa))[idx2];
	entry = *l2_entry_p;

	if ((entry & 0x3) != MM_TYPE_BLOCK)
		return -1;

	/* 修改 AP 位 */
	entry &= ~S2_AP_MASK;
	entry |= new_ap;
	*l2_entry_p = entry;

	/* 失效 TLB */
	asm volatile("tlbi vmalle1" ::: "memory");
	asm volatile("dsb ish" ::: "memory");
	asm volatile("isb" ::: "memory");

	return 0;
}

/*
 * Phase 22: 设置 VM 内存配额（字节）
 *
 *   max_bytes: 最大允许的用户内存（4KB 页 + 2MB 大页合计）
 *              0 表示无限制
 *
 * 返回 0 成功，-1 失败
 */
int set_task_memory_quota(struct task_struct *task, unsigned long max_bytes)
{
	if (!task)
		return -1;
	task->mm.max_memory = max_bytes;
	return 0;
}

/*
 * Phase 22: 获取 VM 当前用户内存使用量（字节）
 *
 * 返回总字节数 = 4KB 页 * 4096 + 2MB 大页 * 2MB
 */
unsigned long get_task_memory_usage(struct task_struct *task)
{
	if (!task)
		return 0;
	return (unsigned long)task->mm.user_pages_count * PAGE_SIZE +
	       (unsigned long)task->mm.huge_pages_count * SECTION_SIZE;
}

/*
 * Phase 22: 内存配额检查
 *
 * 检查再分配 add_bytes 字节后是否超出配额。
 * 返回 0 表示允许，-1 表示超出配额。
 * 配额为 0 时表示无限制。
 */
int quota_check(struct task_struct *task, unsigned long add_bytes)
{
	if (!task || task->mm.max_memory == 0)
		return 0;
	unsigned long usage = get_task_memory_usage(task);
	if (usage + add_bytes > task->mm.max_memory)
		return -1;
	return 0;
}

/*
 * Phase 22: 分配连续物理页（用于大页映射）
 *
 *   npages: 连续页数（必须是 2 的幂，方便对齐）
 *
 * 返回连续块的起始物理地址，失败返回 0。
 *
 * 算法：扫描 memap，找 npages 个连续空闲页，
 *       且起始索引是 npages 的倍数（保证自然对齐）。
 */
paddr_t allocate_block_pages(int npages)
{
	struct page_pool *pool = get_page_pool();
	paddr_t result = 0;

	if (npages <= 0 || (npages & (npages - 1)) != 0)
		return 0;  /* 必须是 2 的幂 */

	page_pool_lock_acquire();

	for (unsigned long i = 0; i < pool->page_nr; i++) {
		unsigned long start =
			(pool->last_page_id + i) % pool->page_nr;

		/* 自然对齐：起始索引必须是 npages 的倍数 */
		if (start % (unsigned long)npages != 0)
			continue;

		/* 检查 npages 个连续页是否都空闲 */
		int ok = 1;
		for (int j = 0; j < npages; j++) {
			unsigned long idx = start + j;
			if (idx >= pool->page_nr ||
			    pool->memap[idx] != 0) {
				ok = 0;
				break;
			}
		}

		if (ok) {
			for (int j = 0; j < npages; j++)
				pool->memap[start + j] = 1;
			result = pool->start_addr + start * PAGE_SIZE;
			pool->last_page_id =
				(start + npages) % pool->page_nr;
			break;
		}
	}

	page_pool_lock_release();
	return result;
}

/*
 * Phase 22: 释放连续物理页
 */
void free_block_pages(paddr_t block_pa, int npages)
{
	struct page_pool *pool = get_page_pool();
	unsigned long start_idx =
		(block_pa - pool->start_addr) / PAGE_SIZE;

	if (block_pa < pool->start_addr ||
	    start_idx + npages > pool->page_nr) {
		PANIC("free_block_pages: invalid pa 0x%lx (npages=%d)",
		      block_pa, npages);
	}

	page_pool_lock_acquire();
	for (int j = 0; j < npages; j++)
		pool->memap[start_idx + j] = 0;
	page_pool_lock_release();
}

paddr_t get_ipa(vaddr_t va)
{
	/*
	 * 学习要点：
	 *   Guest 的两级地址翻译是 VA --(stage1)--> IPA --(stage2)--> PA。
	 *   stage 1 由 Guest 自己的 MMU（SCTLR_EL1.M）控制：
	 *     - MMU 开启（如分页 OS）：用 AT 指令做硬件翻译
	 *     - MMU 关闭（裸机 guest）：VA == IPA，直接透传
	 *   如果不加判断，对 MMU 关闭的 guest 执行 at s1e1r 会得到
	 *   PAR_EL1.F=1（翻译失败），IPA 计算出错。
	 *   在 EL2 直接读 SCTLR_EL1 拿到的就是当前 VM 的实时值。
	 */
	if ((READ_SYSREG64(sctlr_el1) & 1) == 0)
		return va;

	paddr_t par = translate_el1(va);

	/* 检查 PAR_EL1.F 位：1 = 翻译失败，退回 VA == IPA */
	if (par & 1)
		return va;

	paddr_t ipa = par & 0xFFFFFFFFF000;
	ipa |= va & 0xFFF;
	return ipa;
}

#define ISS_ABORT_DFSC_MASK 0x3f

/*
 * Phase 23: Access Flag Fault 处理
 *   - 设置页表 entry 的 AF 位（bit 10）
 *   - 刷新 TLB
 *   - 返回 0 表示已处理（Guest 可以重试指令）
 *
 * 支持 L3 page 和 L2 block 两种情况。
 */
int handle_access_flag_fault(struct task_struct *task, vaddr_t ipa)
{
	paddr_t lv1_table, lv2_table_pa;
	uint64_t entry;
	int idx1, idx2, idx3;

	if (!task || !task->mm.first_table)
		return -1;

	ipa &= PAGE_MASK;
	lv1_table = task->mm.first_table;

	/* Level 1 */
	idx1 = (ipa >> LV1_SHIFT) & (PTRS_PER_TABLE - 1);
	entry = ((uint64_t *)TO_VADDR(lv1_table))[idx1];
	if ((entry & 0x3) != 3)
		return -1;
	lv2_table_pa = entry & PAGE_MASK;

	/* Level 2 — 可能是 block 也可能是 table */
	idx2 = (ipa >> LV2_SHIFT) & (PTRS_PER_TABLE - 1);
	entry = ((uint64_t *)TO_VADDR(lv2_table_pa))[idx2];
	if ((entry & 0x3) == 1) {
		/* L2 block entry（2MB 大页） */
		if ((entry & MM_STAGE2_ACCESS) == 0) {
			entry |= MM_STAGE2_ACCESS;
			((uint64_t *)TO_VADDR(lv2_table_pa))[idx2] = entry;
			asm volatile("tlbi vmalle1" ::: "memory");
			asm volatile("dsb ish" ::: "memory");
			asm volatile("isb" ::: "memory");
			return 0;
		}
		return -1;  /* AF 已经置位了，不是 AF fault */
	} else if ((entry & 0x3) != 3) {
		return -1;  /* 无效 entry */
	}

	/* Level 3 — page entry（4KB 小页） */
	paddr_t lv3_table_pa = entry & PAGE_MASK;
	idx3 = (ipa >> PAGE_SHIFT) & (PTRS_PER_TABLE - 1);
	uint64_t *l3_entry_p = &((uint64_t *)TO_VADDR(lv3_table_pa))[idx3];
	entry = *l3_entry_p;

	if ((entry & 0x3) != 0x3)  /* 不是 page descriptor */
		return -1;

	if (entry & MM_STAGE2_ACCESS)
		return -1;  /* AF 已经是 1，不应该 */

	entry |= MM_STAGE2_ACCESS;
	*l3_entry_p = entry;

	asm volatile("tlbi vmalle1" ::: "memory");
	asm volatile("dsb ish" ::: "memory");
	asm volatile("isb" ::: "memory");

	return 0;
}

/*
 * Phase 23: 大页拆分（split 2MB block into 512 4KB pages）
 *
 * 步骤：
 *   1. 找到 L2 block entry，确认是 block descriptor
 *   2. 分配一个 L3 页表页
 *   3. 填充 512 个 L3 page entry（继承 block 的属性）
 *   4. 将 L2 entry 改为 table descriptor，指向新 L3 表
 *   5. 刷新 TLB
 *   6. 更新计数：huge_pages_count--, user_pages_count += 512, kernel_pages_count++
 *
 * 返回：0 成功，-1 失败
 */
int split_stage2_block(struct task_struct *task, vaddr_t va)
{
	paddr_t lv1_table, lv2_table_pa;
	uint64_t *l2_entry_p;
	uint64_t block_entry;
	int idx1, idx2;

	if (!task || !task->mm.first_table)
		return -1;

	va &= ~((1ULL << LV2_SHIFT) - 1);  /* 2MB 对齐 */
	lv1_table = task->mm.first_table;

	/* Level 1 */
	idx1 = (va >> LV1_SHIFT) & (PTRS_PER_TABLE - 1);
	block_entry = ((uint64_t *)TO_VADDR(lv1_table))[idx1];
	if ((block_entry & 0x3) != 3)
		return -1;
	lv2_table_pa = block_entry & PAGE_MASK;

	/* Level 2 — 必须是 block entry */
	idx2 = (va >> LV2_SHIFT) & (PTRS_PER_TABLE - 1);
	l2_entry_p = &((uint64_t *)TO_VADDR(lv2_table_pa))[idx2];
	block_entry = *l2_entry_p;

	if ((block_entry & 0x3) != 1)  /* 不是 block descriptor */
		return -1;

	/* 提取 block 的物理地址和属性 */
	paddr_t block_pa = block_entry & ~((1ULL << LV2_SHIFT) - 1);
	uint64_t block_attrs = block_entry & 0xfff;  /* 低 12 位属性 */

	/* 分配 L3 页表页 */
	paddr_t lv3_table_pa = get_free_page(get_page_pool());
	if (!lv3_table_pa)
		return -1;

	uint64_t *lv3_table = (uint64_t *)TO_VADDR(lv3_table_pa);
	memset(lv3_table, 0, PAGE_SIZE);

	/* 构造 L3 page entry 的属性基值：
	 *   - 保留低 12 位属性（AF, SH, AP, MemAttr 等）
	 *   - 把类型位从 block (01) 改为 page (11)
	 */
	uint64_t page_attr_base = (block_attrs & ~0x3ULL) | MM_TYPE_PAGE;

	/* 填充 512 个 L3 entry */
	for (int i = 0; i < PTRS_PER_TABLE; i++) {
		paddr_t page_pa = block_pa + ((uint64_t)i << PAGE_SHIFT);
		lv3_table[i] = (page_pa & PAGE_MASK) | page_attr_base;
	}

	/* 将 L2 entry 改为 table descriptor */
	*l2_entry_p = (lv3_table_pa & PAGE_MASK) | 0x3;  /* table = bits[1:0]=11 */

	/* 刷新 TLB */
	asm volatile("tlbi vmalle1" ::: "memory");
	asm volatile("dsb ish" ::: "memory");
	asm volatile("isb" ::: "memory");

	/* 更新计数 */
	task->mm.huge_pages_count--;
	task->mm.user_pages_count += 512;
	task->mm.kernel_pages_count++;

	return 0;
}

/*
 * Phase 23: 页面回收 — 时钟算法（CLOCK / second-chance）
 *
 * 原理：
 *   利用页表的 AF (Access Flag) 位跟踪页面访问情况。
 *   - 页面首次映射时 AF=1
 *   - 时钟扫描时清除 AF 位（给"第二次机会"）
 *   - 如果 Guest 访问了页面，硬件会重新设置 AF=1（通过 Access Flag Fault）
 *   - 下次扫描时如果 AF=0，说明这段时间没被访问过 → 回收
 *
 * 算法流程：
 *   从 clock_hand 开始扫描页表：
 *     - entry 是大页 (block) 且 AF=0 → 整体回收（512 页）
 *     - entry 是大页 (block) 且 AF=1 → 清除 AF 位，指针前进 2MB
 *     - entry 是小页 (page) 且 AF=0 → 回收（1 页）
 *     - entry 是小页 (page) 且 AF=1 → 清除 AF 位，指针前进 4KB
 *     - entry 空 → 指针前进 4KB，不计数
 *
 *   最多扫描 2 * total_mapped_pages 个映射页（约两圈），
 *   达到目标回收数或扫完两圈就停止。
 *
 * 返回：实际回收的 4KB 页面数
 */
int reclaim_pages(struct task_struct *task, int nr_pages)
{
	int reclaimed = 0;
	int total_pages;
	int max_scan;
	int scanned_mapped = 0;  /* 已扫描的有映射页面数（按 4KB 计） */

	if (!task || !task->mm.first_table || nr_pages <= 0)
		return 0;

	total_pages = task->mm.user_pages_count + task->mm.huge_pages_count * 512;
	if (total_pages == 0)
		return 0;

	/*
	 * 渐进式回收：每次最多扫描一圈映射的页面。
	 *   - 第一遍遇到 AF=1 → 清 AF（给第二次机会）
	 *   - 第二遍（下次调用）遇到 AF=0 → 回收
	 *
	 * 这样 Guest 在两次回收之间有机会运行，
	 * 活跃页面的 AF 会被重新设置（通过 Access Flag Fault），
	 * 不活跃页面保持 AF=0 并被回收。
	 */
	max_scan = total_pages;

	while (reclaimed < nr_pages && scanned_mapped < max_scan) {
		vaddr_t ipa = task->mm.clock_hand;
		paddr_t lv1_table = task->mm.first_table;
		int idx1, idx2, idx3;
		uint64_t entry;

		idx1 = (ipa >> LV1_SHIFT) & (PTRS_PER_TABLE - 1);
		entry = ((uint64_t *)TO_VADDR(lv1_table))[idx1];
		if ((entry & 0x3) != 3) {
			/* L1 无效 → 整个 1GB 都没映射，跳过整个 1GB */
			task->mm.clock_hand = (ipa & ~((1ULL << LV1_SHIFT) - 1)) +
					      (1ULL << LV1_SHIFT);
			/* 检查是否回绕 */
			if (task->mm.clock_hand == 0)
				task->mm.clock_hand = 0;  /* 从 0 开始 */
			continue;
		}

		/* 确保 L1 table entry 有 AF 位（旧的 entry 可能没有） */
		if ((entry & MM_STAGE2_ACCESS) == 0) {
			entry |= MM_STAGE2_ACCESS;
			((uint64_t *)TO_VADDR(lv1_table))[idx1] = entry;
		}

		paddr_t lv2_table_pa = entry & PAGE_MASK;
		idx2 = (ipa >> LV2_SHIFT) & (PTRS_PER_TABLE - 1);
		entry = ((uint64_t *)TO_VADDR(lv2_table_pa))[idx2];

		if ((entry & 0x3) == 1) {
			/* L2 block（2MB 大页） */
			scanned_mapped += 512;

			if (entry & MM_STAGE2_ACCESS) {
				/* AF=1 → 清除 AF 位，给第二次机会 */
				entry &= ~MM_STAGE2_ACCESS;
				((uint64_t *)TO_VADDR(lv2_table_pa))[idx2] = entry;
			} else {
				/* AF=0 → 回收整个大页 */
				vaddr_t block_va = ipa & ~((1ULL << LV2_SHIFT) - 1);
				unmap_stage2_block(task, block_va, 1);
				reclaimed += 512;
			}

			/* 指针前进 2MB */
			task->mm.clock_hand = (ipa & ~((1ULL << LV2_SHIFT) - 1)) +
					      (1ULL << LV2_SHIFT);

		} else if ((entry & 0x3) == 3) {
			/* L2 → L3 table */

			/* 确保 L2 table entry 有 AF 位（旧的 entry 可能没有） */
			if ((entry & MM_STAGE2_ACCESS) == 0) {
				entry |= MM_STAGE2_ACCESS;
				((uint64_t *)TO_VADDR(lv2_table_pa))[idx2] = entry;
			}

			paddr_t lv3_table_pa = entry & PAGE_MASK;
			idx3 = (ipa >> PAGE_SHIFT) & (PTRS_PER_TABLE - 1);
			uint64_t *l3_entry_p =
				&((uint64_t *)TO_VADDR(lv3_table_pa))[idx3];
			entry = *l3_entry_p;

			if ((entry & 0x3) == 0x3) {
				/* L3 page（4KB 小页） */
				scanned_mapped++;

				if (entry & MM_STAGE2_ACCESS) {
					/* AF=1 → 清除 AF 位 */
					entry &= ~MM_STAGE2_ACCESS;
					*l3_entry_p = entry;
				} else {
					/* AF=0 → 回收该页 */
					vaddr_t page_va = ipa & PAGE_MASK;
					unmap_stage2_page(task, page_va, 1);
					reclaimed++;
				}
			}
			/* 空 entry: scanned_mapped 不增加 */

			/* 指针前进 4KB */
			task->mm.clock_hand = (ipa & PAGE_MASK) + PAGE_SIZE;

		} else {
			/* L2 空 entry → 前进 4KB（到下一个 L3 slot） */
			task->mm.clock_hand = (ipa & PAGE_MASK) + PAGE_SIZE;
		}

		/* 地址空间回绕：32-bit IPA (4GB) 后回到 0 */
		if (task->mm.clock_hand >= (1ULL << 32))
			task->mm.clock_hand = 0;
	}

	/* 刷新 TLB（批量操作后统一刷新） */
	if (scanned_mapped > 0) {
		asm volatile("tlbi vmalle1" ::: "memory");
		asm volatile("dsb ish" ::: "memory");
		asm volatile("isb" ::: "memory");
	}

	/* 更新统计 */
	task->mm.reclaim_count += reclaimed;
	if (reclaimed == 0 && nr_pages > 0)
		task->mm.reclaim_fail_count++;

	return reclaimed;
}

/*
 * Phase 23: 查询回收统计
 */
int get_reclaim_stats(struct task_struct *task, int *reclaim_count,
		      int *reclaim_fail_count)
{
	if (!task)
		return -1;
	if (reclaim_count)
		*reclaim_count = task->mm.reclaim_count;
	if (reclaim_fail_count)
		*reclaim_fail_count = task->mm.reclaim_fail_count;
	return 0;
}

int handle_mem_abort(vaddr_t addr, uint64_t esr)
{
	struct pt_regs *regs = task_pt_regs(current);
	uint64_t dfsc = esr & ISS_ABORT_DFSC_MASK;
	const struct board_ops *ops = current->board_ops;
	vaddr_t ipa;

	/*
	 * S1PTW (bit 7): Stage 1 translation table walk fault.
	 *   S1PTW=1: Stage-2 fault 发生在 Stage-1 页表遍历过程中。
	 *            FAR_EL2 = 触发翻译的虚拟地址（不是 fault 地址）。
	 *            HPFAR_EL2 = Stage-2 fault 的真实 IPA。
	 *   S1PTW=0: 普通的 Stage-2 fault。
	 *            FAR_EL2 = 触发 fault 的虚拟地址。
	 */
	if (esr & (1 << 7)) {
		uint64_t hpfar;
		asm volatile("mrs %0, hpfar_el2" : "=r"(hpfar));
		/* HPFAR 格式：IPA[39:12] 存放在 bits [31:4]，
		 * bit 0=F, bits[3:1]=RES0。
		 * 即 ipa = (hpfar & ~0xF) << 8 */
		ipa = (hpfar & 0xFFFFFFFFF0ULL) << 8;
	} else {
		ipa = get_ipa(addr);
	}

	/*
	 * Phase 23: Access Flag Fault
	 *   dfsc >> 2 == 0x2 → Access flag fault (level 0-3)
	 *   处理：设置页表 entry 的 AF 位，刷新 TLB，返回重试
	 */
	if ((dfsc >> 2) == 0x2) {
		if (handle_access_flag_fault(current, ipa) == 0)
			return 0;
		/* 如果不是 AF fault（比如 entry 不存在），继续后面的处理 */
	}

	/*
	 * 容错处理：QEMU 可能将某些 Stage-2 Access Flag Fault 误报为
	 * Translation Fault（特别是 S1PTW=1 时，即 Stage-1 页表遍历
	 * 过程中发生的 Stage-2 AF fault）。对于 translation fault，
	 * 先尝试按 AF fault 处理：如果 entry 存在但 AF=0，设置 AF
	 * 位并刷新 TLB。
	 */
	if ((dfsc >> 2) == 0x1) {
		if (handle_access_flag_fault(current, ipa) == 0)
			return 0;
	}

	/*
	 * 检查是否是 MMIO 地址：
	 *   - permission fault (dfsc >> 2 == 0x3) 一定是 MMIO
	 *   - translation fault (dfsc >> 2 == 0x1) 可能是 demand paging，
	 *     也可能是 MMIO 区域（无映射）。用 board_ops->is_mmio_addr 判断。
	 *
	 * 如果是 MMIO 地址，走设备模拟路径。
	 */
	int is_mmio = 0;

	if ((dfsc >> 2) == 0x3) {
		/* Phase 21 fix: permission fault 不一定是 MMIO！
		 * 可能是 mprotect 设置的权限限制（RO/NONE）。
		 * 需要先判断是否是 MMIO 地址。 */
		if (HAVE_FUNC(ops, is_mmio_addr) &&
		    ops->is_mmio_addr(current, ipa))
			is_mmio = 1;
	} else if ((dfsc >> 2) == 0x1) {
		/* translation fault -> 检查是否是 MMIO 区域 */
		if (HAVE_FUNC(ops, is_mmio_addr) &&
		    ops->is_mmio_addr(current, ipa))
			is_mmio = 1;
	}

	/*
	 * Phase 21: 非 MMIO 的 Permission Fault → mprotect 导致的权限错误
	 * 需要把异常转发给 Guest（模拟 Guest 自己发生了 Data Abort）。
	 *
	 * 注入方式：
	 *   - 修改 current->cpu_sysregs 中的 EL1 系统寄存器
	 *     （elr_el1, esr_el1, far_el1, spsr_el1）
	 *   - 修改 pt_regs.pc，让 Guest 返回到向量表入口
	 *   - 注意：不能直接 msr 写物理寄存器，因为 kernel_exit 里的
	 *     vm_entering_work → restore_sysregs 会从 cpu_sysregs
	 *     恢复所有 EL1 寄存器，覆盖直接写入的值。
	 */
	if ((dfsc >> 2) == 0x3 && !is_mmio) {
		uint64_t vbar = current->cpu_sysregs.vbar_el1;
		uint64_t elr = regs->pc;
		uint64_t spsr = regs->pstate;

		/* 模拟 Guest Data Abort：
		 *   ESR_EL1: EC=0x25 (Data Abort, same EL),
		 *   IL=1 (32-bit instruction), ISV=1
		 *   SAS, SRT, SF, WnR, DFSC 从原始 ESR 透传
		 *
		 * ESR Data Abort 位域：
		 *   EC[31:26], IL[25], ISV[24], SAS[23:22], SSE[21],
		 *   SRT[20:16], SF[15], AR[14], VNCR[13], LST[12:11],
		 *   FnV[10], EA[9], CM[8], S1PTW[7], WnR[6], DFSC[5:0]
		 */
		uint64_t srt = (esr >> 16) & 0x1f;
		uint64_t wnr = (esr >> 6) & 0x1;
		uint64_t sf  = (esr >> 15) & 0x1;
		uint64_t sas = (esr >> 22) & 0x3;
		uint64_t sse = (esr >> 21) & 0x1;
		uint64_t guest_esr =
			(0x25ULL << 26) | /* EC: Data Abort, same EL */
			(1ULL << 25)     | /* IL = 1 (32-bit) */
			(1ULL << 24)     | /* ISV = 1 */
			(sas << 22)      | /* SAS */
			(sse << 21)      | /* SSE */
			(srt << 16)      | /* SRT */
			(sf  << 15)      | /* SF */
			(wnr << 6)       | /* WnR */
			(dfsc & 0x3f);     /* DFSC */

		/* 修改 cpu_sysregs 中的 EL1 寄存器（restore_sysregs 时生效）
		 * 注意：不能直接 msr 写物理寄存器，因为 kernel_exit 里的
		 * vm_entering_work → restore_sysregs 会从 cpu_sysregs
		 * 恢复所有 EL1 寄存器，覆盖直接写入的值。 */
		current->cpu_sysregs.elr_el1 = elr;
		current->cpu_sysregs.esr_el1 = guest_esr;
		current->cpu_sysregs.far_el1 = addr;
		current->cpu_sysregs.spsr_el1 = spsr;

		/* 跳转到 Guest 向量表：Current EL with SPx → offset 0x200 */
		regs->pc = vbar + 0x200;

		/* 模拟异常进入：设置 DAIF 屏蔽（和硬件自动行为一致）
		 * Guest 已经在 EL1h，所以 M 位不变 */
		regs->pstate |= (0xf << 6);

		return 0;
	}

	if (is_mmio) {
		unsigned int srt = (esr >> 16) & 0x1f;
		unsigned int wnr = (esr >> 6) & 0x1;
		unsigned int sf  = (esr >> 30) & 0x1;  /* 0 = 32-bit, 1 = 64-bit */

		if (wnr == 0) {
			if (HAVE_FUNC(ops, mmio_read)) {
				uint64_t val = ops->mmio_read(current, ipa);
				/* 目标寄存器是 WZR/XZR (srt=31) 时，不写回
				 * （WZR 永远是 0，写了也没用）。
				 * 编译器通常不会生成读 WZR 的代码，但防一手。 */
				if (srt < 31)
					regs->regs[srt] = val;
			}
		} else {
			if (HAVE_FUNC(ops, mmio_write)) {
				uint64_t val;
				/* 源寄存器是 WZR/XZR (srt=31) 时，值为 0
				 * （编译器优化参数为 0 时常会用 WZR 写入）。
				 * pt_regs 只有 X0-X30 共 31 个寄存器，
				 * regs[31] 是越界访问，读到的是垃圾值。 */
				if (srt == 31) {
					val = 0;
				} else {
					val = regs->regs[srt];
					/* 32-bit 写操作：只取低 32 位 */
					if (sf == 0)
						val = val & 0xFFFFFFFFULL;
				}
				ops->mmio_write(current, ipa, val);
			}
		}

		increment_current_pc(4);
		current->stat.mmio_count++;
		return 0;
	}

	if ((dfsc >> 2) == 0x1) {
		// translation fault -> demand paging
		/* Phase 22: 配额检查：超出则拒绝分配 */
		if (quota_check(current, PAGE_SIZE) < 0) {
			/* Phase 23: 配额不足时先尝试回收页面 */
			int reclaimed = reclaim_pages(current, 1);
			if (reclaimed <= 0 || quota_check(current, PAGE_SIZE) < 0) {
				WARN("demand page: quota exceeded (vm=%d, ipa=0x%lx)",
				     current->pid, ipa & PAGE_MASK);
				return -1;
			}
			/* 回收成功，继续分配 */
		}

		paddr_t page = get_free_page(get_page_pool());

		if (page == 0) {
			return -1;
		}

		map_stage2_page(current, ipa & PAGE_MASK, page,
				MMU_STAGE2_PAGE_FLAGS);
		current->stat.pf_count++;

		/* 调试：打印 demand paging 信息 */
		INFO("demand page: vm=%d ipa=0x%lx pa=0x%lx (total pf=%d)",
		     current->pid, ipa & PAGE_MASK, page, current->stat.pf_count);

		return 0;
	}

	return -1;
}

/*
 * 遍历 VM 的 stage-2 页表，把 IPA 翻译成物理 PA。
 * 返回 0 成功，pa 填物理地址（含页内偏移）；-1 表示未映射。
 *
 * 页表结构（3 级，4KB granule，T0SZ=16 -> 48-bit IPA）：
 *   Level 1 (pg_dir)  : [47:30] -> 1GB block / Level 2 table
 *   Level 2           : [29:21] -> 2MB block / Level 3 table
 *   Level 3           : [20:12] -> 4KB page
 *
 * 但实际 aVisor 用的是 2 级映射（LV1_SHIFT=30, LV2_SHIFT=21，
 * map_stage2_page 里有 lv1_table -> lv2_table -> lv3_table 三级）。
 * 仔细看 mm.c 的 map_stage2_page：
 *   lv1_table (first_table) -> map_stage2_table(LV1_SHIFT=30) -> lv2_table
 *   lv2_table -> map_stage2_table(LV2_SHIFT=21) -> lv3_table
 *   lv3_table -> map_stage2_table_entry(PAGE_SHIFT=12) -> page
 * 所以是 3 级：L1(1GB) -> L2(2MB) -> L3(4KB page)。
 *
 * 每个 table entry：
 *   bit [0] = 0 表示无效（fault）
 *   bit [1:0] = 01 表示 block（L1/L2）
 *   bit [1:0] = 11 表示 table / page（L3 是 page）
 *   bits [47:PAGE_SHIFT] = 输出地址（block/page 对齐）
 */
int ipa_to_pa(struct task_struct *vm, paddr_t ipa, paddr_t *pa)
{
	paddr_t lv1_table, lv2_table, lv3_table;
	uint64_t entry;

	if (!vm || !vm->mm.first_table)
		return -1;

	lv1_table = vm->mm.first_table;

	/* Level 1 */
	{
		uint64_t idx = (ipa >> LV1_SHIFT) & (PTRS_PER_TABLE - 1);
		entry = ((uint64_t *)TO_VADDR(lv1_table))[idx];
	}
	if ((entry & 0x3) == 0)
		return -1;  /* 未映射 */
	if ((entry & 0x3) == 1) {
		/* Level 1 block (1GB) */
		*pa = (entry & ~((1ULL << LV1_SHIFT) - 1)) |
		      (ipa & ((1ULL << LV1_SHIFT) - 1));
		return 0;
	}
	/* entry & 0x3 == 3: table descriptor */
	lv2_table = entry & PAGE_MASK;

	/* Level 2 */
	{
		uint64_t idx = (ipa >> LV2_SHIFT) & (PTRS_PER_TABLE - 1);
		entry = ((uint64_t *)TO_VADDR(lv2_table))[idx];
	}
	if ((entry & 0x3) == 0)
		return -1;
	if ((entry & 0x3) == 1) {
		/* Level 2 block (2MB) */
		*pa = (entry & ~((1ULL << LV2_SHIFT) - 1)) |
		      (ipa & ((1ULL << LV2_SHIFT) - 1));
		return 0;
	}
	/* table descriptor */
	lv3_table = entry & PAGE_MASK;

	/* Level 3 (4KB page) */
	{
		uint64_t idx = (ipa >> PAGE_SHIFT) & (PTRS_PER_TABLE - 1);
		entry = ((uint64_t *)TO_VADDR(lv3_table))[idx];
	}
	if ((entry & 0x3) != 3)
		return -1;  /* 无效或非 page */

	*pa = (entry & PAGE_MASK) | (ipa & (PAGE_SIZE - 1));
	return 0;
}

/*
 * Phase 10: 动态内存映射 hypercall
 *
 * hyp_mem_map: 为当前 VM 在指定 IPA 上映射一个新物理页
 *   ipa: 目标 IPA（4KB 对齐）
 *   flags: 保留，当前固定为 Normal Memory + RW
 * 返回 0 成功，-1 失败
 */
int hyp_mem_map(paddr_t ipa, uint64_t flags)
{
	paddr_t page;

	(void)flags;  /* 暂时忽略，固定 Normal + RW */

	/* 检查 IPA 是否已经映射（避免覆盖）
	 * 用 ipa_to_pa 而不是 check_task_page_mapped，
	 * 因为后者会创建中间表（有副作用）且检查粒度不准确。 */
	paddr_t dummy_pa;
	if (ipa_to_pa(current, ipa & PAGE_MASK, &dummy_pa) == 0) {
		WARN("hyp_mem_map: ipa 0x%lx already mapped",
		     (unsigned long)ipa);
		return -1;
	}

	/* Phase 23: 配额检查，不足则尝试回收 */
	if (quota_check(current, PAGE_SIZE) < 0) {
		int reclaimed = reclaim_pages(current, 1);
		if (reclaimed <= 0 || quota_check(current, PAGE_SIZE) < 0) {
			WARN("hyp_mem_map: quota exceeded (ipa=0x%lx)",
			     (unsigned long)(ipa & PAGE_MASK));
			return -1;
		}
	}

	/* 分配物理页 */
	page = get_free_page(get_page_pool());
	if (page == 0) {
		WARN("hyp_mem_map: out of memory");
		return -1;
	}

	/* 清零新页面 */
	memset((void *)TO_VADDR(page), 0, PAGE_SIZE);

	/* 映射到 Stage-2 页表 */
	map_stage2_page(current, ipa & PAGE_MASK, page,
			MMU_STAGE2_PAGE_FLAGS);

	INFO("hyp_mem_map: vm=%lu ipa=0x%lx pa=0x%lx",
	     current->vmid, (unsigned long)(ipa & PAGE_MASK),
	     (unsigned long)page);
	return 0;
}

/*
 * hyp_mem_unmap: 取消指定 IPA 的映射，释放物理页
 *   ipa: 目标 IPA（4KB 对齐）
 * 返回 0 成功，-1 失败
 *
 * Phase 20: 改为调用通用的 unmap_stage2_page()，
 *   自动获得页表自底向上回收能力，且修复了 free_page 未加锁的 bug。
 */
int hyp_mem_unmap(paddr_t ipa)
{
	int ret = unmap_stage2_page(current, ipa, 1);
	if (ret < 0) {
		WARN("hyp_mem_unmap: ipa 0x%lx not mapped",
		     (unsigned long)ipa);
		return -1;
	}

	INFO("hyp_mem_unmap: vm=%lu ipa=0x%lx",
	     current->vmid, (unsigned long)(ipa & PAGE_MASK));
	return 0;
}
