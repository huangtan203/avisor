// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - KSM (Kernel Samepage Merging)
 *
 * Phase 24: 内存去重 - 扫描内容相同的页面，合并为共享只读页，
 *           写入时触发写时复制（CoW）分离。
 *
 * 核心数据结构：
 *   - ksm_page: 每个被 KSM 管理的物理页
 *   - ksm_hashtab: 全局哈希表，按内容哈希索引，快速查找相同页
 *
 * 合并流程：
 *   1. 扫描 VM 的 Stage-2 页表，收集候选页
 *   2. 计算页面内容哈希
 *   3. 在哈希表中查找相同哈希的页
 *   4. 逐字节比较内容（防止哈希碰撞）
 *   5. 内容相同 → 合并：将当前页映射改为指向共享页（只读）
 *   6. 释放被合并的物理页
 *
 * CoW 分离流程：
 *   1. Guest 写共享页 → Stage-2 permission fault
 *   2. 分配新物理页，拷贝内容
 *   3. 重新映射为 RW
 *   4. 共享页引用计数 -1
 *   5. 引用计数为 0 时释放共享页
 */

#pragma once

#include <common/sched.h>
#include <common/mm.h>

/* KSM 哈希表大小（bucket 数），必须是 2 的幂 */
#define KSM_HASH_SIZE  4096

/* KSM 页面状态 */
#define KSM_PAGE_NORMAL   0  /* 普通页，未被 KSM 管理 */
#define KSM_PAGE_SHARED   1  /* 共享页（被多个映射指向，只读） */
#define KSM_PAGE_COW      2  /* CoW 页（只读，写入时触发分离） */

/*
 * KSM 页描述符
 *
 * 每个被 KSM 管理的物理页对应一个 ksm_page 结构体。
 * 用哈希表按内容哈希索引，同一 bucket 内用链表串联。
 */
struct ksm_page {
	struct ksm_page *next;      /* 哈希链表下一节点 */
	paddr_t pa;                 /* 物理页地址（4KB 对齐） */
	uint32_t hash;              /* 页面内容哈希值 */
	int refcount;               /* 引用计数（多少个 Stage-2 映射指向此页） */
	int status;                 /* KSM_PAGE_SHARED / KSM_PAGE_COW */
};

/* KSM 全局统计 */
struct ksm_stats {
	int pages_shared;     /* 共享页数量（物理页） */
	int pages_sharing;    /* 共享映射数（有多少个映射指向共享页） */
	int pages_merged;     /* 累计合并页数 */
	int pages_unmerged;   /* 累计分离页数（CoW 触发的） */
	int full_scans;       /* 完整扫描次数 */
};

/* 初始化 KSM 子系统 */
void ksm_init(void);

/*
 * 计算页面内容的哈希值（FNV-1a 32-bit）
 *   page_addr: 虚拟地址（必须映射到当前地址空间）
 *   返回 32-bit 哈希值
 */
uint32_t ksm_page_hash(const void *page_addr);

/*
 * 比较两个页面内容是否完全相同
 *   返回 0 表示相同，非 0 表示不同
 */
int ksm_pages_equal(const void *page1, const void *page2);

/*
 * 扫描指定 VM 的所有用户页，执行 KSM 合并
 *   task: 目标 VM
 *   max_scan_pages: 最多扫描多少页（0 = 全部扫描）
 *   返回实际合并的页数
 */
int ksm_scan_vm(struct task_struct *task, int max_scan_pages);

/*
 * 检查指定物理页是否是 KSM 共享页
 *   pa: 物理页地址
 *   返回 1 = 共享页，0 = 普通页
 */
int ksm_is_shared_page(paddr_t pa);

/*
 * CoW 分离：将共享页分离为私有可写页
 *   task: 目标 VM
 *   ipa: 中间物理地址（Stage-2 映射地址）
 *   返回 0 成功，-1 失败
 */
int ksm_cow_break(struct task_struct *task, vaddr_t ipa);

/* 获取 KSM 统计信息 */
void ksm_get_stats(struct ksm_stats *stats);
