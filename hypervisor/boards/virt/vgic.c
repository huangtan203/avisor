// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - GICv3 虚拟化支持 (vGIC)
 *
 * 学习要点：
 *   GICv3 对虚拟化的支持是"硬件级"的，这是它和 GICv2/BCM2837 的
 *   本质区别。核心机制有三块：
 *
 *   1. 虚拟 CPU 接口（Virtual CPU Interface）
 *      HCR_EL2.IMO=1 后，Guest 在 EL1 执行 mrs/msr ICC_*_EL1 时，
 *      硬件自动把访问重定向到"虚拟视图"：
 *        - Guest 读 ICC_IAR1_EL1 → 读到的是 List Register 里的虚拟中断
 *        - Guest 写 ICC_EOIR1_EL1 → 硬件自动让对应 LR 条目失效
 *        - Guest 写 ICC_PMR/IGRPEN1 → 写入 ICH_VMCR_EL2 的后端字段
 *      所以 Guest 里跑的是**原生的 GICv3 驱动**，一行都不用改！
 *
 *   2. List Register（ICH_LR<n>_EL2）
 *      EL2 想给 Guest 注入虚拟中断（比如把物理 vtimer 中断翻译成
 *      虚拟中断），只需写一个 LR 条目：
 *        LR = vINTID | GROUP1 | PENDING | VALID
 *      Guest 侧立刻看到 IRQ pending（虚拟信号线）。
 *
 *   3. 虚拟定时器的"翻译"
 *      Guest 用 CNTV（虚拟定时器，寄存器直通不 trap），到期时
 *      物理中断 PPI #27 路由到 EL2（因为 IMO=1）。
 *      Hypervisor 把它翻译成虚拟中断 27 注入给 Guest——
 *      对 Guest 来说，就像真的有一个定时器中断来了。
 *
 *   本文件实现的简化（Phase 2）：
 *     - 只用 LR0 注入（单 VM、单一虚拟中断源）
 *     - 不用 HW 直通位：Guest EOI 后 LR 自动失效，
 *       不需要处理 maintenance interrupt
 *     - 不保存/恢复 VMCR（单 VM；多 VM 时应按任务保存）
 */

#include "boards/virt/vgic.h"
#include "boards/virt/gicv3.h"
#include "arch/aarch64/sysregs.h"
#include "common/debug.h"
#include "common/utils.h"

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
     *    使能后：
     *    - EL1 的 ICC_* 系统寄存器访问重定向到虚拟接口
     *    - LR 中 pending 的虚拟中断会向 EL1 发出 vIRQ 信号
     */
    WRITE_SYSREG64(ICH_HCR_EL2_EN, ICH_HCR_EL2);
    asm volatile("isb");

    /*
     * 2.5 初始化 ICH_VMCR_EL2（虚拟 CPU 接口的控制状态）
     *    - VPMR = 0xFF：优先级屏蔽放行所有优先级
     *    - VENG1 = 1：使能虚拟 Group 1 中断
     *    Guest 侧的 gic_vcpu_init 也会写 ICC_PMR/ICC_IGRPEN1
     *    （自动重定向到这里），HV 预设一份保证初始状态正确。
     */
    {
        uint64_t vmcr = (0xFFUL << ICH_VMCR_VPMR_SHIFT) | ICH_VMCR_VENG1;
        WRITE_SYSREG64(vmcr, ICH_VMCR_EL2);
        asm volatile("isb");
    }

    /*
     * 3. 在 GIC Redistributor 中使能虚拟定时器的物理中断 PPI #27。
     *    CNTV 到期时产生 PPI 27，路由到 EL2（HCR_EL2.IMO=1），
     *    由 handle_irq() 翻译并注入给 Guest。
     */
    gic_enable_irq(PPI_VIRT_TIMER);

    /*
     * 4. 设置物理 CPU 接口的 EOIMode = 1（split EOI 模式）。
     *
     *    这是 vtimer HW 直通注入的关键配套：
     *    - EL2 ack 物理 PPI 27 后写 ICC_EOIR1 只【降优先级】
     *      （清 APR，解除对其他中断的阻塞），不 deactivate——
     *      物理 27 保持 active，电平信号被屏蔽，不会风暴式 re-pend
     *    - 真正的 deactivate 由 Guest 的虚拟 EOIR 通过
     *      LR 的 HW 映射联动完成（此时 Guest 已重装 CVAL，
     *      信号撤销，27 不会再 pending）
     *    若不开 split 模式，EL2 的 EOIR 会立即 deactivate 27，
     *      而 vtimer 信号（ISTATUS=1）还在，EL2 将陷入中断风暴。
     */
    {
        uint64_t ctlr = READ_SYSREG64(ICC_CTLR_EL1);
        ctlr |= (1UL << 1); /* EOImode */
        WRITE_SYSREG64(ctlr, ICC_CTLR_EL1);
        asm volatile("isb");
    }

    INFO("vGIC: virtual CPU interface enabled (ICH_HCR_EL2.En=1)");
}

