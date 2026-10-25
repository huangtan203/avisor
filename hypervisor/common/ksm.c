// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - KSM (Kernel Samepage Merging)
 *
 * Phase 24: 内存去重
 */

#include <common/ksm.h>
#include <common/utils.h>
#include <common/debug.h>
#include <common/printf.h>
#include <arch/aarch64/mmu.h>

/* 全局哈希表（bucket 数组） */
static struct ksm_page *ksm_hashtab[KSM_HASH_SIZE];

/* 全局统计 */
static struct ksm_stats ksm_global_stats;

/* FNV-1a 32-bit 哈希常量 */
#define FNV1A_32_OFFSET  2166136261u
#define FNV1A_32_PRIME   16777619u

/* 简单的内存分配：从页池中分配一个页作为 KSM 结构体存储 */
/* 由于我们没有 malloc，用静态数组或页池来管理 ksm_page 结构体 */

/* KSM page 结构体池（最多 2048 个，对应 8MB 的元数据开销） */
#define KSM_MAX_PAGES  2048
static struct ksm_page ksm_page_pool[KSM_MAX_PAGES];
static int ksm_page_free_count = KSM_MAX_PAGES;
static struct ksm_page *ksm_page_free_list;

/* 初始化空闲链表 */
static void ksm_page_pool_init(void)
{
	int i;
	ksm_page_free_list = &ksm_page_pool[0];
	for (i = 0; i < KSM_MAX_PAGES - 1; i++) {
		ksm_page_pool[i].next = &ksm_page_pool[i + 1];
	}
	ksm_page_pool[KSM_MAX_PAGES - 1].next = 0;
	ksm_page_free_count = KSM_MAX_PAGES;
}

static struct ksm_page *ksm_page_alloc(void)
{
	struct ksm_page *p = ksm_page_free_list;
	if (!p)
		return 0;
	ksm_page_free_list = p->next;
	ksm_page_free_count--;
	memset(p, 0, sizeof(*p));
	return p;
}

static void ksm_page_free(struct ksm_page *p)
{
	if (!p)
		return;
	p->next = ksm_page_free_list;
	ksm_page_free_list = p;
	ksm_page_free_count++;
}

/* 哈希函数：FNV-1a 32-bit */
uint32_t ksm_page_hash(const void *page_addr)
{
	const unsigned char *data = (const unsigned char *)page_addr;
	uint32_t hash = FNV1A_32_OFFSET;
	int i;

	for (i = 0; i < PAGE_SIZE; i++) {
		hash ^= data[i];
		hash *= FNV1A_32_PRIME;
	}

	return hash;
}

/* 比较两个页面内容是否相同 */
int ksm_pages_equal(const void *page1, const void *page2)
{
	return memcmp(page1, page2, PAGE_SIZE);
}

/* 哈希表插入 */
static void ksm_hash_insert(struct ksm_page *kp)
{
	uint32_t idx = kp->hash & (KSM_HASH_SIZE - 1);
	kp->next = ksm_hashtab[idx];
	ksm_hashtab[idx] = kp;
}

/* 哈希表删除 */
static void ksm_hash_remove(struct ksm_page *kp)
{
	uint32_t idx = kp->hash & (KSM_HASH_SIZE - 1);
	struct ksm_page **pp = &ksm_hashtab[idx];

	while (*pp) {
		if (*pp == kp) {
			*pp = kp->next;
			kp->next = 0;
			return;
		}
		pp = &(*pp)->next;
	}
}

/*
 * 哈希表查找：找相同内容的共享页
 *   hash: 页面内容哈希
 *   page_vaddr: 页面虚拟地址（用于逐字节比较）
 *   返回找到的 ksm_page，没找到返回 0
 */
static struct ksm_page *ksm_hash_lookup(uint32_t hash, const void *page_vaddr)
{
	uint32_t idx = hash & (KSM_HASH_SIZE - 1);
	struct ksm_page *kp = ksm_hashtab[idx];

