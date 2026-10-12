// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Guest - 主程序
 */

#include "guest.h"

/* 简易 itoa（10 进制，无符号） */
static void puts_dec(u64 v)
{
    char buf[24];
    int i = 0;

    if (v == 0) {
        hvc_putc('0');
        return;
    }

    while (v > 0) {
        buf[i++] = '0' + (char)(v % 10);
        v /= 10;
    }

    while (i > 0)
        hvc_putc(buf[--i]);
}

void guest_puts(const char *s)
{
    while (*s)
        hvc_putc(*s++);
}

u64 guest_timer_freq(void)
{
    /* 通过 hypercall 向 Hypervisor 查询（避免硬编码） */
    return hvc_call(HVC_GET_TIMER_FREQ, 0);
}

/* ---------- 虚拟 GIC（CPU 接口）初始化 ---------- */

/*
 * 学习要点：
 *   这些 ICC_*_EL1 写操作在 EL1 执行时被 GICv3 硬件自动重定向到
 *   "虚拟 CPU 接口"（实际写入 ICH_VMCR_EL2 的后端字段），
 *   完全不会陷入 EL2——guest 用的就是标准 GICv3 驱动代码。
 */
static void gic_vcpu_init(void)
{
    /* 优先级屏蔽：放行所有优先级（0xFF = 最低门槛） */
    icc_pmr_write(0xFF);

    /* 使能 Group 1 中断 */
    icc_igrpen1_write(1);
}

/* ---------- 虚拟定时器 ---------- */

static u64 timer_freq;
static volatile u64 tick_count = 0;

static void vtimer_start(void)
{
    timer_freq = guest_timer_freq();

    /* 第一个到期点：现在 + freq/10（10Hz） */
    u64 now = cntvct_read();
    cntv_cval_write(now + timer_freq / GUEST_TIMER_HZ);

    /* 使能定时器 + 不屏蔽中断（bit0=enable, bit1=imask） */
    cntv_ctl_write(1);
}

/* ---------- IRQ 处理 ---------- */

/*
 * 中断处理入口（vectors.S 调用）
 *
 * 流程是标准的 GICv3 CPU 接口操作：
 *   1. 读 ICC_IAR1_EL1 应答（拿到虚拟中断号）
 *   2. 处理
 *   3. 写 ICC_EOIR1_EL1 结束
 * 注意：这些寄存器访问全部作用于虚拟接口，
 * IAR 读到的是 Hypervisor 通过 ICH_LR 注入的虚拟中断。
 */
void guest_irq_handler(void)
{
    u64 iar = icc_iar1_read();
    u32 intid = (u32)iar;

    if (intid == 0x3FF) /* spurious */
        return;

    if (intid == VIRT_TIMER_IRQ) {
        tick_count++;

        /* 重新装载下一个到期点（周期模式） */
        u64 now = cntvct_read();
        cntv_cval_write(now + timer_freq / GUEST_TIMER_HZ);
        /* 写 CVAL 会自动清除 istatus */

        /* 每秒（10 tick）打印一次心跳 */
        if (tick_count % GUEST_TIMER_HZ == 0) {
            guest_puts("guest: tick ");
            puts_dec(tick_count / GUEST_TIMER_HZ);
            guest_puts(" (timer + vGIC working)\n");
        }
    }

    icc_eoir1_write(iar);
}

/* ---------- 致命异常报告 ---------- */

void guest_fatal(const char *msg, u64 esr, u64 elr)
{
    guest_puts("guest FATAL: ");
    guest_puts(msg);
    guest_puts(" esr=");
    puts_dec(esr);
    guest_puts(" elr=");
    puts_dec(elr);
    guest_puts("\n");
}

/* ---------- 主函数 ---------- */

void guest_main(void)
{
    guest_puts("\nguest: hello from EL1!\n");
    guest_puts("guest: running on aVisor hypervisor (QEMU virt)\n");

    gic_vcpu_init();
    guest_puts("guest: virtual GIC CPU interface initialized\n");

    vtimer_start();
    guest_puts("guest: virtual timer started (10Hz)\n");

    /* 打开 IRQ（PSTATE.I 清零；Hypervisor 启动时 DAIF 是全屏蔽的） */
    asm volatile("msr daifclr, #2" ::: "memory");
    guest_puts("guest: interrupts enabled, entering main loop\n");

    /*
     * 主循环：WFI 睡眠等待虚拟中断。
     *
     * 学习要点：
     *   HCR_EL2.TWI=1，所以 WFI 会 trap 到 EL2——
     *   Hypervisor 借机执行调度（让出 CPU），然后 PC+4 返回这里。
     *   虚拟定时器到期时，虚拟 IRQ 信号（GIC 虚拟接口驱动）
     *   会打断 EL1，进入 vectors.S 的 IRQ 向量。
     */
    while (1) {
        asm volatile("wfi" ::: "memory");
    }
}
