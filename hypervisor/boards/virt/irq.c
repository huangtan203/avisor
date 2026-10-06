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
#include "arch/aarch64/sysregs.h"
#include "common/debug.h"
#include "common/entry.h"
#include "common/sched.h"
#include "common/timer.h"
#include "common/utils.h"

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

    /* 2. 根据中断号分发处理 */
    switch (irq) {
    case PPI_EL2_PHYS_TIMER:
        /* EL2 物理定时器中断（Hypervisor tick） */
        handle_timer_irq();
        break;

    case 33 + 32:  /* VIRT_UART0_IRQ = SPI #33, 加32得全局中断号 */
        /* PL011 UART 接收中断 */
        handle_uart_irq();
        break;

    default:
        WARN("Unknown IRQ: %u", irq);
        break;
    }

    /* 3. 结束中断 */
    gic_eoi_irq(irq);
}