	while (kp) {
		if (kp->hash == hash && kp->status == KSM_PAGE_SHARED) {
			/* 哈希相同，逐字节比较防止碰撞 */
			void *shared_vaddr = TO_VADDR(kp->pa);
			if (ksm_pages_equal(page_vaddr, shared_vaddr) == 0)
				return kp;
		}
		kp = kp->next;
	}

	return 0;
}

static struct ksm_page *ksm_hash_lookup_pa(paddr_t pa)
{
	unsigned int i;

	/* 共享页可能已被错误写入，不能用当前内容重算哈希桶。 */
	for (i = 0; i < KSM_HASH_SIZE; i++) {
		struct ksm_page *kp;

		for (kp = ksm_hashtab[i]; kp; kp = kp->next)
			if (kp->pa == pa && kp->status == KSM_PAGE_SHARED)
				return kp;
	}
	return 0;
}

/*
 * 将一个普通页转换为共享页（首次加入哈希表）
 *   pa: 物理页地址
 *   hash: 页面内容哈希
 *   返回 ksm_page 结构体
 */
static struct ksm_page *ksm_make_shared(paddr_t pa, uint32_t hash)
{
	struct ksm_page *kp = ksm_page_alloc();
	if (!kp)
		return 0;

	kp->pa = pa;
	kp->hash = hash;
	kp->refcount = 1;
	kp->status = KSM_PAGE_SHARED;

	ksm_hash_insert(kp);
	ksm_global_stats.pages_shared++;

	return kp;
}

/*
 * 将一个 VM 的页面合并到共享页
 *   task: 目标 VM
 *   ipa: 该页的 IPA
 *   kp: 目标共享页
 *   返回 0 成功
 */
static int ksm_merge_page(struct task_struct *task, vaddr_t ipa, struct ksm_page *kp)
{
	paddr_t old_pa;
	uint64_t *l3_entry_p;
	uint64_t entry;
	int idx1, idx2, idx3;
	paddr_t l1_table, l2_table_pa, l3_table_pa;

	ipa &= PAGE_MASK;
	l1_table = task->mm.first_table;

	/* Level 1 */
	idx1 = (ipa >> LV1_SHIFT) & (PTRS_PER_TABLE - 1);
	entry = ((uint64_t *)TO_VADDR(l1_table))[idx1];
	if ((entry & 0x3) != 3)
		return -1;
	l2_table_pa = entry & MM_STAGE2_PA_MASK;

	/* Level 2 */
	idx2 = (ipa >> LV2_SHIFT) & (PTRS_PER_TABLE - 1);
	entry = ((uint64_t *)TO_VADDR(l2_table_pa))[idx2];
	if ((entry & 0x3) != 3)  /* 只处理小页，大页跳过 */
		return -1;
	l3_table_pa = entry & MM_STAGE2_PA_MASK;

	/* Level 3 */
	idx3 = (ipa >> PAGE_SHIFT) & (PTRS_PER_TABLE - 1);
	l3_entry_p = &((uint64_t *)TO_VADDR(l3_table_pa))[idx3];
	entry = *l3_entry_p;

	if ((entry & 0x3) != 0x3)  /* 不是有效页 */
		return -1;

	old_pa = entry & MM_STAGE2_PA_MASK;
	if (old_pa == kp->pa)  /* 已经是共享页了 */
		return -1;

	/* 修改映射：指向共享页，设置为只读（RO）+ KSM 标记 */
	entry = kp->pa;
	entry |= MM_TYPE_PAGE;         /* valid + page */
	entry |= MM_STAGE2_ACCESS;     /* AF=1 */
	entry |= MM_STAGE2_SH;         /* SH=inner shareable */
	entry |= S2_AP_RO;             /* RO（只读，触发 CoW） */
	entry |= MM_STAGE2_MEMATTR;    /* Normal memory */
	entry |= MM_STAGE2_KSM_SHARED; /* KSM 共享页标记 */

	*l3_entry_p = entry;

	/* 释放旧的物理页（该页内容已合并到共享页） */
	deallocate_page(TO_VADDR(old_pa));
	task->mm.user_pages_count--;

	/* 共享页引用计数 +1 */
	kp->refcount++;
	ksm_global_stats.pages_sharing++;
	ksm_global_stats.pages_merged++;

	/* 刷新 TLB */
	asm volatile("tlbi vmalle1" ::: "memory");
	asm volatile("dsb ish" ::: "memory");
	asm volatile("isb" ::: "memory");

	return 0;
}

