// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - Live Migration（热迁移）
 *
 * Phase 25: 脏页跟踪 + 状态保存/恢复 + 迭代预拷贝
 *
 * 迁移模型（pre-copy）：
 *   - migration_start    : 清除源 VM 全部 Stage-2 AF 位 + 全量拷贝内存
 *   - migration_precopy  : 收集 AF=1 的脏页并拷贝（可多轮迭代收敛）
 *   - migration_finish   : 最后一轮脏页拷贝 + 转移 vCPU 状态 + 停止源 VM
 *                          并调度到目标 VM 恢复执行
 *
 * 脏页跟踪复用 Stage-2 描述符的 AF 位（bit 10，MM_STAGE2_ACCESS）：
 * 清除 AF 后，Guest 再次访问该页会触发 Access Flag Fault，由
 * handle_access_flag_fault 自动置位 AF；迁移时收集 AF=1 的页即为
 * "自上次清 AF 后被访问过"的脏页（这是 KVM 早期经典的 AF 跟踪法）。
 *
 * 说明：AF 位跟踪的是"访问"而非"写"，因此读过的页也会被重复拷贝，
 * 这是 AF 方案的固有超集行为，仅带来轻微额外拷贝，不影响正确性。
 */

#include "common/migration.h"
#include "common/utils.h"
#include "common/task.h"
#include "common/smp.h"
#include "common/ksm.h"
#include "common/debug.h"
#include "arch/aarch64/mmu.h"

#ifdef PLATFORM_VIRT
#include "arch/aarch64/sysregs.h"
#include "boards/virt/vgic.h"
#include "boards/virt/virt_board.h"
#include "boards/virt/shmem.h"
#endif

/* 迁移目标 VM（main_virt.c 创建 dormant VM3 后通过 migration_set_dest 注册） */
static struct task_struct *mig_dst;
static enum migration_state mig_state = MIGRATION_IDLE;

/* FINISH 时捕获的源 VM pt_regs（ERET 现场），目标 VM 首次调度时恢复 */
static struct pt_regs mig_saved_regs;

/* Phase 34: 源 VM 回收统计 */
static int mig_reclaim_pages;
static int mig_reclaim_shm;
static int mig_reclaim_ksm;
static int mig_reclaim_vm_done;

/* 迁移遍历操作 */
enum {
	MIG_OP_CLEAR_AF,     /* 清除全部 AF 位（开启脏页跟踪） */
	MIG_OP_FULL_COPY,    /* 全量拷贝所有映射页 */
	MIG_OP_COLLECT_DIRTY,/* 收集并拷贝 AF=1 的脏页，同时清 AF 供下一轮 */
};

/*
 * 拷贝单个 4KB 页：src_pa -> 目标 VM 的 ipa。
 *
 * 多轮预拷贝不泄漏的关键：先判断目标 VM 该 ipa 是否已映射。
 * 已映射则复用原物理页 memcpy（覆盖旧内容），否则新分配一页并建映射。
 */
static void mig_copy_page(struct task_struct *src, vaddr_t ipa, paddr_t src_pa)
{
	paddr_t dst_pa;

	ipa &= PAGE_MASK;

	if (ipa_to_pa(mig_dst, ipa, &dst_pa) == 0) {
		/* 目标已映射：复用（覆盖内容），避免预拷贝每轮泄漏一页 */
		memcpy((void *)TO_VADDR(dst_pa), (void *)TO_VADDR(src_pa),
		       PAGE_SIZE);
	} else {
		void *dst_va = allocate_page();
		if (!dst_va)
			return;
		memcpy(dst_va, (void *)TO_VADDR(src_pa), PAGE_SIZE);
		map_stage2_page(mig_dst, ipa, TO_PADDR(dst_va),
				MMU_STAGE2_PAGE_FLAGS);
	}
}

/* 拷贝一个 2MB block（拆成 512 个 4KB 页，目标统一用 4KB 小页） */
static void mig_copy_block(struct task_struct *src, vaddr_t ipa, paddr_t src_pa)
{
	int npages = 1 << (LV2_SHIFT - PAGE_SHIFT);

	for (int i = 0; i < npages; i++)
		mig_copy_page(src, ipa + i * PAGE_SIZE,
			      src_pa + i * PAGE_SIZE);
}

/*
 * 遍历源 VM 的 Stage-2 页表（L1 -> L2 -> L3），按 op 执行对应操作。
 * 返回 MIG_OP_COLLECT_DIRTY 时累计的脏页数，其余操作返回 0。
 */
