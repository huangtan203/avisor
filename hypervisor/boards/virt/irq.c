// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - QEMU virt 平台中断处理
 *
 * 学习要点：
 *   在 GICv3 架构下，中断处理流程是：
 *   1. CPU 收到中断信号，跳转到异常向量表的 IRQ 入口
 *   2. 保存上下文
 *   3. 读 ICC_IAR1_EL1 获取中断号（应答中断）
 *   4. 根据中断号分发到对应的处理函数
 *   5. 写 ICC_EOIR1_EL1 结束中断
 *   6. 恢复上下文，返回
 *
 *   这里 handle_irq() 是中断分发器（dispatcher），
 *   对应树莓派版本里读 IRQ_PENDING_1 寄存器的角色。
 */

#include "boards/virt/gicv3.h"
#include "boards/virt/pl011_uart.h"
#include "boards/virt/arch_timer.h"
#include "boards/virt/vgic.h"
#include "boards/virt/pci.h"
#include "boards/virt/its.h"
#include "boards/virt/vits.h"
#include "arch/aarch64/sysregs.h"
#include "common/debug.h"
#include "common/entry.h"
#include "common/sched.h"
#include "common/smp.h"
#include "common/timer.h"
#include "common/utils.h"

/* Phase 10: 读取物理计数器（用于中断延迟测量）
 * CNTPCT_EL0 是一个自由运行的 64 位计数器，频率 = timer_freq
 * 注意：EL2 可以直接读 CNTPCT_EL0
 */
static inline uint64_t read_cntpct(void)
{
    uint64_t val;
    asm volatile("mrs %0, cntpct_el0" : "=r"(val));
    return val;
}

/* 异常入口错误消息表（和 raspi 版本一致） */
const char *entry_error_messages[] = {
    "SYNC_INVALID_EL2",
    "IRQ_INVALID_EL2",
    "FIQ_INVALID_EL2",
    "ERROR_INVALID_EL2",

    "SYNC_INVALID_EL01_64",
    "IRQ_INVALID_EL01_64",
    "FIQ_INVALID_EL01_64",
    "ERROR_INVALID_EL01_64",

    "SYNC_INVALID_EL01_32",
    "IRQ_INVALID_EL01_32",
    "FIQ_INVALID_EL01_32",
    "ERROR_INVALID_EL01_32",
};

/*
 * 使能中断控制器
 * 对于 GICv3，初始化已经在 gic_init() 里做了，
 * 这里保留接口兼容。
 */
void enable_interrupt_controller()
{
    /* GICv3 已经在 gic_init() 中初始化并使能了 */
}

/*
 * 显示未捕获异常信息
 */
void show_invalid_entry_message(int type, unsigned long esr, unsigned long elr,
                unsigned long far)
{
    PANIC("uncaught exception(%s) esr: %x, elr: %x, far: %x",
          entry_error_messages[type], esr, elr, far);
}

/*
 * 中断处理主函数
 *
 * 学习要点：
 *   GICv3 的中断处理流程：
 *   1. ack (IAR) - 获取中断号，同时标记为 active
 *   2. handle - 调用对应处理函数
 *   3. eoi (EOIR) - 结束中断，清除 active 状态
 *
 *   和 BCM2837 的区别：
 *   - BCM2837：读多个 pending 寄存器，判断是哪个中断
 *   - GICv3：直接读 IAR 寄存器拿到中断号，简洁高效
 */
