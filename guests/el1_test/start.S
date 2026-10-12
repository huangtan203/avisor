// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Guest - 启动代码 (EL1 入口)
 *
 * Hypervisor 通过 ERET 把 PC 设到这里（IPA 0x40000000），
 * 此时 EL1 的状态：
 *   - MMU 关闭（VA == IPA，经 Stage 2 翻译到真实物理页）
 *   - PSTATE = EL1h，DAIF 全屏蔽
 *   - SP 未定义（pt_regs.sp 不会被恢复）—— 必须在这里自己设栈
 *
 * 这和真机 bootloader 跳进 kernel 前的状态非常像。
 */

.section .text.start

.globl _start
_start:
    /* 设置 EL1 异常向量表 */
    ldr x0, =vectors
    msr vbar_el1, x0

    /* 设置栈（向下增长） */
    ldr x0, =0x40400000        /* GUEST_STACK_TOP */
    mov sp, x0

    /* 清零 BSS（loader 只拷贝了镜像中实际存在的段） */
    ldr x0, =__bss_start
    ldr x1, =__bss_end
1:  cmp x0, x1
    b.hs 2f
    str xzr, [x0], #8
    b 1b
2:
    /* 跳到 C 主函数 */
    bl guest_main

    /* guest_main 不应返回；万一返回了，死循环 */
3:  wfe
    b 3b
