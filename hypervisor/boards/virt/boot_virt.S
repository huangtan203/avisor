// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - QEMU virt 平台启动代码
 *
 * 学习要点：
 *   启动代码是 Hypervisor 最先执行的代码，负责：
 *   1. 识别 CPU ID，只让主核继续运行（次核暂时挂起）
 *   2. 设置 SCTLR（关闭 MMU 和缓存）
 *   3. 判断当前异常级别（EL3 还是 EL2）
 *   4. 从 EL3 降到 EL2（如果需要）
 *   5. 清零 BSS 段
 *   6. 创建页表并开启 MMU
 *   7. 跳转到 C 语言的 hypervisor_main()
 *
 * QEMU virt 平台的特点：
 *   - QEMU 默认把内核加载到 0x40000000 (DRAM 起始)
 *   - QEMU 可以配置启动时的 EL（默认 EL2 或 EL1）
 *   - 用 -machine virt,secure=on 可以有 EL3
 *   - 通常启动级别是 EL2（HYP 模式）
 *
 *   内存映射（等值映射，VA = PA）：
 *     0x4000_0000 ~ 0x4000_0000 + 2MB - 1  => DRAM 低 2MB（代码+数据）
 *     0x0800_0000 ~ 0x0A00_0000            => 外设 MMIO 区域
 *
 *   注意：这里和 raspi 版本的关键区别：
 *   - raspi: 物理地址 0 开始是 RAM，0x3F000000 以上是外设
 *   - virt:  物理地址 0x40000000 以上是 RAM，低地址是外设
 *   所以页表映射的策略完全不同。
 */

#include "arch/aarch64/mmu.h"
#include "arch/aarch64/sysregs.h"
#include "boards/virt/base.h"
#include "boards/virt/virt_mm.h"
#include "common/mm.h"

/*
 * 早期调试宏：直接写 UART 输出一个字符
 * 用于在串口驱动初始化之前调试启动流程
 * 用法：early_putc 'A'
 *
 * 注意：这是最简单的查询式发送，不依赖任何 C 代码或 BSS。
 * 只会破坏 x0, x1 寄存器，所以调用前请保存。
 */
.macro early_putc chr
    ldr     x0, =VIRT_UART_BASE