void handle_irq(void)
{
    unsigned int irq;

    /* 1. 应答中断，获取中断号 */
    irq = gic_ack_irq();

    /* 虚假中断：没有真正的中断，直接返回 */
    if (irq == GIC_SPURIOUS_IRQ) {
        return;
    }

    /*
     * LPI (Locality-specific Peripheral Interrupt) 处理（Phase 8）
     *
     * LPI 是 MSI/MSI-X 经 ITS 翻译后产生的中断，特点：
     *   - INTID >= 8192（GICv3 规范，QEMU 同样强制）
     *   - 无 active 状态：读 IAR1 时硬件即清除 pending 位
     *   - 边沿触发：MSI 写一次触发一次，EOI 后不会重触发
     *
     * 处理流程：
     *   1. IAR1 读取（gic_ack_irq）已顺带清除了 pending 位
     *   2. 若是直通设备的 MSI 且当前在目标 VM：软件注入虚拟中断
     *      （LPI 不能用 LR 的 HW 映射模式，纯软件注入）
     *   3. 写 EOIR 结束（LPI 无 active 状态，不需要 DIR）
     */
    if (irq >= 8192) {
        /*
         * Phase 10: 测量 LPI 处理延迟（从 ack 到注入完成的 ticks）
         */
        uint64_t t_start = read_cntpct();

        /*
         * Phase 9: 先查 vITS 的 pLPI → (vmid, vINTID) 映射表。
         * 若找到匹配：向目标 VM 注入对应的虚拟中断。
         * 若没找到：回退到 Phase 8 的硬编码方式（edu 的 8192..8207 → vIRQ 50）。
         */
        unsigned int tgt_vmid = 0, tgt_vintid = 0;
        if (vits_lpi_to_virq(irq, &tgt_vmid, &tgt_vintid) == 0) {
            /*
             * Phase 10: 用 vgic_inject_virq_to_vm 支持向任意 VM 注入
             * （即使目标 VM 不在当前运行也能注入到 saved state）
             * 遍历所有 CPU 的任务表找到目标 VM。
             */
            struct task_struct *target_vm = NULL;
            for (int c = 0; c < MAX_CPUS; c++) {
                for (int i = 0; i < cpu_state[c].nr_tasks; i++) {
                    struct task_struct *t = cpu_state[c].task[i];
                    if (t && t->pid != 0 && t->vmid == tgt_vmid &&
                        t->state != TASK_ZOMBIE) {
                        target_vm = t;
                        break;
                    }
                }
                if (target_vm) break;
            }

            if (target_vm) {
                uint32_t vspi = (tgt_vintid >= 8192)
                                    ? (64 + (tgt_vintid - 8192))
                                    : tgt_vintid;
                vgic_inject_virq_to_vm(target_vm, vspi);
            }

            uint64_t t_end = read_cntpct();
            irq_stats_record_lpi(t_end - t_start);

            gic_eoi_irq(irq);
            return;
        }

        /* Phase 8 兼容：硬编码 edu 的 8192..8207 → vIRQ 50 */
        if (pci_edu_msi_enabled &&
            irq >= pci_edu_lpi_base && irq < pci_edu_lpi_base + 16) {
            if (current && current->pid != 0 && current->vmid == 1)
                vgic_inject_virq(VIRQ_PCI_EDU);
        } else {
            WARN("LPI %u dropped (no mapping)", irq);
        }
        gic_eoi_irq(irq);
        return;
    }

    /*
     * PCI INTx 直通转发（edu 设备，Phase 7）。
     *
     * 物理中断号在枚举时由 pin+slot swizzle 算出，是运行时变量
     * （pci_edu_intx_irq），因此不能用 switch 的常量 case 标签。
     *
     * 仅注入给直通该设备的 VM1；idle 或 VM2 等其他上下文直接去活
     * 物理中断——电平触发的 INTx 线仍为高，GIC 会立刻重新 pending，
     * 等 VM1 再次被调度时即可送达（避免电平中断卡在 active 状态）。
     *
     * split EOI + HW 联动：写 EOIR 只降优先级，物理中断保持 active，
     * 由 Guest 写虚拟 EOIR 时经 LR2 的 HW 映射自动去活。
     */
    if (pci_edu_intx_irq != 0 && irq == pci_edu_intx_irq) {
        if (current && current->pid != 0 && current->vmid == 1) {
            vgic_inject_virq_hw_eoi(VIRQ_PCI_EDU, pci_edu_intx_irq);
            gic_eoi_irq(irq);
            return;  // 跳过末尾公共处理，由 HW 联动去活物理 INTx
        }
        /* 非目标 VM 上下文：显式去活，否则电平中断无法再次触发 */
        WRITE_SYSREG64(pci_edu_intx_irq, ICC_DIR_EL1);
        asm volatile("isb");
        gic_eoi_irq(irq);
        return;
    }

    /* 2. 根据中断号分发处理 */
    switch (irq) {
    case PPI_EL2_PHYS_TIMER:
        /* EL2 物理定时器中断（Hypervisor tick） */
        handle_timer_irq();
        /* split EOI 模式下需要显式 DIR deactivate */
        WRITE_SYSREG64(PPI_EL2_PHYS_TIMER, ICC_DIR_EL1);
        asm volatile("isb");
        break;

    case PPI_VIRT_TIMER:
        /*
         * 虚拟定时器中断（Guest 的 CNTV 到期）
         *
         * GICv3 vtimer 标准虚拟化流程（HW 直通模式）：
         *   1. EL2 ack 物理 27 → active
         *   2. 注入 HW 模式 LR（vINTID=27, pINTID=27）
         *   3. 写 EOIR 降优先级（split EOI 模式，保持物理 27 active）
         *   4. eret 回 EL1 → Guest 收到 vIRQ 27
         *   5. Guest 处理 → 重装 CVAL（清电平）→ 虚拟 EOI
         *   6. HW 联动：Guest 写虚拟 EOI 自动 deactivate 物理 27
         */
        if (current && current->pid != 0) {
            vgic_inject_virq_hw(PPI_VIRT_TIMER, PPI_VIRT_TIMER);
        }
        /*
         * split EOI：只写 EOIR（降优先级），不写 DIR。
         * 物理 27 保持 active，由 Guest 虚拟 EOI 联动硬件 deactivate。
         */
        gic_eoi_irq(irq);
        return;  // 跳过末尾的公共处理

    case 33 + 32:  /* VIRT_UART0_IRQ = SPI #33, 加32得全局中断号 */
        /* PL011 UART 接收中断 */
        handle_uart_irq();
        break;

    default:
        /*
         * SGI（0-15）核间软中断：分发到 SMP IPI 处理。
         * SGI 是核间通信的基础，处理完走末尾统一 EOI。
         */
        if (irq < 16) {
            smp_ipi_handle(irq);
            /*
             * SGI 是边沿触发中断：split EOI 模式下必须显式 DIR
             * 解除激活，否则该 SGI 号一直保持 active，无法再次触发。
             * （与 PPI #26 定时器相同的处理方式。）
             */
            WRITE_SYSREG64(irq, ICC_DIR_EL1);
            asm volatile("isb");
            break;
        }
        WARN("Unknown IRQ: %u", irq);
        break;
    }

    /* 3. 结束中断 */
    gic_eoi_irq(irq);
}
