// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - GICv3 中断控制器驱动
 *
 * GICv3 是 ARMv8-A 的标准中断控制器，取代了 GICv2。
 * 主要变化：
 *   - CPU 接口从 MMIO 访问改为系统寄存器访问
 *   - 支持亲和路由（Affinity Routing），更适合大规模多核
 *   - 支持 LPI (Locality-specific Peripheral Interrupt)，即 MSI
 *
 * 学习要点：
 *   1. GICv3 的三个主要组件：Distributor, Redistributor, CPU Interface
 *   2. 中断类型：SGI(0-15) / PPI(16-31) / SPI(32-1019)
 *   3. 中断生命周期：pending -> active -> inactive
 *   4. 中断应答 (IAR) 和结束 (EOI) 的操作
 *   5. Group 0 vs Group 1 的概念（安全相关，我们主要用 Group 1）
 *
 * 注意：这是一个简化版的 GICv3 驱动，只实现了基本功能：
 *   - 初始化 Distributor 和 Redistributor
 *   - 使能/禁用中断
 *   - 应答/结束中断
 *   - 发送 SGI（核间中断）
 *   没有实现优先级、抢占、LPI 等高级功能。
 */

#include "boards/virt/gicv3.h"
#include "arch/aarch64/sysregs.h"
#include "common/debug.h"
#include "common/utils.h"

/*
 * 获取指定 CPU 的 Redistributor 基地址
 *
 * QEMU virt 的 GICv3 Redistributor 布局：
 *   CPU0: GICR_BASE + 0 * GICR_STRIDE
 *   CPU1: GICR_BASE + 1 * GICR_STRIDE
 *   ...
 * 每个 Redistributor 有两个 64KB 帧：
 *   - RD_base (控制/配置)
 *   - SGI_base (SGI+PPI 寄存器)
 */
uint64_t gic_get_redist_base(int cpu_id)
{
    return GICR_BASE + cpu_id * VIRT_GICR_STRIDE;
}

/*
 * 写入 ICC 系统寄存器的辅助宏
 * 因为 ICC 寄存器名是"寄存器名加后缀"的形式，
 * 不能直接用 C 变量拼接，所以用宏来处理
 */

/* 读取 ICC_PMR_EL1（优先级掩码） */
static inline unsigned int icc_pmr_read(void)
{
    return READ_SYSREG(ICC_PMR_EL1);
}

/* 写入 ICC_PMR_EL1 */
static inline void icc_pmr_write(unsigned int val)
{
    WRITE_SYSREG(val, ICC_PMR_EL1);
}

/* 读取 ICC_IAR1_EL1（中断应答，Group 1） */
static inline unsigned int icc_iar1_read(void)
{
    return READ_SYSREG(ICC_IAR1_EL1);
}

/* 写入 ICC_EOIR1_EL1（中断结束，Group 1） */
static inline void icc_eoir1_write(unsigned int val)
{
    WRITE_SYSREG(val, ICC_EOIR1_EL1);
}

/* 写入 ICC_SGI1R_EL1（触发 SGI） */
static inline void icc_sgi1r_write(uint64_t val)
{
    WRITE_SYSREG(val, ICC_SGI1R_EL1);
}

/* 写入 ICC_IGRPEN1_EL1（Group 1 中断使能） */
static inline void icc_igrpen1_write(unsigned int val)
{
    WRITE_SYSREG(val, ICC_IGRPEN1_EL1);
}

/* 写入 ICC_CTLR_EL1（CPU 接口控制） */
static inline void icc_ctlr_write(unsigned int val)
{
    WRITE_SYSREG(val, ICC_CTLR_EL1);
}

/* 读取 ICC_SRE_EL2 */
static inline unsigned int icc_sre_el2_read(void)
{
    return READ_SYSREG(ICC_SRE_EL2);
}

/* 写入 ICC_SRE_EL2 */
static inline void icc_sre_el2_write(unsigned int val)
{
    WRITE_SYSREG(val, ICC_SRE_EL2);
}

/*
 * GICv3 Distributor 初始化
 */