static int mig_walk(struct task_struct *src, int op)
{
	uint64_t *l1, *l2, *l3;
	int i1, i2, i3;
	int dirty = 0;
	int touched = 0;  /* 是否清过 AF 位（决定是否需要 TLB flush） */

	if (!src || !src->mm.first_table)
		return 0;

	l1 = (uint64_t *)TO_VADDR(src->mm.first_table);

	for (i1 = 0; i1 < PTRS_PER_TABLE; i1++) {
		uint64_t e1 = l1[i1];
		if ((e1 & 0x3) != 3)
			continue;  /* L1 无效 / 1GB block（本项目不产生） */

		l2 = (uint64_t *)TO_VADDR(e1 & MM_STAGE2_PA_MASK);

		for (i2 = 0; i2 < PTRS_PER_TABLE; i2++) {
			uint64_t e2 = l2[i2];
			vaddr_t ipa_base;

			if ((e2 & 0x3) == 1) {
				/* L2 block（2MB 大页） */
				/* 跳过 device memory（MMIO 不可拷贝） */
				if ((e2 & (0x7 << 2)) != MM_STAGE2_MEMATTR)
					continue;

				ipa_base = ((vaddr_t)i1 << LV1_SHIFT) |
					   ((vaddr_t)i2 << LV2_SHIFT);

				if (op == MIG_OP_CLEAR_AF) {
					if (e2 & MM_STAGE2_ACCESS) {
						l2[i2] = e2 & ~MM_STAGE2_ACCESS;
						touched = 1;
					}
				} else if (op == MIG_OP_FULL_COPY) {
					mig_copy_block(src, ipa_base,
						       e2 & MM_STAGE2_PA_MASK);
				} else { /* COLLECT_DIRTY */
					if (e2 & MM_STAGE2_ACCESS) {
						mig_copy_block(src, ipa_base,
							       e2 & MM_STAGE2_PA_MASK);
						l2[i2] = e2 & ~MM_STAGE2_ACCESS;
						dirty += (1 << (LV2_SHIFT -
								PAGE_SHIFT));
						touched = 1;
					}
				}
				continue;
			}

			if ((e2 & 0x3) != 3)
				continue;  /* L2 空 entry */

			l3 = (uint64_t *)TO_VADDR(e2 & MM_STAGE2_PA_MASK);

			for (i3 = 0; i3 < PTRS_PER_TABLE; i3++) {
				uint64_t e3 = l3[i3];
				vaddr_t ipa;

				if ((e3 & 0x3) != 3)
					continue;  /* L3 无效 */

				/* 跳过 device memory（MMIO 不可拷贝） */
				if ((e3 & (0x7 << 2)) != MM_STAGE2_MEMATTR)
					continue;

				ipa = ((vaddr_t)i1 << LV1_SHIFT) |
				      ((vaddr_t)i2 << LV2_SHIFT) |
				      ((vaddr_t)i3 << PAGE_SHIFT);

				if (op == MIG_OP_CLEAR_AF) {
					if (e3 & MM_STAGE2_ACCESS) {
						l3[i3] = e3 & ~MM_STAGE2_ACCESS;
						touched = 1;
					}
				} else if (op == MIG_OP_FULL_COPY) {
					mig_copy_page(src, ipa,
						      e3 & MM_STAGE2_PA_MASK);
				} else { /* COLLECT_DIRTY */
					if (e3 & MM_STAGE2_ACCESS) {
						mig_copy_page(src, ipa,
							      e3 & MM_STAGE2_PA_MASK);
						l3[i3] = e3 & ~MM_STAGE2_ACCESS;
						dirty++;
						touched = 1;
					}
				}
			}
		}
	}

	/* 清 AF 后统一失效 Stage-2 TLB，保证下次访问触发 AF Fault */
	if (touched) {
		asm volatile("tlbi vmalle1" ::: "memory");
		asm volatile("dsb ish" ::: "memory");
		asm volatile("isb" ::: "memory");
	}

	return dirty;
}

enum migration_state migration_get_state(void)
{
	return mig_state;
}

/* 注册迁移目标 VM */
void migration_set_dest(struct task_struct *dst)
{
	mig_dst = dst;
}

/* 迁移阶段一：清 AF（开脏页跟踪）+ 全量拷贝 */
int migration_start(struct task_struct *src)
{
	if (mig_state == MIGRATION_PRECOPY)
		return -1;

	if (!src || !src->mm.first_table || !mig_dst || src == mig_dst ||
	    src->state != TASK_RUNNING || mig_dst->state != TASK_WAITING ||
	    src->vmid == mig_dst->vmid) {
		mig_state = MIGRATION_REJECTED;
		return -1;
	}

	/*
	 * Phase 33: 双 vCPU 迁移。
	 *
	 * Phase 25 只支持单 vCPU 源（多 vCPU 直接 REJECTED）；Phase 33
	 * 起源 VM 可为双 vCPU：START/PRECOPY 阶段源 VM 仍在执行，两个
	 * vCPU 都写脏页；FINISH 阶段才在 VM 级冻结窗口内做 stop-and-copy
	 * + 双 vCPU 状态转移。因此这里不再拒绝多 vCPU 源。
	 */
	mig_state = MIGRATION_PRECOPY;

	mig_walk(src, MIG_OP_CLEAR_AF);
	mig_walk(src, MIG_OP_FULL_COPY);

	return 0;
}