/*
 * CoW 分离：将共享页分离为私有可写页
 */
int ksm_cow_break(struct task_struct *task, vaddr_t ipa)
{
	paddr_t shared_pa;
	uint64_t *l3_entry_p;
	uint64_t entry;
	int idx1, idx2, idx3;
	paddr_t l1_table, l2_table_pa, l3_table_pa;
	paddr_t new_pa;
	void *shared_vaddr, *new_vaddr;
	struct ksm_page *kp;
	uint32_t hash;

	ipa &= PAGE_MASK;
	l1_table = task->mm.first_table;

	/* 遍历页表找到 L3 entry */
	idx1 = (ipa >> LV1_SHIFT) & (PTRS_PER_TABLE - 1);
	entry = ((uint64_t *)TO_VADDR(l1_table))[idx1];
	if ((entry & 0x3) != 3)
		return -1;
	l2_table_pa = entry & MM_STAGE2_PA_MASK;

	idx2 = (ipa >> LV2_SHIFT) & (PTRS_PER_TABLE - 1);
	entry = ((uint64_t *)TO_VADDR(l2_table_pa))[idx2];
	if ((entry & 0x3) != 3)
		return -1;
	l3_table_pa = entry & MM_STAGE2_PA_MASK;

	idx3 = (ipa >> PAGE_SHIFT) & (PTRS_PER_TABLE - 1);
	l3_entry_p = &((uint64_t *)TO_VADDR(l3_table_pa))[idx3];
	entry = *l3_entry_p;

	if ((entry & 0x3) != 0x3)
		return -1;

	shared_pa = entry & MM_STAGE2_PA_MASK;

	/* 检查是否是共享页（在哈希表中查找） */
	/* 先算内容哈希来找 ksm_page（因为哈希表按内容哈希索引） */
	shared_vaddr = TO_VADDR(shared_pa);
	hash = ksm_page_hash(shared_vaddr);

	kp = ksm_hash_lookup(hash, shared_vaddr);
	if (!kp) {
		/* 不在哈希表中，可能是普通只读页（mprotect 设置的） */
		return -1;
	}

	/* 引用计数为 1 时，直接变回可写（不用分配新页） */
	if (kp->refcount == 1) {
		/* 变回 RW，并清除 KSM 共享标记 */
		entry &= ~(S2_AP_MASK | MM_STAGE2_KSM_SHARED);
		entry |= S2_AP_RW;
		*l3_entry_p = entry;

		/* 从哈希表移除，变回普通页 */
		ksm_hash_remove(kp);
		ksm_global_stats.pages_shared--;
		ksm_global_stats.pages_sharing--;
		ksm_page_free(kp);

		asm volatile("tlbi vmalle1" ::: "memory");
		asm volatile("dsb ish" ::: "memory");
		asm volatile("isb" ::: "memory");
		return 0;
	}

	/* 引用计数 > 1，需要分配新页并拷贝 */
	new_vaddr = allocate_page();
	if (!new_vaddr)
		return -1;
	new_pa = TO_PADDR(new_vaddr);

	memcpy(new_vaddr, shared_vaddr, PAGE_SIZE);

	/* 修改映射：指向新页，RW */
	entry = new_pa;
	entry |= MM_TYPE_PAGE;
	entry |= MM_STAGE2_ACCESS;
	entry |= MM_STAGE2_SH;
	entry |= S2_AP_RW;
	entry |= MM_STAGE2_MEMATTR;

	*l3_entry_p = entry;
	task->mm.user_pages_count++;

	/* 共享页引用计数 -1 */
	kp->refcount--;
	ksm_global_stats.pages_sharing--;
	ksm_global_stats.pages_unmerged++;

	asm volatile("tlbi vmalle1" ::: "memory");
	asm volatile("dsb ish" ::: "memory");
	asm volatile("isb" ::: "memory");

	return 0;
}