static void gicd_init(void)
{
    unsigned int typer;
    unsigned int it_lines;

    /* 读取 GIC 类型，获取支持的中断数 */
    typer = get32(GICD_TYPER);
    it_lines = ((typer & 0x1F) + 1) * 32;  /* ITLinesNumber 字段 */

    INFO("GICv3: support %u SPI interrupts", it_lines - 32);

    /*
     * 1. 关闭 Distributor，便于配置
     *    先禁用所有中断组
     */
    put32(GICD_CTLR, 0);

    /*
     * 2. 把所有 SPI 中断配置为 Group 1
     *    IGROUPR 寄存器：0 = Group 0, 1 = Group 1
     *    我们所有中断都用 Group 1
     */
    for (unsigned int i = 1; i < (it_lines / 32); i++) {
        /* i=0 是 SGI+PPI，在 Redistributor 里配置 */
        put32(GICD_IGROUPR(i), 0xFFFFFFFF);  /* 全设为 Group 1 */
    }

    /*
     * 3. 禁用所有 SPI 中断
     *    （默认就是禁用的，保险起见再写一次）
     */
    for (unsigned int i = 1; i < (it_lines / 32); i++) {
        put32(GICD_ICENABLER(i), 0xFFFFFFFF);
    }

    /*
     * 4. 清除所有 SPI 中断的挂起状态
     */
    for (unsigned int i = 1; i < (it_lines / 32); i++) {
        put32(GICD_ICPENDR(i), 0xFFFFFFFF);
    }

    /*
     * 5. 设置所有 SPI 中断为电平触发（默认配置）
     *    ICFGR: 0 = 电平触发, 2 = 边沿触发
     */
    for (unsigned int i = 2; i < (it_lines / 16); i++) {
        /* i=0,1 是 SGI+PPI，在 Redistributor 里配置 */
        put32(GICD_ICFGR(i), 0x00000000);
    }

    /*
     * 6. 设置所有 SPI 中断的优先级为中等
     *    优先级值越小优先级越高，0x80 是中等偏低
     */
    for (unsigned int i = 2; i < (it_lines / 4); i++) {
        /* i=0,1 是 SGI+PPI */
        put32(GICD_IPRIORITYR(i), 0x80808080);
    }

    /*
     * 7. 使能 Distributor（Group 1 Non-secure）
     *    同时使能亲和路由（ARE_NS），这是 GICv3 的标准模式
     */
    put32(GICD_CTLR, GICD_CTLR_ENABLE_G1NS | GICD_CTLR_ARE_NS);
}

/*
 * GICv3 Redistributor 初始化（当前 CPU 的）
 */
static void gicr_init(void)
{
    uint64_t gicr_base = gic_get_redist_base(0);  /* CPU0 */
    uint64_t sgi_base = gicr_base + GICR_SGI_BASE;

    /*
     * 1. 唤醒 Redistributor（退出睡眠状态）
     *    WAKER.ProcessorSleep = 0
     *
     * 注意：有些 GICv3 实现需要等 ChildrenAsleep 位清零，
     * QEMU 的实现比较简单，直接写就行。
     */
    put32(gicr_base + GICR_WAKER,
          get32(gicr_base + GICR_WAKER) & ~GICR_WAKER_ProcessorSleep);

    /* 等待唤醒完成 */
    while (get32(gicr_base + GICR_WAKER) & GICR_WAKER_ChildrenAsleep)
        ;

    /*
     * 2. 配置 SGI + PPI 为 Group 1
     */
    put32(sgi_base + GICR_IGROUPR0, 0xFFFFFFFF);

    /*
     * 3. 禁用所有 SGI + PPI 中断
     */
    put32(sgi_base + GICR_ICENABLER0, 0xFFFFFFFF);

    /*
     * 4. 清除所有 SGI + PPI 中断的挂起状态
     */
    put32(sgi_base + GICR_ICPENDR0, 0xFFFFFFFF);

    /*
     * 5. 设置所有 SGI + PPI 的优先级
     */
    for (int i = 0; i < 8; i++) {
        put32(sgi_base + GICR_IPRIORITYR(i), 0x80808080);
    }

    /*
     * 6. SGI 配置为边沿触发，PPI 配置为电平触发
     *    ICFGR0 (SGI): 全设为边沿触发
     *    ICFGR1 (PPI): 全设为电平触发
     */
    put32(sgi_base + GICR_ICFGR0, 0xFFFFFFFF);  /* SGI: 边沿触发 */
    put32(sgi_base + GICR_ICFGR1, 0x00000000);  /* PPI: 电平触发 */
}

/*
 * GICv3 CPU Interface 初始化
 *
 * 这部分通过系统寄存器访问，配置当前 CPU 的中断接口。
 */
static void gic_cpuif_init(void)
{
    /*
     * 0. 启用 ICC 系统寄存器访问（EL2）
     *    在 EL2 下，必须先设置 ICC_SRE_EL2.SRE = 1，
     *    否则所有 ICC_*_EL1 系统寄存器访问都会被忽略，
     *    GIC CPU 接口处于 MMIO 兼容模式（GICv2 风格）。
     */
    icc_sre_el2_write(ICC_SRE_SRE);
    asm volatile ("isb" ::: "memory");
    {
        unsigned int sre = icc_sre_el2_read();
        INFO("GICv3: ICC_SRE_EL2 = 0x%x (SRE=%d)", sre, sre & ICC_SRE_SRE);
    }

    /*
     * 1. 设置优先级掩码
     *    0xFF = 屏蔽所有中断
     *    0x00 = 允许所有中断
     *    我们设为 0xF0，允许大部分优先级的中断
     */
    icc_pmr_write(0xF0);

    /*
     * 2. 配置 ICC_CTLR
     *    EOImode = 0（EOI 同时做 priority drop 和 deactivate）
     *    简单模式，一步完成中断结束
     */
    icc_ctlr_write(0x0);

    /*
     * 3. 使能 Group 1 中断
     */
    icc_igrpen1_write(1);

    /*
     * 内存屏障确保配置生效
     */
    asm volatile ("dsb sy" ::: "memory");
    asm volatile ("isb" ::: "memory");
}

