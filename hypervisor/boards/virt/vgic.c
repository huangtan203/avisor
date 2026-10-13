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
#include "boards/virt/virt_board.h"
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
     *    Phase 17 额外置 TC：使 Guest 写 ICC_SGI1R_EL1 时 trap 到 EL2，
     *    由 handle_trap_system() 翻译成 vSGI 注入（trap-and-emulate）。
     *    TC 只影响 EL1 的 SGI 寄存器访问，EL2 自身发物理 SGI 不受影响。
     */
    WRITE_SYSREG64(ICH_HCR_EL2_EN | ICH_HCR_EL2_TC, ICH_HCR_EL2);
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
 * Phase 15: 虚拟 GICD 初始化
 *
 * 默认所有 SPI 使能（spi_enable 全 1），保持向后兼容：现有设备 SPI
 * （virtio 48 / ivshmem 49 / edu 50 / shm 60）无需 Guest 显式使能即可工作。
 * Guest 之后可通过 ICENABLER/ISENABLER 自行开关。
 */
void vgicd_init(struct vgic_dist *d)
{
    if (!d)
        return;

    /* Group1 NS 使能 + 亲和路由使能 */
    d->ctlr = GICD_CTLR_ENABLE_G1NS | GICD_CTLR_ARE_NS;
    for (int i = 0; i < 32; i++)
        d->spi_enable[i] = 0xFFFFFFFFu;
}

/*
 * Phase 15: 虚拟 GICD 寄存器读（offset 相对 0x08000000）
 */
uint32_t vgicd_read(struct vgic_dist *d, uint32_t offset)
{
    if (!d)
        return 0;

    if (offset == 0x0000)
        return d->ctlr;                 /* GICD_CTLR */
    if (offset == 0x0004)
        return 0x1F;                    /* GICD_TYPER: ITLinesNumber=31 */
    if (offset == 0x0008)
        return 0x43B;                   /* GICD_IIDR */

    /* IGROUPR(n): 0x0080 + n*4，全部返回 Group 1 */
    if (offset >= 0x0080 && offset < 0x0100 && ((offset - 0x0080) & 3) == 0)
        return 0xFFFFFFFFu;

    /* ISENABLER(n): 0x0100 + n*4 */
    if (offset >= 0x0100 && offset < 0x0180 && ((offset - 0x0100) & 3) == 0)
        return d->spi_enable[(offset - 0x0100) >> 2];

    /* IPRIORITYR(n): 默认优先级 0xA0 */
    if (offset >= 0x0400 && offset < 0x0800 && ((offset - 0x0400) & 3) == 0)
        return 0xA0A0A0A0u;

    /* 其余（ICENABLER/ISPENDR/ICPENDR/ICFGR/IROUTER 等）读 0 */
    return 0;
}

/*
 * Phase 15: 虚拟 GICD 寄存器写（offset 相对 0x08000000）
 */
void vgicd_write(struct vgic_dist *d, uint32_t offset, uint32_t val)
{
    if (!d)
        return;

    if (offset == 0x0000) {
        /* GICD_CTLR：只保留使能 + 亲和路由位 */
        d->ctlr = val & (GICD_CTLR_ENABLE_G0 | GICD_CTLR_ENABLE_G1NS |
                         GICD_CTLR_ENABLE_G1S | GICD_CTLR_ARE_NS |
                         GICD_CTLR_ARE_S);
        return;
    }

    /* ISENABLER(n): 0x0100 + n*4，置位使能 */
    if (offset >= 0x0100 && offset < 0x0180 && ((offset - 0x0100) & 3) == 0) {
        d->spi_enable[(offset - 0x0100) >> 2] |= val;
        return;
    }

    /* ICENABLER(n): 0x0180 + n*4，清除使能 */
    if (offset >= 0x0180 && offset < 0x0200 && ((offset - 0x0180) & 3) == 0) {
        d->spi_enable[(offset - 0x0180) >> 2] &= ~val;
        return;
    }

    /* 其余（IGROUPR/IPRIORITYR/ICFGR/ISPENDR/IROUTER 等）忽略写 */
}

/*
 * Phase 16: 虚拟 GICR 初始化
 *
 * 默认所有 PPI 使能（ppi_enable 全 1），保持向后兼容：软件 PPI
 * 默认放行。真实 vtimer PPI 27 走 HW 直通（vgic_inject_virq_hw），
 * 不受此软件位图影响。
 */
