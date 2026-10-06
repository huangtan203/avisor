// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - ARM 架构定时器驱动
 *
 * ARMv8-A 的通用定时器（Generic Timer）是 CPU 的标准功能，
 * 通过系统寄存器访问，每个 CPU 都有独立的定时器。
 *
 * 学习要点：
 *   1. 架构定时器 vs MMIO 定时器的区别
 *      - 架构定时器：属于 CPU 本身，通过系统寄存器访问，每个 CPU 一个
 *      - MMIO 定时器：属于外设，通过内存映射访问，全局共享
 *
 *   2. Hypervisor 为什么要用 EL2 物理定时器？
 *      - EL2 是 Hypervisor 的特权级，需要自己的定时器 tick
 *      - Guest OS 用的虚拟定时器（CNTV）由 Hypervisor 模拟
 *      - 物理定时器（CNTP）可以留给 Guest EL1 直接用（trap 或者 passthrough）
 *
 *   3. 定时器的工作模式
 *      - 比较值模式 (CVAL): 设定一个绝对时间，计数器到了就触发中断
 *      - 倒计时模式 (TVAL): 设定一个相对时间，倒计时到 0 触发中断
 *      两种模式是同一个定时器的不同访问方式，写 TVAL 会自动更新 CVAL。
 *
 *   4. 定时器中断是 PPI (Private Peripheral Interrupt)
 *      - 每个 CPU 自己的定时器中断不共享
 *      - PPI #26 是 EL2 物理定时器（在 GICv3 里叫 EL2 Physical Timer）
 *      - PPI #30 是 EL1 非安全物理定时器
 *      - PPI #27 是虚拟定时器
 */

#include "boards/virt/arch_timer.h"
#include "boards/virt/gicv3.h"
#include "arch/aarch64/sysregs.h"
#include "common/board.h"
#include "common/debug.h"
#include "common/sched.h"
#include "common/utils.h"

/* 全局变量：定时器频率 */
static unsigned long timer_freq = 0;

/* 全局 tick 计数（每 100Hz tick = 1 秒） */
static volatile unsigned long hyp_tick_count = 0;

/*
 * 读取计数器频率
 * CNTFRQ_EL0 记录了系统计数器的频率，由固件设置。
 * QEMU virt 平台通常是 62.5 MHz 或 1GHz。
 */
unsigned long get_timer_frequency(void)
{
    if (timer_freq == 0) {
        timer_freq = READ_SYSREG(CNTFRQ_EL0);
    }
    return timer_freq;
}

/*
 * 获取当前物理计数值
 * CNTPCT_EL0 是 64 位的自由运行计数器
 */
unsigned long get_physical_timer_count(void)
{
    return READ_SYSREG(CNTPCT_EL0);
}

/*
 * 定时器中断处理函数
 *
 * 学习要点：
 *   这是 Hypervisor 的 tick 中断处理入口，
 *   相当于 Linux 的 scheduler_tick()。
 *   每次定时器中断触发，我们做：
 *   1. 设置下一次中断的时间（周期性）
 *   2. 调用调度器的 tick 处理
 *   3. （可选）打印 tick 信息，验证系统在运行
 */
void handle_timer_irq(void)
{
    unsigned long interval;

    /* 计算下一次中断的间隔（以 ticks 为单位） */
    interval = get_timer_frequency() / HYP_TIMER_HZ;

    /*
     * 设置下一个定时器中断（用 TVAL 倒计时模式）
     * 写 TVAL 会自动重新使能定时器并清除中断状态
     */
    WRITE_SYSREG(interval, CNTHP_TVAL_EL2);

    /* 增加 tick 计数 */
    hyp_tick_count++;

    /* 每秒打印一次（验证定时器中断在正常工作） */
    if (hyp_tick_count % HYP_TIMER_HZ == 0) {
        printf("[tick %lu] Hypervisor is alive at EL2\n", hyp_tick_count);
    }

    /* 调用调度器的 tick 函数（可能触发任务调度） */
    timer_tick();
}

/*
 * 架构定时器初始化
 *
 * 学习要点：
 *   初始化步骤：
 *   1. 读取计数器频率（确认可用）
 *   2. 设置中断间隔
 *   3. 使能定时器
 *   4. 在 GIC 中使能对应的 PPI 中断
 *
 * 注意：EL2 物理定时器的中断号是 PPI #26
 * 在 GICv3 中，PPI 中断号是 16-31，#26 就是 EL2 物理定时器
 */
void timer_init(void)
{
    unsigned long interval;

    /* 1. 读取并打印频率 */
    timer_freq = get_timer_frequency();
    INFO("Architected Timer frequency: %lu Hz", timer_freq);

    /* 2. 计算 tick 间隔 */
    interval = timer_freq / HYP_TIMER_HZ;
    INFO("Timer tick: %lu Hz, interval: %lu ticks", HYP_TIMER_HZ, interval);

    /*
     * 3. 配置 EL2 物理定时器
     *    - 设置初始倒计时值
     *    - 使能定时器，不屏蔽中断
     */
    WRITE_SYSREG(interval, CNTHP_TVAL_EL2);
    WRITE_SYSREG(TIMER_CTL_ENABLE, CNTHP_CTL_EL2);

    /*
     * 4. 在 GIC 中使能 EL2 物理定时器 PPI
     *    PPI #26 是 EL2 Hypervisor Timer
     *
     * 注意：在 EL2 下，我们用的是 EL2 物理定时器，
     * 它的 PPI 中断号是 26（在 GICv3 规范里）。
     * 但实际上不同 GIC 版本/实现可能有差异，
     * QEMU virt + GICv3 下 EL2 物理定时器就是 PPI #26。
     */
    gic_enable_irq(PPI_EL2_PHYS_TIMER);

    INFO("Architected Timer initialized.");
}

/*
 * 这个符号供外部引用（和 raspi 版本的 handle_timer1_irq 对应）
 * 保持接口兼容
 */
void handle_timer1_irq(void)
{
    handle_timer_irq();
}

/*
 * handle_timer3_irq: raspi 平台才有（系统定时器 channel 3）
 * virt 平台没有，提供空实现
 */
void handle_timer3_irq(void)
{
    /* 空实现 */
}

/*
 * get_system_timer: 获取系统时间（微秒）
 * virt 平台用架构定时器计数器换算
 */
unsigned long get_system_timer(void)
{
    unsigned long cnt = get_physical_timer_count();
    unsigned long freq = get_timer_frequency();
    return (cnt * 1000000) / freq;
}