/*
 * 向当前 Guest 注入一个虚拟中断（HW 直通映射模式）
 *
 * 参数：
 *   vintid - 虚拟中断号（Guest 视角）
 *   pintid - 对应的物理中断号
 *
 * 学习要点（GICv3 的精髓，KVM 对 vtimer 就是这么做的）：
 *   LR 的 HW=1 + pINTID 建立虚拟/物理中断的硬件联动：
 *     - EL2 ack 物理 PPI 后【不做物理 EOI】，保持 active 状态
 *       （active 期间电平信号被 GIC 屏蔽，不会风暴式 re-pend）
 *     - 注入本 LR 条目，Guest 的虚拟接口立刻看到 vINTID pending
 *     - Guest 读 ICC_IAR1 应答 → LR 变 active
 *     - Guest 写 ICC_EOIR1 结束 → 【硬件自动 deactivate 物理 PPI】
 *       （EOI 位使能了硬件联动，无需 EL2 介入，无 maintenance 中断）
 *   整个过程中 EL2 只做一次"ack + 注入"，剩下全由硬件完成。
 *
 * 注意：调用本函数后【不要】再对 pintid 执行物理 gic_eoi_irq()！
 */
void vgic_inject_virq_hw(uint32_t vintid, uint32_t pintid)
{
    /*
     * LR 布局（与 QEMU/KVM 编码一致）：
     *   bits[31:0]  vINTID
     *   bits[41:32] pINTID（10 位，HW=1 时生效）
     *   bits[55:48] priority（默认 0 = 最高）
     *   bit 60      Group（1 = Group 1）
     *   bit 61      HW（虚拟/物理硬件联动）
     *   bits[63:62] state（1 = pending）
     *
     * 注意：bit41（EOI）与 pINTID 最高位重叠，设它会把 pINTID
     * 污染成 |512，导致 guest EOIR 时 deactivate 错误的物理中断
     * （QEMU 的硬件联动 deactivate 不需要 EOI 位，与 KVM 做法一致）。
     */
    uint64_t lr = ((uint64_t)vintid & 0xffffffffULL) /* vINTID [31:0] */
                  | ((uint64_t)pintid << 32)          /* pINTID [41:32] */
                  | ICH_LR_GROUP1                     /* Group 1 */
                  | ICH_LR_HW                         /* 硬件映射模式 */
                  | ICH_LR_PENDING;                   /* state = pending */

    WRITE_SYSREG64(lr, ICH_LR0_EL2);
    asm volatile("isb");
}

/*
 * 读取 LR0 当前内容（调试用：判断虚拟中断是否还在排队）
 */
uint64_t vgic_get_lr0(void)
{
    return READ_SYSREG64(ICH_LR0_EL2);
}
