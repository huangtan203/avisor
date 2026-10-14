// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Guest - EL1 异常向量表
 *
 * 学习要点：
 *   ARMv8-A 的向量表和 EL2 的一样是 16 个槽位（4 类 x 4 来源），
 *   只是这里挂在 VBAR_EL1 上。
 *   裸机 guest 只需要处理 IRQ（虚拟定时器注入），
 *   其他异常都进入错误报告（打印 + 停机）。
 *
 *   IRQ 处理流程：
 *     1. 保存通用寄存器（简单的全量压栈）
 *     2. 调用 C 处理函数 guest_irq_handler()
 *     3. 恢复寄存器，eret 返回被中断的代码
 */

.macro VENTRY label
    .align 7
    b \label
.endm

/* VBAR_EL1 要求向量表 2KB 对齐（低 12 位写入时被硬件忽略） */
.balign 2048

.section .text

.globl vectors
vectors:
    /* 当前 EL，使用 SP0（不会发生，防御） */
    VENTRY exc_sync_sp0
    VENTRY exc_irq_sp0
    VENTRY exc_fiq_sp0
    VENTRY exc_error_sp0

    /* 当前 EL，使用 SPx —— guest 主要运行在这个模式（EL1h） */
    VENTRY exc_sync_spx
    VENTRY exc_irq_spx          /* ← 虚拟定时器中断走这里 */
    VENTRY exc_fiq_spx
    VENTRY exc_error_spx

    /* 来自低 EL，AArch64（EL0，Phase 2 未用） */
    VENTRY exc_sync_a64
    VENTRY exc_irq_a64
    VENTRY exc_fiq_a64
    VENTRY exc_error_a64

    /* 来自低 EL，AArch32（不会发生） */
    VENTRY exc_sync_a32
    VENTRY exc_irq_a32
    VENTRY exc_fiq_a32
    VENTRY exc_error_a32

/* ---------- IRQ：保存现场 → C 处理 → 恢复 ---------- */
exc_irq_sp0:
exc_irq_spx:
exc_irq_a64:
exc_irq_a32:
    /* 压栈 x0-x30（31 个寄存器 x 8 字节 = 248 字节，对齐到 256） */
    sub sp, sp, #256
    stp x0, x1, [sp, #16 * 0]
    stp x2, x3, [sp, #16 * 1]
    stp x4, x5, [sp, #16 * 2]
    stp x6, x7, [sp, #16 * 3]
    stp x8, x9, [sp, #16 * 4]
    stp x10, x11, [sp, #16 * 5]
    stp x12, x13, [sp, #16 * 6]
    stp x14, x15, [sp, #16 * 7]
    stp x16, x17, [sp, #16 * 8]
    stp x18, x19, [sp, #16 * 9]
    stp x20, x21, [sp, #16 * 10]
    stp x22, x23, [sp, #16 * 11]
    stp x24, x25, [sp, #16 * 12]
    stp x26, x27, [sp, #16 * 13]
    stp x28, x29, [sp, #16 * 14]
    str x30, [sp, #16 * 15]

    bl guest_irq_handler

    ldp x0, x1, [sp, #16 * 0]
    ldp x2, x3, [sp, #16 * 1]
    ldp x4, x5, [sp, #16 * 2]
    ldp x6, x7, [sp, #16 * 3]
    ldp x8, x9, [sp, #16 * 4]
    ldp x10, x11, [sp, #16 * 5]
    ldp x12, x13, [sp, #16 * 6]
    ldp x14, x15, [sp, #16 * 7]
    ldp x16, x17, [sp, #16 * 8]
    ldp x18, x19, [sp, #16 * 9]
    ldp x20, x21, [sp, #16 * 10]
    ldp x22, x23, [sp, #16 * 11]
    ldp x24, x25, [sp, #16 * 12]
    ldp x26, x27, [sp, #16 * 13]
    ldp x28, x29, [sp, #16 * 14]
    ldr x30, [sp, #16 * 15]
    add sp, sp, #256
    eret

/* ---------- 其他异常：打印 ESR/ELR 并停机 ---------- */
.macro EXC_FATAL label
    ldr x0, =\label
    mrs x1, esr_el1
    mrs x2, elr_el1
    bl guest_fatal
1:  wfe
    b 1b
.endm

.section .rodata
msg_sync:
    .asciz "guest_sync_exception"
msg_fiq:
    .asciz "guest_fiq"
msg_serror:
    .asciz "guest_serror"
msg_sync_el0:
    .asciz "guest_sync_from_EL0"

.section .text
/* ---------- 同步异常：先调用 C handler，返回 0 则恢复 ---------- */
exc_sync_sp0:
exc_sync_spx:
    sub sp, sp, #256
    stp x0, x1, [sp, #16 * 0]
    stp x2, x3, [sp, #16 * 1]
    stp x4, x5, [sp, #16 * 2]
    stp x6, x7, [sp, #16 * 3]
    stp x8, x9, [sp, #16 * 4]
    stp x10, x11, [sp, #16 * 5]
    stp x12, x13, [sp, #16 * 6]
    stp x14, x15, [sp, #16 * 7]
    stp x16, x17, [sp, #16 * 8]
    stp x18, x19, [sp, #16 * 9]
    stp x20, x21, [sp, #16 * 10]
    stp x22, x23, [sp, #16 * 11]
    stp x24, x25, [sp, #16 * 12]
    stp x26, x27, [sp, #16 * 13]
    stp x28, x29, [sp, #16 * 14]
    str x30, [sp, #16 * 15]

    mrs x0, esr_el1
    mrs x1, elr_el1
    mrs x2, far_el1
    bl guest_sync_handler
    /* 返回 0 表示已处理，恢复现场并 eret；非 0 表示 fatal */
    cmp x0, #0
    b.eq 1f

    /* fatal 路径：调用 guest_fatal */
    ldr x0, =msg_sync
    mrs x1, esr_el1
    mrs x2, elr_el1
    bl guest_fatal
2:  wfe
    b 2b

1:  /* 正常恢复路径 */
    ldp x0, x1, [sp, #16 * 0]
    ldp x2, x3, [sp, #16 * 1]
    ldp x4, x5, [sp, #16 * 2]
    ldp x6, x7, [sp, #16 * 3]
    ldp x8, x9, [sp, #16 * 4]
    ldp x10, x11, [sp, #16 * 5]
    ldp x12, x13, [sp, #16 * 6]
    ldp x14, x15, [sp, #16 * 7]
    ldp x16, x17, [sp, #16 * 8]
    ldp x18, x19, [sp, #16 * 9]
    ldp x20, x21, [sp, #16 * 10]
    ldp x22, x23, [sp, #16 * 11]
    ldp x24, x25, [sp, #16 * 12]
    ldp x26, x27, [sp, #16 * 13]
    ldp x28, x29, [sp, #16 * 14]
    ldr x30, [sp, #16 * 15]
    add sp, sp, #256
    eret
exc_fiq_sp0:
exc_fiq_spx:
exc_fiq_a64:
exc_fiq_a32:
    EXC_FATAL msg_fiq
exc_error_sp0:
exc_error_spx:
exc_error_a64:
exc_error_a32:
    EXC_FATAL msg_serror
exc_sync_a64:
exc_sync_a32:
    EXC_FATAL msg_sync_el0
