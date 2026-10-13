// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - GICv3 虚拟化支持 (vGIC)
 *
 * Phase 4 扩展：
 *   - 多 LR 管理（LR0: vtimer HW 映射, LR1+: 纯软件虚拟中断）
 *   - 纯软件注入 API（vgic_inject_virq）
 *   - 世界切换保存/恢复所有 LR + VMCR
 */

#include "boards/virt/vgic.h"
#include "boards/virt/gicv3.h"
#include "arch/aarch64/sysregs.h"
#include "common/debug.h"
#include "common/sched.h"
#include "common/smp.h"
#include "common/utils.h"

static inline uint64_t read_lr(int idx)
{
    switch (idx) {
    case 0: return READ_SYSREG64(ICH_LR0_EL2);
    case 1: return READ_SYSREG64(ICH_LR1_EL2);
    case 2: return READ_SYSREG64(ICH_LR2_EL2);
    default: return 0;
    }
}

static inline void write_lr(int idx, uint64_t val)
{
    switch (idx) {
    case 0: WRITE_SYSREG64(val, ICH_LR0_EL2); break;
    case 1: WRITE_SYSREG64(val, ICH_LR1_EL2); break;
    case 2: WRITE_SYSREG64(val, ICH_LR2_EL2); break;
    }
}

/*
 * vGIC 初始化
 *
 * 前置条件：gic_init() 已完成（物理 CPU 接口已使能系统寄存器模式，
 * 即 ICC_SRE_EL2.SRE=1，这是虚拟接口可用的前提）。
 */
void vgic_init(void)
{
    /*
     * 1. CNTHCTL_EL2 = 0：允许 EL1 直接访问
     *    - CNTPCT_EL0 / CNTVCT_EL0（读计数器）
     *    - CNTV_CTL/CNTV_CVAL/CNTV_TVAL（虚拟定时器控制）
     *    全部不 trap。这是虚拟定时器"直通"的关键。
     */
    WRITE_SYSREG64(0, CNTHCTL_EL2);

    /*
     * 2. 使能虚拟 CPU 接口（ICH_HCR_EL2.En）
     */
    WRITE_SYSREG64(ICH_HCR_EL2_EN, ICH_HCR_EL2);
    asm volatile("isb");

    /*
     * 3. 初始化 ICH_VMCR_EL2
     *    - VPMR = 0xFF：优先级屏蔽放行所有优先级
     *    - VENG1 = 1：使能虚拟 Group 1 中断
     */
    {
        uint64_t vmcr = (0xFFUL << ICH_VMCR_VPMR_SHIFT) | ICH_VMCR_VENG1;
        WRITE_SYSREG64(vmcr, ICH_VMCR_EL2);
        asm volatile("isb");
    }

    /*
     * 4. 清零所有 LR
     */
    for (int i = 0; i < NR_VGIC_LRS; i++)
        write_lr(i, 0);

    /*
     * 5. 在 GIC Redistributor 中使能虚拟定时器的物理中断 PPI #27
     */
    gic_enable_irq(PPI_VIRT_TIMER);

    /*
     * 6. 设置物理 CPU 接口的 EOIMode = 1（split EOI 模式）
     *
     * 这是 vtimer HW 直通注入的关键配套：
     *   - EL2 ack 物理 PPI 27 后写 ICC_EOIR1 只降优先级，不 deactivate
     *   - 真正的 deactivate 由 Guest 的虚拟 EOIR 通过 LR 的 HW 映射联动完成
     */
    {
        uint64_t ctlr = READ_SYSREG64(ICC_CTLR_EL1);
        ctlr |= (1UL << 1); /* EOImode */
        WRITE_SYSREG64(ctlr, ICC_CTLR_EL1);
        asm volatile("isb");
    }

    INFO("vGIC: virtual CPU interface enabled (%d LRs)", NR_VGIC_LRS);
}

/*
 * 向当前 Guest 注入一个虚拟中断（HW 直通映射模式）
 *
 * 用于 vtimer 等有对应物理中断的场景。固定使用 LR0。
 */
void vgic_inject_virq_hw(uint32_t vintid, uint32_t pintid)
{
    uint64_t lr = ((uint64_t)vintid & 0xffffffffULL) /* vINTID [31:0] */
                  | ((uint64_t)pintid << 32)          /* pINTID [41:32] */
                  | ICH_LR_GROUP1                     /* Group 1 */
                  | ICH_LR_HW                         /* 硬件映射模式 */
                  | ICH_LR_PENDING;                   /* state = pending */

    WRITE_SYSREG64(lr, ICH_LR0_EL2);
    asm volatile("isb");
}

/*
 * 注入 HW 映射的虚拟中断（PCI INTx 直通，固定 LR2）
 *
 * 与 vgic_inject_virq_hw 完全一致，只是固定使用 LR2。
 *
 * 关键：绝不设置 bit 41（pINTID 字段的 MSB）。对 HW 映射中断
 * （ICH_LR_HW=1），Guest 写虚拟 EOIR 时硬件会自动 deactivate
 * 链接的物理中断（pINTID），无需也不应手动置 EOI 位——
 * 否则会破坏 pINTID 字段本身。
 */
