// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - SMP 核间通信（IPI）
 *
 * 基于 GICv3 SGI（Software Generated Interrupt）的核间中断机制：
 *   - smp_send_ipi(cpu, sgi_id)：向指定物理核发送核间中断
 *   - smp_ipi_handle(sgi_id)：当前核收到 SGI 后的分发处理
 *   - smp_online[cpu]：各核初始化完成标志，用于启动握手
 *
 * SGI 中断号约定（见 boards/virt/gicv3.h）：
 *   - SGI_IPI_NOTIFY (0)：通用核间通知
 *   - SGI_IPI_TEST   (1)：核间通信 ping-pong 测试
 */

#pragma once

#include "common/sched.h"

/* 各核初始化完成标志：smp_online[cpu] == 1 表示该核已开中断、可接收 IPI */
extern volatile int smp_online[MAX_CPUS];

/* 向指定物理核发送核间中断（SGI） */
void smp_send_ipi(int cpu, int sgi_id);

/* 处理当前核收到的 SGI（在 irq.c 的 handle_irq 中分发调用） */
void smp_ipi_handle(int sgi_id);