1:
    ldr     w1, [x0, #0x18]   /* UARTFR */
    tbnz    w1, #5, 1b        /* 等待 TX FIFO 不满 */
    mov     w1, #\chr
    str     w1, [x0, #0x00]   /* UARTDR */
.endm

.section ".text.boot"
.globl _start
_start:
    mrs x0, mpidr_el1
    /* Check processor id */
    and x0, x0, #0xFF
    /* Hang for all non-primary CPU */
    cbz x0, master
    b proc_hang

proc_hang:
    wfe
    b proc_hang

master:
    ldr x0, =SCTLR_VALUE_MMU_DISABLED
    msr sctlr_el2, x0

    /*
     * 判断当前 EL
     * QEMU virt 平台的启动级别：
     *   - 默认（不加 -machine secure=on）: EL2
     *   - 加了 -machine secure=on:       EL3
     * 我们支持两种情况。
     */
    mrs x0, CurrentEL
    lsr x0, x0, #2
    cmp x0, #3
    beq el3
    cmp x0, #2
    beq el2_entry

    /* 如果是 EL1，说明没开虚拟化，直接挂起 */
    b proc_hang

el3:
    /* 从 EL3 降到 EL2 */
    ldr x0, =HCR_VALUE
    msr hcr_el2, x0

    ldr x0, =SCR_VALUE
    msr scr_el3, x0

    ldr x0, =SPSR_VALUE
    msr spsr_el3, x0

    adr x0, el2_entry
    msr elr_el3, x0

    eret

el2_entry:
    /* 清零 BSS 段 */
    adr x0, bss_begin
    adr x1, bss_end
    sub x1, x1, x0
    bl memzero

    /* 创建页表 */
    bl __create_page_tables

    /* 设置栈指针（虚拟地址 = 物理地址，等值映射）
     * 栈放在 LOW_MEMORY 处，从高地址向低地址增长 */
    ldr x0, =LOW_MEMORY
    mov sp, x0

    /* 设置 TTBR0_EL2 指向页表 */
    adrp x0, pg_dir
    msr ttbr0_el2, x0

    /* 配置 TCR_EL2 */
    ldr x0, =(TCR_VALUE)
    msr tcr_el2, x0

    /* 配置 VTCR_EL2（Stage-2 转换） */
    ldr x0, =(VTCR_VALUE)
    msr vtcr_el2, x0

    /* 配置 MAIR_EL2（内存属性索引） */
    ldr x0, =(MAIR_VALUE)
    msr mair_el2, x0

    /* 清除 TLB */
    tlbi alle2
    dsb sy
    isb

    /* 开启 MMU + I-cache + D-cache
     *
     * 学习要点：
     *   开启 MMU 的同时通常也开启缓存。
     *   SCTLR_EL2 的关键位：
     *     - M (bit 0): MMU 使能
     *     - C (bit 2): 数据缓存使能
     *     - I (bit 12): 指令缓存使能
     *     - SA (bit 3): 栈对齐检查（AArch64）
     */
    ldr x0, =(SCTLR_MMU_ENABLED | (1 << 2) | (1 << 12))
    dsb ish
    isb
    msr sctlr_el2, x0
    isb

    /* 跳转到 C 主函数 */
    bl hypervisor_main
    b .

/*
 * 创建一级页表项的宏
 * 从 PGD 开始，逐级创建 PUD、PMD（如果需要的话）
 *
 * 对于 4KB granule + 39-bit VA：
 *   PGD:  level 0, 512 entries, 映射 512GB each
 *   PUD:  level 1, 512 entries, 映射 1GB each
 *   PMD:  level 2, 512 entries, 映射 2MB each (块映射)
 *   PTE:  level 3, 512 entries, 映射 4KB each (页映射)
 *
 * 我们用 2MB 块映射（section map），简单且够用。
 */
    .macro create_pgd_entry, tbl, virt, tmp1, tmp2
    create_table_entry \tbl, \virt, PGD_SHIFT, \tmp1, \tmp2
    create_table_entry \tbl, \virt, PUD_SHIFT, \tmp1, \tmp2
    .endm

    .macro create_table_entry, tbl, virt, shift, tmp1, tmp2
    lsr \tmp1, \virt, #\shift
    and \tmp1, \tmp1, #PTRS_PER_TABLE - 1
    add \tmp2, \tbl, #PAGE_SIZE
    orr \tmp2, \tmp2, #MM_TYPE_PAGE_TABLE
    str \tmp2, [\tbl, \tmp1, lsl #3]
    add \tbl, \tbl, #PAGE_SIZE
    .endm

    .macro create_block_map, tbl, phys, start, end, flags, tmp1
    lsr \start, \start, #SECTION_SHIFT
    and \start, \start, #PTRS_PER_TABLE - 1
    lsr \end, \end, #SECTION_SHIFT
    and \end, \end, #PTRS_PER_TABLE - 1
    lsr \phys, \phys, #SECTION_SHIFT
    mov \tmp1, #\flags
    orr \phys, \tmp1, \phys, lsl #SECTION_SHIFT
1:  str \phys, [\tbl, \start, lsl #3]
    add \start, \start, #1
    add \phys, \phys, #SECTION_SIZE
    cmp \start, \end
    b.ls 1b
    .endm

/*
 * 创建页表
 *
 * 学习要点：
 *   QEMU virt 平台的内存映射策略：
 *
 *   1. 映射 DRAM 区域（0x40000000 ~ 0x40000000 + PHYS_MEMORY_SIZE）
 *      属性：Normal Cacheable（普通内存，可缓存）
 *      用途：Hypervisor 代码、数据、栈、堆、页池
 *
 *   2. 映射 MMIO 外设区域（0x08000000 ~ 0x0A000000）
 *      属性：Device nGnRnE（设备内存，不可缓存）
 *      用途：访问 UART、GIC、RTC 等外设
 *
 *   注意：等值映射（VA = PA），这样开启 MMU 前后地址不变，
 *   代码可以平滑过渡，不会出现"跳转后地址突变"的问题。
 *
 * 页表布局（pg_dir 开始，共 5 页）：
 *   页 0: PGD (Level 0)
 *   页 1: PUD (Level 1) — DRAM 和 MMIO 共享 PUD 表
 *   页 2: PMD_DRAM (Level 2) — PUD[1] 指向，DRAM 的块映射
 *   页 3: PMD_MMIO (Level 2) — PUD[0] 指向，MMIO 的块映射
 *   页 4: 预留
 */
__create_page_tables:
    /* 保存返回地址 */
    mov x29, x30

    /* 清零页表区域 */
    adrp x0, pg_dir
    mov x1, #PG_DIR_SIZE
    bl memzero

    /* ---- 计算各级页表的地址 ---- */
    adrp x0, pg_dir
    add x1, x0, #PAGE_SIZE       /* x1 = PUD 表 = pg_dir + 1 页 */
    add x2, x0, #(2 * PAGE_SIZE) /* x2 = DRAM PMD 表 = pg_dir + 2 页 */
    add x3, x0, #(3 * PAGE_SIZE) /* x3 = MMIO PMD 表 = pg_dir + 3 页 */

    /* ---- 建立 PGD -> PUD 链接 ---- */
    /* PGD[0] 指向 PUD 表（DRAM 和 MMIO 都在 PGD index 0 内） */
    mov x4, x1
    orr x4, x4, #MM_TYPE_PAGE_TABLE
    str x4, [x0]               /* PGD[0] = PUD 基址 | TABLE */

    /* ---- 建立 PUD[0] -> MMIO PMD 链接 ---- */
    /* PUD[0] 对应 0x00000000 ~ 0x3FFFFFFF (0~1GB) */
    mov x4, x3
    orr x4, x4, #MM_TYPE_PAGE_TABLE
    str x4, [x1]               /* PUD[0] = MMIO PMD 基址 | TABLE */

    /* ---- 建立 PUD[1] -> DRAM PMD 链接 ---- */
    /* PUD[1] 对应 0x40000000 ~ 0x7FFFFFFF (1~2GB) */
    mov x4, x2
    orr x4, x4, #MM_TYPE_PAGE_TABLE
    str x4, [x1, #8]           /* PUD[1] = DRAM PMD 基址 | TABLE (每个 entry 8 字节) */

    /* ---- 填充 DRAM PMD 表 ---- */
    /* 虚拟地址范围：0x40000000 ~ 0x40000000 + PHYS_MEMORY_SIZE - 2MB
     * 物理地址：  同虚拟地址（等值映射）
     * 属性：MMU_FLAGS (Normal Cacheable)
     *
     * 注意：DRAM 从 0x40000000 开始，对应 PMD index 0
     * （因为 PMD 在 PUD[1] 内部，PUD[1] 基址是 0x40000000）
     */
    mov x4, #VIRT_DRAM_BASE       /* x4 = phys base */
    lsr x4, x4, #SECTION_SHIFT
    mov x5, #MMU_FLAGS
    orr x4, x5, x4, lsl #SECTION_SHIFT  /* x4 = first block entry */

    mov x5, #0                     /* x5 = start PMD index (0) */
    ldr x6, =(PHYS_MEMORY_SIZE / SECTION_SIZE - 1)  /* x6 = end PMD index */

1:  str x4, [x2, x5, lsl #3]     /* PMD[index] = entry */
    add x5, x5, #1
    add x4, x4, #SECTION_SIZE
    cmp x5, x6
    b.ls 1b

    /* ---- 填充 MMIO PMD 表 ---- */
    /* 虚拟地址范围：VIRT_MMIO_BASE ~ VIRT_MMIO_BASE + VIRT_MMIO_SIZE - 2MB
     * 物理地址：  同虚拟地址（等值映射）
     * 属性：MMU_DEVICE_FLAGS (Device nGnRnE)
     *
     * MMIO 从 0x08000000 开始：
     *   PMD index = 0x08000000 >> SECTION_SHIFT = 0x08000000 >> 21 = 64
     */
    mov x4, #VIRT_MMIO_BASE       /* x4 = phys base */
    lsr x4, x4, #SECTION_SHIFT
    mov x5, #MMU_DEVICE_FLAGS
    orr x4, x5, x4, lsl #SECTION_SHIFT  /* x4 = first block entry */

    mov x5, #(VIRT_MMIO_BASE >> SECTION_SHIFT)  /* x5 = start PMD index */
    and x5, x5, #(PTRS_PER_TABLE - 1)
    add x6, x5, #(VIRT_MMIO_SIZE / SECTION_SIZE - 1)  /* x6 = end PMD index */

2:  str x4, [x3, x5, lsl #3]     /* PMD[index] = entry */
    add x5, x5, #1
    add x4, x4, #SECTION_SIZE
    cmp x5, x6
    b.ls 2b

    /* 恢复返回地址 */
    mov x30, x29
    ret
