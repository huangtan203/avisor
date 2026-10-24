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
#include "common/platform.h"
#include "common/sched.h"
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
    return VA_START + platform_gicr_base(cpu_id);
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
 * GICv3 Redistributor 初始化（指定 CPU 的）
 */
static void gicr_init(int cpu_id)
{
    uint64_t gicr_base = gic_get_redist_base(cpu_id);
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
     * 3.1 使能所有 SGI（0-15，核间软中断）
     *     SGI 是核间通信（IPI）的基础，每个核都需要开启。
     *     PPI（16-31）仍保持禁用，由具体驱动（如定时器）按需使能。
     */
    put32(sgi_base + GICR_ISENABLER0, 0x0000FFFF);

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
     *    EOImode = 1（split EOI 模式）
     *      - 写 EOIR 只做 priority drop（清 APR），不 deactivate
     *      - deactivate 需要单独写 ICC_DIR
     *
     * 为什么需要 split EOI：
     *   虚拟定时器（PPI 27）使用 HW 模式 LR 注入。EL2 ack 物理 27 后，
     *   需要保持物理中断 active 状态来屏蔽电平信号，防止 re-pend 风暴。
     *   Guest 写虚拟 EOIR 时，硬件自动 deactivate 物理中断。
     *   这一切只有在 split EOI 模式下才成立。
     *
     * 对于普通中断（如 PPI 26），处理完后手动写 ICC_DIR deactivate。
     */
    icc_ctlr_write(0x1 << 1);  // EOImode=1 (split EOI)

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
 * GICv3 总初始化入口（主核 CPU0：Distributor + 本核 Redistributor + CPU 接口）
 */
void gic_init(void)
{
    gicd_init();   /* Distributor */
    gicr_init(0);  /* Redistributor (CPU0) */
    gic_cpuif_init(); /* CPU Interface */

    INFO("GICv3 initialized.");
}

/*
 * GICv3 次核初始化（SMP）
 *
 * Distributor 是全局共享的（gic_init 已配置），
 * 次核只需要初始化自己的 Redistributor 和 CPU Interface。
 * 必须在次核上下文中调用（访问本核的系统寄存器）。
 */
void gic_init_secondary(void)
{
    gicr_init(smp_processor_id());
    gic_cpuif_init();

    INFO("GICv3 secondary CPU%d initialized.", smp_processor_id());
}

/*
 * 使能一个中断
 *
 * 对于 SGI/PPI (0-31)，操作【当前 CPU】的 Redistributor
 * 对于 SPI (>=32)，操作 Distributor 的寄存器
 */
void gic_enable_irq(unsigned int irq)
{
    if (irq < 32) {
        /* SGI or PPI: 操作当前 CPU 的 Redistributor */
        uint64_t sgi_base =
            gic_get_redist_base(smp_processor_id()) + GICR_SGI_BASE;
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
        uint64_t sgi_base =
            gic_get_redist_base(smp_processor_id()) + GICR_SGI_BASE;
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
 * AArch64 ICC_SGI1R_EL1 格式（GICv3 亲和路由 ARE_NS=1）：
 *   [55:48]  Aff3
 *   [47:44]  RS     RangeSelector（RSS=0 时 RES0）
 *   [40]     IRM    中断路由模式：1 = 广播给除自身外的所有 PE
 *   [39:32]  Aff2
 *   [27:24]  INTID  SGI 中断号 (0-15)
 *   [23:16]  Aff1
 *   [15:0]   TargetList  aff0 位图：bit n 对应 aff0=n 的 PE
 *
 * QEMU virt 下所有 CPU 的 Aff1/Aff2/Aff3 均为 0，仅 aff0 区分
 * （CPU0 aff0=0, CPU1 aff0=1），故用 TargetList 的 bit 指定目标核。
 */
void gic_send_sgi(unsigned int cpu_id, unsigned int sgi_id)
{
    uint64_t sgi_val;

    sgi_val = ((uint64_t)(sgi_id & 0xF) << 24)   /* INTID [27:24] */
            | (1ULL << (cpu_id & 0xF));            /* TargetList: bit cpu_id */

    asm volatile("dsb ishst" ::: "memory");
    icc_sgi1r_write(sgi_val);
    asm volatile("isb" ::: "memory");
}

/*
 * 把指定 SPI 中断路由到指定 CPU（GICv3 亲和路由，通过 IROUTER）
 *
 * GICD_IROUTER<n> 是 64 位寄存器，低 32 位编码目标亲和：
 *   [7:0]   Aff0
 *   [15:8]  Aff1
 *   [23:16] Aff2
 *   [31:24] Aff3
 * 高 32 位为保留（写 0）。QEMU virt 只用到 Aff0（cpu_id）。
 *
 * 注意：GICD_IROUTER 按【完整 INTID】编址：offset = 0x6000 + 8*INTID，
 * 第一个 SPI（INTID 32）位于 0x6100；INTID < 32（SGI/PPI）的槽位
 * RAZ/WI（QEMU 对这类写入直接忽略）。因此参数 irq 必须直接用全局
 * 中断号（>=32）索引，不能先减 32 转成 SPI 序号。
 */
void gic_set_spi_target(unsigned int irq, unsigned int cpu_id)
{
    if (irq < 32)
        return;

    uint64_t router = GICD_IROUTER(irq);

    uint32_t aff = (cpu_id & 0xFF);  /* Aff0；QEMU virt 只用 aff0 */

    put32(router, aff);
    put32(router + 4, 0);  /* Aff3:Aff2:Aff1 高位清零 */

    asm volatile("dsb sy" ::: "memory");
}