void vgicr_init(struct vgic_rdist *r)
{
    if (!r)
        return;

    r->ctlr = 0;                /* GICR_CTLR 复位为 0（无 LPI 使能位） */
    r->ppi_enable = 0xFFFFFFFFu;
}

/*
 * Phase 16: 虚拟 GICR 寄存器读（offset 相对 0x080A0000）
 *
 * GICR 由两个 64KB 帧组成：
 *   - RD_base 帧（0x0000-0xFFFF）：GICR_CTLR/IIDR/TYPER/WAKER 等
 *   - SGI_base 帧（0x10000-0x1FFFF）：SGI/PPI 的组/使能/优先级
 */
uint32_t vgicr_read(struct vgic_rdist *r, uint32_t offset)
{
    if (!r)
        return 0;

    if (offset == GICR_CTLR)            /* 0x0000 */
        return r->ctlr;
    if (offset == GICR_IIDR)            /* 0x0004 */
        return 0x43B;
    if (offset == GICR_TYPER)           /* 0x0008：Affinity_Value=0，简化 */
        return 0;
    if (offset == GICR_WAKER)           /* 0x0014：未睡眠 */
        return 0;

    /* SGI/PPI 帧（0x10000 起），sgi 为相对 SGI_base 的偏移 */
    if (offset >= GICR_SGI_BASE) {
        uint32_t sgi = offset - GICR_SGI_BASE;

        if (sgi == GICR_IGROUPR0)       /* 0x0080：SGI+PPI 全 Group 1 */
            return 0xFFFFFFFFu;
        if (sgi == GICR_ISENABLER0)     /* 0x0100 */
            return r->ppi_enable;
        /* IPRIORITYR(n)：0x0400 + n*4，默认优先级 0xA0 */
        if (sgi >= 0x0400 && sgi < 0x0420 && ((sgi - 0x0400) & 3) == 0)
            return 0xA0A0A0A0u;
    }

    /* 其余（ICENABLER0/ISPENDR0/ICPENDR0/ICFGR/PROPBASER 等）读 0 */
    return 0;
}

/*
 * Phase 16: 虚拟 GICR 寄存器写（offset 相对 0x080A0000）
 */
void vgicr_write(struct vgic_rdist *r, uint32_t offset, uint32_t val)
{
    if (!r)
        return;

    if (offset == GICR_CTLR) {          /* 0x0000 */
        r->ctlr = val;
        return;
    }

    if (offset == GICR_WAKER)           /* 0x0014：Guest 唤醒，忽略 */
        return;

    if (offset >= GICR_SGI_BASE) {
        uint32_t sgi = offset - GICR_SGI_BASE;

        if (sgi == GICR_ISENABLER0) {   /* 0x0100：置位使能 */
            r->ppi_enable |= val;
            return;
        }
        if (sgi == GICR_ICENABLER0) {   /* 0x0180：清除使能 */
            r->ppi_enable &= ~val;
            return;
        }
    }

    /* 其余（IGROUPR0/IPRIORITYR/ICFGR/PROPBASER 等）忽略写 */
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
/*
 * 纯软件注入（不经过 GICD 门控）——内部实现。
 *
 * 从 LR1 开始查找空闲 LR：
 *   LR0 预留给 vtimer（HW 映射）
 *   LR2 预留给 PCI INTx（HW+EOI 映射）
 * 软件注入只使用 LR1。
 *
 * 与 GICD 门控解耦：vITS 的 LPI 注入、vSGI 注入都走这里，
 * 它们由 ITS / SGI 各自的使能语义管理，不受 GICD SPI enable 门控。
 */
static int vgic_inject_virq_raw(uint32_t vintid)
{
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
 * 纯软件注入虚拟中断（设备 SPI 入口，带 GICD enable 门控）
 *
 * 用于 virtio、ivshmem、edu MSI 等纯软件设备向当前 VM 注入 SPI。
 * 若目标虚拟 SPI 在 GICD 中被 Guest 禁用（ICENABLER），则丢弃并返回 -1，
 * 模拟真实 GICD 对禁用 SPI 的屏蔽行为。
 *
 * 返回 0 成功，-1 被门控丢弃或没有空闲 LR
 */
int vgic_inject_virq(uint32_t vintid)
{
    /* PPI (16-31) 受 GICR enable 门控（软件 PPI） */
    if (vintid >= 16 && vintid < 32 &&
        !virt_vgicr_ppi_enabled(current, vintid))
        return -1;

    /* SPI (32-1019) 受 GICD enable 门控；SGI/LPI 不在此列 */
    if (vintid >= 32 && vintid < 1020 &&
        !virt_vgicd_spi_enabled(current, vintid))
        return -1;

    return vgic_inject_virq_raw(vintid);
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

    /* 情况 1：目标就是当前运行的 vCPU，直接写本核硬件 LR
     * （走 raw：LPI/vSGI 经此路径，不受 GICD SPI enable 门控） */
    if (vm == current && vm->pid != 0)
        return vgic_inject_virq_raw(vintid);

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
            /* 目标正在运行：写硬件 LR，立即送达
             * （走 raw：LPI/vSGI 经此路径，不受 GICD SPI enable 门控） */
            vgic_inject_virq_raw(vintid);
        } else {
            /* 目标未运行：写 saved state，下次调度恢复时生效 */
            vgic_inject_saved(&vm->virt, vintid);
        }
    }
}

