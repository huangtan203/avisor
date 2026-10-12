// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - QEMU virt 平台板级支持
 *
 * Phase 3：多 VM 分时复用下的世界切换保存/恢复。
 *
 * GICv3 虚拟接口（ICH_VMCR/ICH_LR）和架构虚拟定时器（CNTV_*）
 * 都是【per-CPU】硬件资源——它们属于"当前加载的 VM"。
 * 多个 VM 分时复用同一 CPU 时，若不在世界切换时保存/恢复，
 * VM A 的状态会泄漏给 VM B。
 *
 * 保存/恢复策略（Phase 3）：
 *   leaving_vm（每次离开 VM）：
 *     1. 保存 ICH_VMCR_EL2 / ICH_LR0_EL2 到 task->virt
 *     2. 若 LR0 是 vtimer 的 HW 映射条目且非 invalid：
 *        用 ICC_DIR_EL1 强制 deactivate 物理 PPI 27——
 *        防止 A 的 active 27 屏蔽 B 运行期间的 vtimer 信号
 *     3. 保存 CNTV_CVAL/CTL；若已到期未处理（ISTATUS=1）置
 *        vtimer_pending 标志（恢复时补注入）
 *     4. 禁用物理 CNTV（CTL=0）——VM 不在 CPU 上时其定时器
 *        不再产生物理信号，避免中断归属歧义
 *   entering_vm（每次回到 VM）：
 *     1. 恢复 CNTVOFF_EL2（虚拟时间隔离）
 *     2. 恢复 ICH_VMCR_EL2 / ICH_LR0_EL2
 *     3. 恢复 CNTV_CVAL/CTL——若离开期间虚拟时间已越过 CVAL，
 *        恢复 ENABLE 的瞬间 ISTATUS 立即置位，PPI 27 pending，
 *        EL2 会正常翻译注入给本 VM（时间自动"补课"）
 *     4. 若 vtimer_pending 置位：补注入 HW LR
 */

#include "boards/virt/virt_board.h"
#include "boards/virt/virt_mm.h"
#include "boards/virt/gicv3.h"
#include "boards/virt/vgic.h"
#include "arch/aarch64/sysregs.h"
#include "common/mm.h"
#include "common/types.h"
#include "common/debug.h"

/*
 * 页面分配位图
 */
static uint8_t virt_page_memap[PAGING_PAGES] = { 0 };

static struct page_pool virt_page_pool = {
	.start_addr = LOW_MEMORY,
	.page_nr = PAGING_PAGES,
	.memap = virt_page_memap,
	.last_page_id = 0,
};

struct page_pool *get_virt_page_pool(void)
{
	return &virt_page_pool;
}

/*
 * VM 创建时的初始化（create_task 中调用）
 */
static void virt_initialize(struct task_struct *tsk)
{
	tsk->mm.cntvoff = READ_SYSREG64(CNTPCT_EL0);
	tsk->virt.gic_vmcr = (0xF8UL << ICH_VMCR_VPMR_SHIFT) | ICH_VMCR_VENG1;
}

/*
 * 世界切换：VM 被调度出去时调用（switch_to 中，切换 current 之前）
 *
 * 此时 prev VM 仍在硬件上加载（所有 per-VM 寄存器还是 prev 的值），
 * 且 handle_irq 已经执行完毕（所有中断已 ack），可以安全地修改
 * 中断状态（禁用 CNTV、deactivate 物理中断等）。
 *
 * 步骤：
 *   1. 保存 ICH_VMCR / ICH_LR0
 *   2. 若 LR0 是 vtimer HW 映射且非 invalid → ICC_DIR deactivate 物理 27
 *   3. 保存 CNTV_CVAL / CNTV_CTL；若 ISTATUS=1 置 vtimer_pending
 *   4. 禁用 CNTV（CTL=0）——VM 不在 CPU 上时不产生物理信号
 */
