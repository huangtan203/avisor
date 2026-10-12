// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - SMP 核间通信（IPI）
 *
 * 基于 GICv3 SGI 的核间中断，提供：
 *   1. smp_send_ipi()：向指定物理核发送 SGI
 *   2. smp_ipi_handle()：收到 SGI 后的分发处理
 *   3. smp_online[]：核启动就绪标志（主核等待次核就绪后发送 IPI）
 *
 * SGI 是边沿触发的，经 ICC_SGI1R_EL1 触发后目标核的 Redistributor
 * 记录 pending，待目标核开中断（PSTATE.I=0）且 CPU Interface 使能
 * Group 1 后，即触发一次 IRQ 陷入 EL2。
 */

#include "boards/virt/gicv3.h"
#include "common/debug.h"
#include "common/smp.h"

/* 各核就绪标志：次核在 hypervisor_secondary_main 开中断后置 1 */
volatile int smp_online[MAX_CPUS] = { 0 };

/* 各核收到的 IPI 计数（调试/统计用） */
static volatile unsigned long ipi_count[MAX_CPUS];

/*
 * 向指定物理核发送核间中断
 */
void smp_send_ipi(int cpu, int sgi_id)
{
    gic_send_sgi(cpu, sgi_id);
}

/*
 * 处理当前核收到的 SGI
 *
 * 注意：在 EL2 中断上下文执行（handle_irq 内），此时仍处于
 * split EOI 流程中——SGI 属于普通中断，handle_irq 末尾会统一
 * 写 ICC_DIR 解除激活，这里无需手动处理 active 状态。
 */
void smp_ipi_handle(int sgi_id)
{
    int cpu = smp_processor_id();
    ipi_count[cpu]++;

    switch (sgi_id) {
    case SGI_IPI_NOTIFY:
        printf("[cpu%d] IPI: received SGI#%d (count=%lu)\n",
               cpu, sgi_id, ipi_count[cpu]);
        break;

    case SGI_IPI_TEST:
        printf("[cpu%d] IPI: received SGI#%d (count=%lu)\n",
               cpu, sgi_id, ipi_count[cpu]);
        /*
         * ping-pong：CPU1 收到 CPU0 的 ping 后回一个 pong。
         * （当前仅双核，次核固定回复给主核 CPU0。）
         */
        if (cpu == 1)
            gic_send_sgi(0, SGI_IPI_TEST);
        break;

    default:
        WARN("Unknown SGI#%d", sgi_id);
        break;
    }
}
