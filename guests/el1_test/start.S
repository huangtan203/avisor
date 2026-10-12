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
    /*
     * 读取 MPIDR 判断当前 CPU ID
     * 主核 (cpu 0) 走 master 路径
     * 次核 走 secondary 路径
     *
     * 注意：HCR_EL2.TID1=1，读 MPIDR 会陷入 Hypervisor，
     * Hypervisor 返回虚拟 MPIDR（每个 vCPU 不同），
     * Guest 借此区分自己是主 vCPU 还是次 vCPU。
     */
    mrs x0, mpidr_el1
    and x0, x0, #0xFF
    cbz x0, master
    b secondary_entry

/*
 * 次核启动入口（虚拟 SMP 的第二个 vCPU）
 * 次核有自己的栈，从 guest_secondary_main 开始执行
 */
secondary_entry:
    /* 设置 EL1 异常向量表（和主核共享） */
    ldr x1, =vectors
    msr vbar_el1, x1

    /* 设置次核栈（每个核 16KB 栈空间）
     * CPU0 栈: 0x40400000 (GUEST_STACK_TOP)
     * CPU1 栈: 0x40400000 - 0x4000
     */
    ldr x1, =0x40400000
    mov x2, #0x4000
    mul x2, x0, x2
    sub x1, x1, x2
    mov sp, x1

    /* 清零 BSS —— 注意：BSS 是 VM 全局的，只能清零一次
     * 主核负责清零 BSS，次核直接跳过 */

    /* 跳转到次核 C 入口 */
    bl guest_secondary_main

    /* 不应返回 */
3:  wfe
    b 3b

master:
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
