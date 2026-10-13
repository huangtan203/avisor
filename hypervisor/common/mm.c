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
	pool->memap[(p - pool->start_addr) / PAGE_SIZE] = 0;
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
		uint64_t entry = next_level_table | MM_TYPE_PAGE_TABLE;

		((uint64_t *)table)[index] = entry;
		return next_level_table;
	} else {
		*new_table = 0;
	}

	return ((uint64_t *)table)[index] & PAGE_MASK;
}

bool check_task_page_mapped(struct task_struct *task, vaddr_t va)
{
	paddr_t lv1_table;

	if (!task->mm.first_table) {
		task->mm.first_table = get_free_page(get_page_pool());
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

	paddr_t ipa = translate_el1(va);

	ipa &= 0xFFFFFFFFF000;
	ipa |= va & 0xFFF;
	return ipa;
}

#define ISS_ABORT_DFSC_MASK 0x3f

int handle_mem_abort(vaddr_t addr, uint64_t esr)
{
	struct pt_regs *regs = task_pt_regs(current);
	uint64_t dfsc = esr & ISS_ABORT_DFSC_MASK;
	const struct board_ops *ops = current->board_ops;
	vaddr_t ipa = get_ipa(addr);

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
		/* permission fault -> 肯定是 MMIO */
		is_mmio = 1;
	} else if ((dfsc >> 2) == 0x1) {
		/* translation fault -> 检查是否是 MMIO 区域 */
		if (HAVE_FUNC(ops, is_mmio_addr) &&
		    ops->is_mmio_addr(current, ipa))
			is_mmio = 1;
	}

	if (is_mmio) {
		unsigned int srt = (esr >> 16) & 0x1f;
		unsigned int wnr = (esr >> 6) & 0x1;

		if (wnr == 0) {
			if (HAVE_FUNC(ops, mmio_read))
				regs->regs[srt] = ops->mmio_read(current, ipa);
		} else {
			if (HAVE_FUNC(ops, mmio_write))
				ops->mmio_write(current, ipa,
						regs->regs[srt]);
		}

		increment_current_pc(4);
		current->stat.mmio_count++;
		return 0;
	}

	if ((dfsc >> 2) == 0x1) {
		// translation fault -> demand paging
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
