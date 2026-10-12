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
#include "common/utils.h"

static inline uint64_t read_lr(int idx)
{
    switch (idx) {
    case 0: return READ_SYSREG64(ICH_LR0_EL2);
    case 1: return READ_SYSREG64(ICH_LR1_EL2);
    default: return 0;
    }
}

static inline void write_lr(int idx, uint64_t val)
{
    switch (idx) {
    case 0: WRITE_SYSREG64(val, ICH_LR0_EL2); break;
    case 1: WRITE_SYSREG64(val, ICH_LR1_EL2); break;
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
    /* 从 LR1 开始查找空闲 LR（LR0 预留给 vtimer） */
    for (int i = 1; i < NR_VGIC_LRS; i++) {
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