/*
 * Phase 17: 处理 Guest 写 ICC_SGI1R_EL1 的 trap（虚拟 SGI trap-and-emulate）
 *
 * 背景：GICv3 里 Guest 用 ICC_SGI1R_EL1 发 SGI。若 ICH_HCR_EL2.TC=1，
 * 该写会 trap 到 EL2（EC=0x18），由 handle_trap_system() 解析 ESR 后
 * 调用本函数。本函数把标准的 ICC_SGI1R_EL1 写翻译成 vSGI 注入，
 * 从而让 Guest 的 GICv3 SGI 发送代码无需 HVC 半虚拟化即可工作。
 *
 * ICC_SGI1R_EL1 编码（GICv3 亲和路由 ARE_NS=1，与 gic_send_sgi 一致）：
 *   [55:48]  Aff3
 *   [40]     IRM   1=广播给除自身外的所有 PE
 *   [39:32]  Aff2
 *   [27:24]  INTID SGI 号 (0-15)
 *   [23:16]  Aff1
 *   [15:0]   TargetList  bit n 对应 aff0=n 的 PE
 *
 * 本项目单 cluster（所有 vCPU 的 Aff1/Aff2/Aff3 均为 0），仅 Aff0 区分
 * vCPU，因此：
 *   - 若 Aff1/Aff2/Aff3 非 0，目标不在本 VM，直接忽略；
 *   - 否则把 TargetList 位图翻译成 vcpu_id（=Aff0），逐位注入。
 */
void handle_vgic_sgi1r(uint64_t val)
{
    uint32_t sgi_id = (uint32_t)((val >> 24) & 0xF);   /* INTID [27:24] */
    int irm        = (int)((val >> 40) & 1);           /* IRM [40] */
    uint32_t aff1  = (uint32_t)((val >> 16) & 0xFF);   /* Aff1 [23:16] */
    uint32_t aff2  = (uint32_t)((val >> 32) & 0xFF);   /* Aff2 [39:32] */
    uint32_t aff3  = (uint32_t)((val >> 48) & 0xFF);   /* Aff3 [55:48] */
    uint32_t target_list = (uint32_t)(val & 0xFFFF);   /* TargetList [15:0] */

    int vmid = (int)current->vmid;
    int self_vcpu = (int)current->vcpu_id;

    /* 单 cluster 简化：目标亲和必须是 0 才算本 VM 的 vCPU */
    if (aff1 || aff2 || aff3)
        return;

    if (irm) {
        /* 广播：给本 VM 所有 vCPU，排除自身 */
        for (int v = 0; v < MAX_VCPUS_PER_VM; v++) {
            if (v == self_vcpu)
                continue;
            virt_inject_virq_to_vcpu(vmid, v, sgi_id);
        }
        return;
    }

    /* 定向：遍历 TargetList 位图，bit n -> vcpu_id = n (Aff0) */
    for (int v = 0; v < 16; v++) {
        if (target_list & (1u << v))
            virt_inject_virq_to_vcpu(vmid, v, sgi_id);
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