/* 迁移阶段二：迭代预拷贝，返回本轮拷贝的脏页数 */
int migration_precopy(struct task_struct *src)
{
	if (!src || mig_state != MIGRATION_PRECOPY || !mig_dst ||
	    src->state != TASK_RUNNING || mig_dst->state != TASK_WAITING)
		return -1;

	return mig_walk(src, MIG_OP_COLLECT_DIRTY);
}

/*
 * Phase 34: 回收源 VM 的全部资源。
 *
 * 在 FINISH 转移状态后调用，释放 VM2 独占资源：
 *   1. shm_detach_vm   —— 清除跨 VM 共享内存 attach（不释放物理页）
 *   2. ksm_drop_vm     —— 释放 KSM 共享页引用（refcount 归 0 才释放页）
 *   3. virt_destroy_vm —— 注销 ivshmem peer + 释放 board_data 页
 *   4. destroy_vm_pages—— 释放普通页 + 页表页 + first_table
 *
 * 顺序关键：先清共享资源引用（避免 destroy_vm_pages 误释放共享页），
 * 再销毁设备模型与页表。
 */
static void migration_reclaim_src(struct task_struct *src)
{
	if (!src)
		return;

	mig_reclaim_pages = -1;
	mig_reclaim_shm = 0;
	mig_reclaim_ksm = 0;
	mig_reclaim_vm_done = 0;

#ifdef PLATFORM_VIRT
	mig_reclaim_shm = shm_detach_vm(src);
	mig_reclaim_ksm = ksm_drop_vm(src);
	virt_destroy_vm(src);
	mig_reclaim_pages = destroy_vm_pages(src);
	mig_reclaim_vm_done = (mig_reclaim_shm >= 0 &&
				mig_reclaim_ksm >= 0 &&
				mig_reclaim_pages >= 0);
#else
	mig_reclaim_pages = destroy_vm_pages(src);
	mig_reclaim_vm_done = (mig_reclaim_pages >= 0);
#endif

	INFO("migration: source VM reclaimed (pages=%d shm=%d ksm=%d done=%d)",
	     mig_reclaim_pages, mig_reclaim_shm, mig_reclaim_ksm,
	     mig_reclaim_vm_done);
}

/* Phase 34: 查询源 VM 回收统计 */
void migration_reclaim_stats(int *pages, int *shm, int *ksm, int *done)
{
	if (pages)
		*pages = mig_reclaim_pages;
	if (shm)
		*shm = mig_reclaim_shm;
	if (ksm)
		*ksm = mig_reclaim_ksm;
	if (done)
		*done = mig_reclaim_vm_done;
}

/*
 * 迁移阶段三：stop-and-copy + 状态转移 + 调度到目标 VM。
 *
 * Phase 33：双 vCPU 全迁移。本函数在 VM2 vCPU0 的 HVC handler 内被
 * 调用（协调核，CPU0）。与 Phase 25 单 vCPU 版不同，这里借助
 * Phase 32 的 VM 级事务冻结窗口保证"整 VM 静止"后再做 stop-and-copy：
 *
 *   1. smp_vm_txn_begin()  冻结 vCPU1（CPU1 上 SGI#5 忙等）并捕获
 *                           vCPU0 自身快照 → 整 VM 静止
 *   2. 最后一轮脏页拷贝（两 vCPU 都不再写脏页）
 *   3. 就地改造 vCPU1：vmid 2→3、mm/board_data 挂到 VM3、重新注册
 *   4. 转移 vCPU0 状态到 VM3 vCPU0（沿用 Phase 25 单核逻辑）
 *   5. smp_vm_txn_commit() 释放 vCPU1：它从冻结点恢复，以 VM3 身份
 *                          继续执行（vm_entering_work 检测 vmid 变化
 *                          自动切 VTTBR + 失效 TLB）
 *   6. vCPU0 自身 schedule() 切到 VM3 vCPU0
 *
 * 返回值（脏页数）通过 mig_saved_regs.regs[0] 传给恢复后的目标 VM，
 * 本函数不会返回（schedule() 切走后源 VM 进入 ZOMBIE）。
 */