static void virt_leaving_vm(struct task_struct *tsk)
{
	uint64_t lr0, ctl;

	/* 1. 保存虚拟 GIC 接口状态 */
	tsk->virt.gic_vmcr = READ_SYSREG64(ICH_VMCR_EL2);
	lr0 = READ_SYSREG64(ICH_LR0_EL2);
	tsk->virt.gic_lr0 = lr0;

	/*
	 * 2. 若 LR0 是 vtimer 的 HW 映射且非 invalid，
	 *    强制 deactivate 物理 PPI 27。
	 *
	 * 说明：如果 guest 正在处理 vtimer 中断（LR active，物理 27 active），
	 * 切换走时必须释放物理 27，否则下一个 VM 的 vtimer 永远无法触发
	 * （同优先级 active 屏蔽 pending）。
	 *
	 * 代价：本 VM 的 vtimer 中断 active 状态丢失，下次恢复时
	 * 通过 vtimer_pending 重新注入为 pending 状态——
	 * 定时器中断可重入，延迟一次影响不大。
	 */
	if ((lr0 & ICH_LR_HW) && (lr0 & ICH_LR_STATE_MASK)) {
		uint32_t pintid = (uint32_t)(lr0 >> 32) & 0x3ff;

		if (pintid == PPI_VIRT_TIMER) {
			WRITE_SYSREG64(PPI_VIRT_TIMER, ICC_DIR_EL1);
			asm volatile("isb");
		}
	}

	/* 3. 保存虚拟定时器状态 */
	tsk->virt.cntv_cval = READ_SYSREG64(CNTV_CVAL_EL0);
	ctl = READ_SYSREG64(CNTV_CTL_EL0);
	tsk->virt.cntv_ctl = ctl;

	/* 已到期未处理 → 记延迟注入标志 */
	if (ctl & CNTV_CTL_ISTATUS)
		tsk->virt.vtimer_pending = 1;

	/* 4. 禁用物理 CNTV：VM 不在 CPU 上时不产生信号 */
	WRITE_SYSREG64(0, CNTV_CTL_EL0);
	asm volatile("isb");
}

/*
 * 世界切换：VM 被调度进来时调用（switch_to 中，切换 current 之后）
 *
 * 此时 next 已经是 current，需要把它的虚拟化状态加载到硬件寄存器中。
 *
 * 步骤：
 *   1. 恢复 CNTVOFF（虚拟时间隔离）
 *   2. 恢复 ICH_VMCR / ICH_LR0
 *   3. 恢复 CNTV_CVAL / CNTV_CTL
 *   4. 若 vtimer_pending → 补注入 HW LR
 */
static void virt_entering_vm(struct task_struct *tsk)
{
	/* 1. 恢复虚拟时间偏移 */
	WRITE_SYSREG64(tsk->mm.cntvoff, CNTVOFF_EL2);

	/* 2. 恢复虚拟 GIC 接口状态 */
	WRITE_SYSREG64(tsk->virt.gic_vmcr, ICH_VMCR_EL2);
	WRITE_SYSREG64(tsk->virt.gic_lr0, ICH_LR0_EL2);

	/*
	 * 3. 恢复虚拟定时器。
	 *    若离开期间虚拟计数已越过 CVAL，恢复 ENABLE 的瞬间
	 *    ISTATUS 立即置位 → PPI 27 pending → EL2 正常翻译注入。
	 */
	WRITE_SYSREG64(tsk->virt.cntv_cval, CNTV_CVAL_EL0);
	WRITE_SYSREG64(tsk->virt.cntv_ctl, CNTV_CTL_EL0);
	asm volatile("isb");

	/* 4. 补注入离开前缓存的 vtimer 虚拟中断 */
	if (tsk->virt.vtimer_pending) {
		tsk->virt.vtimer_pending = 0;
		vgic_inject_virq_hw(PPI_VIRT_TIMER, PPI_VIRT_TIMER);
	}
}

const struct board_ops virt_board_ops = {
	.initialize = virt_initialize,
	.mmio_read = NULL,
	.mmio_write = NULL,
	.entering_vm = virt_entering_vm,
	.leaving_vm = virt_leaving_vm,
	.is_irq_asserted = NULL,
	.is_fiq_asserted = NULL,
	.debug = NULL,
};