void vgic_inject_virq_hw_eoi(uint32_t vintid, uint32_t pintid)
{
    uint64_t lr = ((uint64_t)vintid & 0xffffffffULL) /* vINTID [31:0] */
                  | ((uint64_t)pintid << 32)          /* pINTID [41:32] */
                  | ICH_LR_GROUP1                     /* Group 1 */
                  | ICH_LR_HW                         /* 硬件映射模式 */
                  | ICH_LR_PENDING;                   /* state = pending */

    write_lr(2, lr);
    asm volatile("isb");
}

/*
 * 读取 LR0 当前内容（调试用）
 */
uint64_t vgic_get_lr0(void)
{
    return READ_SYSREG64(ICH_LR0_EL2);
}

/*
 * 纯软件注入虚拟中断（不关联物理中断）
 *
 * 用于 virtio、ivshmem 等纯软件设备的中断注入。
 * 从 LR1 开始找空闲条目（LR0 预留给 vtimer HW 映射）。
 *
 * 返回 0 成功，-1 没有空闲 LR
 */
int vgic_inject_virq(uint32_t vintid)
{
    /*
     * 从 LR1 开始查找空闲 LR：
     *   LR0 预留给 vtimer（HW 映射）
     *   LR2 预留给 PCI INTx（HW+EOI 映射）
     * 软件注入只使用 LR1。
     */
    for (int i = 1; i < NR_VGIC_LRS - 1; i++) {
        uint64_t lr = read_lr(i);
        if ((lr & ICH_LR_STATE_MASK) == 0) {
            /* 空闲：写入 pending 状态的虚拟中断 */
            lr = ((uint64_t)vintid & 0xffffffffULL)  /* vINTID */
                 | ICH_LR_GROUP1                     /* Group 1 */
                 | ICH_LR_PENDING;                   /* pending */
            write_lr(i, lr);
            asm volatile("isb");
            return 0;
        }
        /*
         * 同一 vINTID 已 pending：边沿触发中断可合并，无需重复注入。
         * （跨核注入时硬件 LR 可能已被 Guest ACK/EOI 过，这里以硬件 LR
         *  为准做去重，避免重复写入。）
         */
        if ((uint32_t)(lr & 0xffffffffULL) == vintid &&
            (lr & ICH_LR_STATE_MASK) == ICH_LR_PENDING)
            return 0;
    }
    return -1;  /* 没有空闲 LR */
}

/*
 * 查询指定虚拟中断是否 pending
 */
int vgic_is_virq_pending(uint32_t vintid)
{
    for (int i = 0; i < NR_VGIC_LRS; i++) {
        uint64_t lr = read_lr(i);
        if ((lr & ICH_LR_STATE_MASK) &&
            (uint32_t)(lr & 0xffffffffULL) == vintid)
            return 1;
    }
    return 0;
}

/*
 * vGIC 状态保存（世界切换：离开 VM 时调用）
 */
void vgic_save_state(struct vm_virt_state *state)
{
    state->gic_vmcr = READ_SYSREG64(ICH_VMCR_EL2);
    for (int i = 0; i < NR_VGIC_LRS; i++)
        state->gic_lr[i] = read_lr(i);
}

/*
 * vGIC 状态恢复（世界切换：进入 VM 时调用）
 */
void vgic_restore_state(struct vm_virt_state *state)
{
    WRITE_SYSREG64(state->gic_vmcr, ICH_VMCR_EL2);
    for (int i = 0; i < NR_VGIC_LRS; i++)
        write_lr(i, state->gic_lr[i]);
    asm volatile("isb");
}

/*
 * 把纯软件 vIRQ 写入 saved state 的 LR1 槽位。
 *
 * 与 vgic_inject_virq 一致：纯软件虚拟中断只使用 LR1
 * （LR0=vtimer HW 映射，LR2=PCI INTx HW+EOI 映射）。
 * 返回 0 成功，-1 LR1 被其他 vIRQ 占用且不可合并。
 */
static int vgic_inject_saved(struct vm_virt_state *virt, uint32_t vintid)
{
    int i = 1;  /* 纯软件注入固定 LR1 */
    uint64_t lr = virt->gic_lr[i];

    if ((lr & ICH_LR_STATE_MASK) == 0) {
        virt->gic_lr[i] = ((uint64_t)vintid & 0xffffffffULL)
                          | ICH_LR_GROUP1
                          | ICH_LR_PENDING;
        return 0;
    }
    /* 同一 vINTID 已 pending：边沿触发中断可合并 */
    if (((uint32_t)(lr & 0xffffffffULL)) == vintid &&
        (lr & ICH_LR_STATE_MASK) == ICH_LR_PENDING)
        return 0;

    return -1;
}

