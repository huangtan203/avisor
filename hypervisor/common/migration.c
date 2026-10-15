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
#include "arch/aarch64/mmu.h"

#ifdef PLATFORM_VIRT
#include "arch/aarch64/sysregs.h"
#include "boards/virt/vgic.h"
#include "boards/virt/virt_board.h"
#endif

/* 迁移目标 VM（main_virt.c 创建 dormant VM3 后通过 migration_set_dest 注册） */
static struct task_struct *mig_dst;
static int mig_active;

/* FINISH 时捕获的源 VM pt_regs（ERET 现场），目标 VM 首次调度时恢复 */
static struct pt_regs mig_saved_regs;

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

/* 注册迁移目标 VM */
void migration_set_dest(struct task_struct *dst)
{
	mig_dst = dst;
}

/* 迁移阶段一：清 AF（开脏页跟踪）+ 全量拷贝 */
int migration_start(struct task_struct *src)
{
	if (!mig_dst || mig_active)
		return -1;

	mig_active = 1;

	mig_walk(src, MIG_OP_CLEAR_AF);
	mig_walk(src, MIG_OP_FULL_COPY);

	return 0;
}

/* 迁移阶段二：迭代预拷贝，返回本轮拷贝的脏页数 */
int migration_precopy(struct task_struct *src)
{
	if (!mig_active || !mig_dst)
		return -1;

	return mig_walk(src, MIG_OP_COLLECT_DIRTY);
}

/*
 * 迁移阶段三：stop-and-copy + 状态转移 + 调度到目标 VM。
 *
 * 返回值（脏页数）通过 mig_saved_regs.regs[0] 传给恢复后的目标 VM，
 * 本函数本身不会返回（schedule() 切走后源 VM 进入 ZOMBIE）。
 */
int migration_finish(struct task_struct *src)
{
	int dirty;

	if (!mig_active || !mig_dst)
		return -1;

	/* 1. 最后一轮脏页拷贝 */
	dirty = mig_walk(src, MIG_OP_COLLECT_DIRTY);

	/* 2. 捕获 ERET 现场（HVC 返回后 pc 已指向 HVC+4，无需手动推进） */
	memcpy(&mig_saved_regs, task_pt_regs(src), sizeof(struct pt_regs));
	/* 把脏页数写进目标 VM 恢复后的 x0（handle_hvc64 写 x0 发生在本函数之后） */
	mig_saved_regs.regs[0] = (unsigned long)dirty;

	/* 3. 转移 vCPU 系统寄存器（含 SP_EL1/ELR_EL1/SPSR/TTBR 等 EL1 全量状态） */
	memcpy(&mig_dst->cpu_sysregs, &src->cpu_sysregs,
	       sizeof(struct cpu_sysregs));

#ifdef PLATFORM_VIRT
	/* 4. 新鲜捕获源 VM 的 vGIC + vtimer 状态（timer 不在 save_sysregs 里，
	 *    运行期真实状态在 tsk->virt，必须在此重新读取硬件） */
	vgic_save_state(&src->virt);
	src->virt.cntv_cval = READ_SYSREG64(CNTV_CVAL_EL0);
	{
		uint64_t ctl = READ_SYSREG64(CNTV_CTL_EL0);
		src->virt.cntv_ctl = ctl;
		if (ctl & CNTV_CTL_ISTATUS)
			src->virt.vtimer_pending = 1;
	}

	/* 5. 转移板级 vGIC MMIO 状态（GICD/GICR trap-and-emulate 建模） */
	virt_copy_vgic_state(mig_dst, src);
#endif

	/* 6. 转移 per-vCPU 虚拟化状态（gic_vmcr/gic_lr/cntv/vtimer_pending） */
	memcpy(&mig_dst->virt, &src->virt, sizeof(struct vm_virt_state));

	/* 7. 转移虚拟计数器偏移（保证迁移后 Guest 的 CNTVCT 连续） */
	mig_dst->mm.cntvoff = src->mm.cntvoff;

	/* 8. 停止源 VM，唤醒目标 VM 并立即调度过去 */
	src->state = TASK_ZOMBIE;
	mig_dst->state = TASK_RUNNING;
	mig_dst->priority = src->priority;
	mig_dst->counter = 1000;
	mig_active = 0;

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