/*
 * 扫描单个 VM 的所有页面，执行 KSM 合并
 */
int ksm_scan_vm(struct task_struct *task, int max_scan_pages)
{
	paddr_t l1_table, l2_table_pa, l3_table_pa;
	uint64_t *l1, *l2, *l3;
	int i1, i2, i3;
	int merged = 0;
	int scanned = 0;
	vaddr_t ipa;

	if (!task || !task->mm.first_table)
		return 0;

	l1_table = task->mm.first_table;
	l1 = (uint64_t *)TO_VADDR(l1_table);

	for (i1 = 0; i1 < PTRS_PER_TABLE &&
	     (max_scan_pages == 0 || scanned < max_scan_pages); i1++) {
		uint64_t e1 = l1[i1];
		if ((e1 & 0x3) != 3)
			continue;  /* L1 无效 */

		l2_table_pa = e1 & MM_STAGE2_PA_MASK;
		l2 = (uint64_t *)TO_VADDR(l2_table_pa);

		for (i2 = 0; i2 < PTRS_PER_TABLE &&
		     (max_scan_pages == 0 || scanned < max_scan_pages); i2++) {
			uint64_t e2 = l2[i2];
			if ((e2 & 0x3) != 3)
				continue;  /* 不是 table descriptor（无效/block/reserved 都跳过） */

			/* L2 是 table，继续遍历 L3 */
			l3_table_pa = e2 & MM_STAGE2_PA_MASK;
			l3 = (uint64_t *)TO_VADDR(l3_table_pa);

			for (i3 = 0; i3 < PTRS_PER_TABLE &&
			     (max_scan_pages == 0 || scanned < max_scan_pages); i3++) {
				uint64_t e3 = l3[i3];
				paddr_t pa;
				void *vaddr;
				uint32_t hash;
				struct ksm_page *existing;

				if ((e3 & 0x3) != 0x3)
					continue;  /* 无效或非 page */

				/* 跳过 device memory 页（MMIO，不能做内容去重） */
				if ((e3 & (0x7 << 2)) != MM_STAGE2_MEMATTR)
					continue;

				/* 只读页可能已经是共享页，跳过 */
				if ((e3 & S2_AP_MASK) == S2_AP_RO)
					continue;

				/* 活跃 SMP 控制页必须保持可写，不参与 KSM 合并。 */
				if ((i1 << LV1_SHIFT | i2 << LV2_SHIFT |
				     i3 << PAGE_SHIFT) == 0x40300000UL)
					continue;

				pa = e3 & MM_STAGE2_PA_MASK;

				vaddr = TO_VADDR(pa);

				/* 计算哈希 */
				hash = ksm_page_hash(vaddr);

				/* 查找是否已有相同内容的共享页 */
				existing = ksm_hash_lookup(hash, vaddr);
				if (existing) {
					/* 找到相同页，合并 */
					ipa = (i1 << LV1_SHIFT) |
					      (i2 << LV2_SHIFT) |
					      (i3 << PAGE_SHIFT);

					if (ksm_merge_page(task, ipa, existing) == 0) {
						merged++;
					}
				} else {
					/* 没找到，将当前页转为共享页 */
					struct ksm_page *kp = ksm_make_shared(pa, hash);
					if (kp) {
						/* 将该页映射改为只读 + KSM 标记 */
						e3 &= ~S2_AP_MASK;
						e3 |= S2_AP_RO;
						e3 |= MM_STAGE2_KSM_SHARED;
						l3[i3] = e3;

						asm volatile("tlbi vmalle1" ::: "memory");
						asm volatile("dsb ish" ::: "memory");
						asm volatile("isb" ::: "memory");
					}
				}

				scanned++;
			}
		}
	}

	if (merged > 0) {
		ksm_global_stats.full_scans++;
	}

	return merged;
}