/*
 * GICv3 总初始化入口
 */
void gic_init(void)
{
    gicd_init();   /* Distributor */
    gicr_init();   /* Redistributor (CPU0) */
    gic_cpuif_init(); /* CPU Interface */

    INFO("GICv3 initialized.");
}

/*
 * 使能一个中断
 *
 * 对于 SGI/PPI (0-31)，操作 Redistributor 的寄存器
 * 对于 SPI (>=32)，操作 Distributor 的寄存器
 */
void gic_enable_irq(unsigned int irq)
{
    if (irq < 32) {
        /* SGI or PPI: 操作当前 CPU 的 Redistributor */
        uint64_t sgi_base = gic_get_redist_base(0) + GICR_SGI_BASE;
        put32(sgi_base + GICR_ISENABLER0, 1 << irq);
    } else {
        /* SPI: 操作 Distributor */
        unsigned int reg_idx = irq / 32;
        unsigned int bit_idx = irq % 32;
        put32(GICD_ISENABLER(reg_idx), 1 << bit_idx);
    }
}

/*
 * 禁用一个中断
 */
void gic_disable_irq(unsigned int irq)
{
    if (irq < 32) {
        uint64_t sgi_base = gic_get_redist_base(0) + GICR_SGI_BASE;
        put32(sgi_base + GICR_ICENABLER0, 1 << irq);
    } else {
        unsigned int reg_idx = irq / 32;
        unsigned int bit_idx = irq % 32;
        put32(GICD_ICENABLER(reg_idx), 1 << bit_idx);
    }
}

/*
 * 设置中断优先级
 */
void gic_set_priority(unsigned int irq, unsigned int priority)
{
    /* 简单起见，只支持 SPI，且优先级用 8 位 */
    if (irq >= 32) {
        unsigned int reg_idx = irq / 4;
        unsigned int byte_idx = irq % 4;
        unsigned int val = get32(GICD_IPRIORITYR(reg_idx));
        val &= ~(0xFF << (byte_idx * 8));
        val |= (priority & 0xFF) << (byte_idx * 8);
        put32(GICD_IPRIORITYR(reg_idx), val);
    }
}

/*
 * 应答中断：读取当前最高优先级的挂起中断号
 * 返回中断号，如果是虚假中断返回 1023
 */
unsigned int gic_ack_irq(void)
{
    return icc_iar1_read() & 0xFFFFFF;  /* 低 24 位是中断号 */
}

/*
 * 结束中断：通知 GIC 该中断已处理完毕
 */
void gic_eoi_irq(unsigned int irq)
{
    icc_eoir1_write(irq);
    asm volatile ("dsb sy" ::: "memory");
}

/*
 * 发送 SGI（核间中断）
 *
 * ICC_SGI1R_EL1 格式：
 *   [23:0]   目标列表（target list）或亲和值
 *   [24]     保留
 *   [25]     IRM（中断路由模式：0 = 列表模式，1 = 全部）
 *   [26]     保留
 *   [27]     RS（路由模式：0 = 旧版，1 = 亲和）
 *   [31:28]  SGI 中断号
 *   [39:32]  Aff0（亲和等级 0）
 *   [47:40]  Aff1
 *   [55:48]  Aff2
 *   [63:56]  Aff3
 *
 * 简单起见，我们用"发送给所有 CPU"的模式（IRM=1）
 */
void gic_send_sgi(unsigned int cpu_id, unsigned int sgi_id)
{
    uint64_t sgi_val;

    /*
     * 这里用最简单的模式：
     *  - 旧版路由 (RS=0)
     *  - 目标列表模式 (IRM=0)
     *  - 目标 CPU 由 target list 指定
     *
     * 注意：在 GICv3 亲和路由模式下，正确的做法是用 Aff0/Aff1/Aff2/Aff3
     * 但为了简化，QEMU 的 GICv3 也兼容旧的 target list 方式。
     *
     * 实际上在 ARE 模式下，应该用 ICC_SGI1R_EL1 的亲和格式。
     * 这里我们用亲和模式（RS=1），CPU0 的亲和值是 0.0.0.0
     */

    sgi_val = ((uint64_t)(sgi_id & 0xF) << 24)  /* INTID */
            | ((uint64_t)1 << 40)               /* Aff1 = 1 (表示CPU 0) */
            | ((uint64_t)1 << 0);                /* 目标列表第 0 位 */

    /* 更简单的做法：发给所有 CPU (IRM=1) */
    sgi_val = ((uint64_t)(sgi_id & 0xF) << 24) | (1ULL << 40);

    icc_sgi1r_write(sgi_val);
}
