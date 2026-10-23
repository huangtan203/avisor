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
int smp_freeze_begin(int target_cpu);
int smp_freeze_release(void);
unsigned long smp_freeze_progress(int cpu);
unsigned long smp_freeze_ack(int cpu);
int smp_freeze_snapshot_status(int cpu, unsigned long *generation);

/* Phase 31: 事务式冻结（quiesce + commit/rollback）
 *
 * 与 Phase 27 的瞬时冻结不同，事务冻结在目标核 ACK 后保持冻结，
 * 直到协调核显式 COMMIT（按冻结点保存的现场恢复执行）或
 * ROLLBACK（从冻结点重新执行）。冻结期间目标 vCPU 完全静止，
 * 因此快照保持有效，可安全用于迁移等状态消费。
 */
int smp_txn_freeze_begin(int target_cpu);
int smp_txn_freeze_commit(void);
int smp_txn_freeze_rollback(void);
int smp_txn_freeze_status(void);

/* Phase 32: VM 级双 vCPU 事务冻结（stop-the-world）
 *
 * Phase 31 只冻结一个远端 vCPU，协调核自身继续执行；本接口把
 * 协调核（vCPU0）也纳入冻结——BEGIN 在 HVC 边界同步捕获自身快照，
 * 整个 VM 的两个 vCPU 同时静止，形成 VM 级一致快照窗口。
 *
 * BEGIN 返回值：
 *   0  = 冻结成功（正常路径）
 *   1  = 回放：上次 ROLLBACK 后 vCPU0 的现场被回卷到 BEGIN 的
 *        HVC 指令重新执行——Guest 借此感知"回到了事务入口"
 *  -1  = 失败（无 vCPU1 / 事务进行中 / 超时）
 */
int smp_vm_txn_begin(void);
int smp_vm_txn_commit(void);
int smp_vm_txn_rollback(void);
int smp_vm_txn_status(void);