/*
 * Phase 10: 向指定 VM 注入虚拟中断（不要求目标 VM 正在运行）
 *
 * 三种情况：
 *   1. 目标 VM == current：直接写硬件 LR（和 vgic_inject_virq 一样）
 *   2. 目标 VM 在本核但未运行（被调度出去）：写 saved state
 *   3. 目标 VM 在另一个物理核：置 mailbox 标志 + 发物理 SGI，
 *      由目标核的 vgic_ipi_update_handle() 完成实际注入
 *
 * 跨核为何不能在这里直接写 saved state / 硬件 LR：
 *   - 硬件 LR 是目标核的 per-CPU 资源，本核无法访问
 *   - saved state 会与目标核的世界切换（vgic_save/restore_state）竞态，
 *     且读 cpu_state[target].curr 判断"是否在运行"本身也是竞态
 *
 * 因此跨核统一走 mailbox + IPI，把注入动作交给目标核在中断上下文
 * （IRQ 屏蔽，与世界切换互斥）里原子地完成。
 *
 * 返回 0 成功，-1 没有空闲 LR
 */
int vgic_inject_virq_to_vm(struct task_struct *vm, uint32_t vintid)
{
    if (!vm || vm->state == TASK_ZOMBIE)
        return -1;

    /* 情况 1：目标就是当前运行的 vCPU，直接写本核硬件 LR */
    if (vm == current && vm->pid != 0)
        return vgic_inject_virq(vintid);

    /* 情况 3：跨核——mailbox + 物理 SGI 交给目标核处理 */
    if (vm->phys_cpu != smp_processor_id()) {
        vm->virt.vgic_ipi_vintid = vintid;
        vm->virt.vgic_ipi_pending = 1;
        /* 确保 mailbox 写全局可见后再触发 SGI */
        asm volatile("dmb ish" ::: "memory");
        smp_send_ipi(vm->phys_cpu, SGI_VGIC_UPDATE);
        return 0;
    }

    /* 情况 2：同核但目标未运行——写 saved state，下次调度恢复时生效 */
    return vgic_inject_saved(&vm->virt, vintid);
}

/*
 * Phase 13: 处理跨核 vGIC 维护 IPI
 *
 * 目标核收到 SGI_VGIC_UPDATE 后，在本核任务表中找到带 pending
 * 标志的目标 vCPU，完成实际注入：
 *   - 目标 vCPU 正在运行（== current）：写硬件 LR，立即送达
 *   - 目标 vCPU 未运行（被调度出去）：写 saved state，下次恢复时生效
 *
 * 由 smp_ipi_handle() 在目标核的 IRQ 上下文调用（IRQ 屏蔽），
 * 因此与本核的世界切换互斥，`current` 是稳定的，注入动作原子。
 */
void vgic_ipi_update_handle(void)
{
    int cpu = smp_processor_id();

    /*
     * 不能只用 `current` 找目标：收到 IPI 时目标 vCPU 可能已被
     * 调度出去（此时 current 是 idle 或另一个 vCPU），而 pending
     * 标志挂在目标 vCPU 自己的 virt 结构上。所以遍历本核任务表。
     */
    for (int i = 0; i < cpu_state[cpu].nr_tasks; i++) {
        struct task_struct *vm = cpu_state[cpu].task[i];
        if (!vm || vm->pid == 0)
            continue;
        if (!vm->virt.vgic_ipi_pending)
            continue;

        uint32_t vintid = vm->virt.vgic_ipi_vintid;
        vm->virt.vgic_ipi_pending = 0;

        if (cpu_state[cpu].curr == vm) {
            /* 目标正在运行：写硬件 LR，立即送达 */
            vgic_inject_virq(vintid);
        } else {
            /* 目标未运行：写 saved state，下次调度恢复时生效 */
            vgic_inject_saved(&vm->virt, vintid);
        }
    }
}

/*
 * Phase 10: 中断性能统计
 *
 * 测量从中断触发（进入 EL2）到注入完成的 cycles 数，
 * 用于评估中断虚拟化开销。
 */
struct irq_stats {
    uint64_t lpi_count;       /* LPI 中断总数 */
    uint64_t lpi_total_cycles; /* LPI 总处理 cycles */
    uint64_t lpi_max_cycles;   /* LPI 最大单次 cycles */
    uint64_t lpi_min_cycles;   /* LPI 最小单次 cycles */
};

static struct irq_stats irq_stat = {
    .lpi_min_cycles = (uint64_t)-1,
};

void irq_stats_record_lpi(uint64_t cycles)
{
    irq_stat.lpi_count++;
    irq_stat.lpi_total_cycles += cycles;
    if (cycles > irq_stat.lpi_max_cycles)
        irq_stat.lpi_max_cycles = cycles;
    if (cycles < irq_stat.lpi_min_cycles)
        irq_stat.lpi_min_cycles = cycles;
}

void irq_stats_dump(void)
{
    if (irq_stat.lpi_count == 0) {
        printf("[irq-stats] no LPI interrupts yet\n");
        return;
    }
    uint64_t avg = irq_stat.lpi_total_cycles / irq_stat.lpi_count;
    printf("[irq-stats] LPI: count=%lu avg=%lu min=%lu max=%lu cycles\n",
           (unsigned long)irq_stat.lpi_count,
           (unsigned long)avg,
           (unsigned long)irq_stat.lpi_min_cycles,
           (unsigned long)irq_stat.lpi_max_cycles);
}