int migration_finish(struct task_struct *src)
{
	int dirty;
	struct task_struct *vcpu1;

	if (!src || mig_state != MIGRATION_PRECOPY || !mig_dst ||
	    src->state != TASK_RUNNING || mig_dst->state != TASK_WAITING)
		return -1;

	/* 1. 冻结整 VM（vCPU1 在 CPU1 忙等，vCPU0 捕获自身快照） */
	if (smp_vm_txn_begin() != 0)
		return -1;

	/* 2. 最后一轮脏页拷贝：此刻两 vCPU 都静止，拷贝收敛 */
	dirty = mig_walk(src, MIG_OP_COLLECT_DIRTY);

	/* 3. 就地改造 vCPU1（冻结中的 VM2 vCPU1 → VM3 vCPU1） */
	vcpu1 = smp_vm_txn_target_task();
	if (vcpu1) {
		vcpu1->vmid = mig_dst->vmid;
		vcpu1->mm = mig_dst->mm;
		vcpu1->board_data = mig_dst->board_data;
		(void)strncpy(vcpu1->name, "vm3-cpu1", 36);
#ifdef PLATFORM_VIRT
		virt_register_vcpu(vcpu1, (int)mig_dst->vmid, vcpu1->vcpu_id);
#endif
	}

	/* 4. 转移 vCPU0 系统寄存器（含 SP_EL1/ELR_EL1/SPSR/TTBR 等 EL1 全量） */
	memcpy(&mig_dst->cpu_sysregs, &src->cpu_sysregs,
	       sizeof(struct cpu_sysregs));

#ifdef PLATFORM_VIRT
	/* 5. 新鲜捕获源 VM 的 vGIC + vtimer 状态（timer 不在 save_sysregs 里，
	 *    运行期真实状态在 tsk->virt，必须在此重新读取硬件） */
	vgic_save_state(&src->virt);
	src->virt.cntv_cval = READ_SYSREG64(CNTV_CVAL_EL0);
	{
		uint64_t ctl = READ_SYSREG64(CNTV_CTL_EL0);
		src->virt.cntv_ctl = ctl;
		if (ctl & CNTV_CTL_ISTATUS)
			src->virt.vtimer_pending = 1;
	}

	/* 6. 转移板级 vGIC MMIO 状态（GICD/GICR trap-and-emulate 建模） */
	virt_copy_vgic_state(mig_dst, src);
#endif

	/* 7. 转移 per-vCPU 虚拟化状态（gic_vmcr/gic_lr/cntv/vtimer_pending） */
	memcpy(&mig_dst->virt, &src->virt, sizeof(struct vm_virt_state));

	/* 8. 转移虚拟计数器偏移（保证迁移后 Guest 的 CNTVCT 连续） */
	mig_dst->mm.cntvoff = src->mm.cntvoff;

	/* 9. 捕获 vCPU0 的 ERET 现场（HVC 返回后 pc 已指向 HVC+4） */
	memcpy(&mig_saved_regs, task_pt_regs(src), sizeof(struct pt_regs));
	mig_saved_regs.regs[0] = (unsigned long)dirty;

	/* 10. 释放 vCPU1：它以 VM3 身份从冻结点恢复执行 */
	smp_vm_txn_commit();

#ifdef PLATFORM_VIRT
	/* 10b. Phase 34: 回收源 VM2 资源。
	 *
	 * vCPU1 已就地改造为 VM3 vCPU1（mm/board_data 指向 VM3），
	 * 不再引用 VM2 的任何资源；此处清理 VM2 独占资源：
	 *   - vm_vcpus 查找表（[1][0] = src，[1][1] = vCPU1 残留槽）
	 *   - 设备模型 + ivshmem peer + 跨 VM shm + KSM 共享引用
	 *   - Stage-2 页表与普通用户页
	 * 共享物理页（ivshmem shmem / shm）不会被误释放。
	 */
	virt_unregister_vcpu(2, 0);
	virt_unregister_vcpu(2, 1);
	migration_reclaim_src(src);
#endif

	/* 11. 停止源 vCPU0，唤醒目标 VM3 vCPU0 并立即调度过去 */
	src->state = TASK_ZOMBIE;
	mig_dst->state = TASK_RUNNING;
	mig_dst->priority = src->priority;
	mig_dst->counter = 1000;
	mig_state = MIGRATION_COMPLETED;

	schedule();

	/* 不会执行到这里：schedule() 已切换到目标 VM */
	return dirty;
}

/*
 * 目标 VM 的 resume loader：首次被调度时恢复迁移过来的 ERET 现场。
 * prepare_task 会先设 pstate，这里 memcpy 覆盖为源 VM 的现场（含 pstate）。
 */
int migration_resume_loader(void *arg, struct pt_regs *regs)
{
	(void)arg;
	memcpy(regs, &mig_saved_regs, sizeof(struct pt_regs));
	return 0;
}