/* 获取 KSM 统计 */
void ksm_get_stats(struct ksm_stats *stats)
{
	if (stats)
		memcpy(stats, &ksm_global_stats, sizeof(*stats));
}

/*
 * Phase 34: 释放指定 VM 对 KSM 共享页的全部引用。
 *
 * 遍历 VM 的 Stage-2 页表，对每个带 MM_STAGE2_KSM_SHARED 标志的
 * 映射递减对应 ksm_page 的引用计数；refcount 归 0 时释放共享物理页
 * 与 ksm_page 结构体。最后清零 L3 entry。
 *
 * 返回处理的共享映射数，-1 失败。
 */
int ksm_drop_vm(struct task_struct *task)
{
	paddr_t l1_table, l2_table_pa, l3_table_pa;
	uint64_t *l1, *l2, *l3;
	int i1, i2, i3;
	int dropped = 0;
	int failed = 0;

	if (!task || !task->mm.first_table)
		return -1;

	l1_table = task->mm.first_table;
	l1 = (uint64_t *)TO_VADDR(l1_table);

	for (i1 = 0; i1 < PTRS_PER_TABLE; i1++) {
		uint64_t e1 = l1[i1];
		if ((e1 & 0x3) != 3)
			continue;

		l2_table_pa = e1 & MM_STAGE2_PA_MASK;
		l2 = (uint64_t *)TO_VADDR(l2_table_pa);

		for (i2 = 0; i2 < PTRS_PER_TABLE; i2++) {
			uint64_t e2 = l2[i2];
			if ((e2 & 0x3) != 3)
				continue;

			l3_table_pa = e2 & MM_STAGE2_PA_MASK;
			l3 = (uint64_t *)TO_VADDR(l3_table_pa);

			for (i3 = 0; i3 < PTRS_PER_TABLE; i3++) {
				uint64_t e3 = l3[i3];
				paddr_t shared_pa;
				struct ksm_page *kp;

				if ((e3 & 0x3) != 0x3)
					continue;
				if (!(e3 & MM_STAGE2_KSM_SHARED))
					continue;

				shared_pa = e3 & MM_STAGE2_PA_MASK;
				kp = ksm_hash_lookup_pa(shared_pa);
				if (!kp) {
					failed = 1;
					continue;
				}

				/* pages_sharing 只统计规范页之外的额外映射。 */
				if (kp->refcount > 1)
					ksm_global_stats.pages_sharing--;
				kp->refcount--;
				if (kp->refcount == 0) {
					ksm_hash_remove(kp);
					ksm_global_stats.pages_shared--;
					deallocate_page((void *)TO_VADDR(shared_pa));
					ksm_page_free(kp);
				}

				l3[i3] = 0;
				dropped++;
			}
		}
	}

	if (dropped > 0) {
		asm volatile("tlbi vmalle1" ::: "memory");
		asm volatile("dsb ish" ::: "memory");
		asm volatile("isb" ::: "memory");
	}

	return failed ? -1 : dropped;
}

/* 初始化 KSM */
void ksm_init(void)
{
	int i;

	ksm_page_pool_init();

	for (i = 0; i < KSM_HASH_SIZE; i++)
		ksm_hashtab[i] = 0;

	memset(&ksm_global_stats, 0, sizeof(ksm_global_stats));

	INFO("KSM: initialized (hashtab=%d buckets, max_pages=%d)",
	     KSM_HASH_SIZE, KSM_MAX_PAGES);
}
