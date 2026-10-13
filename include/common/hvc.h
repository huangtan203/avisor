// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - 半虚拟化调用（Hypercall）接口
 *
 * 学习要点：
 *   Hypervisor 和 Guest 之间的"系统调用"叫 hypercall：
 *   Guest 执行 HVC 指令（Hypervisor Call），陷入 EL2。
 *   这是半虚拟化（paravirtualization）的核心机制，
 *   Xen/KVM 都用类似方式提供 console、IPI、内存管理等接口。
 *
 *   调用约定（本项目自定义，类似 Linux KVM 的 smccc 风格简化版）：
 *     x8 - hypercall 功能号
 *     x0~x7 - 参数 / 返回值
 *
 *   注意：entry.S 的 el01_sync 会把 x8 作为 hvc_nr 传入
 *   handle_sync_exception（项目约定，不走 ESR.ISS 的 HVC 立即数）。
 */

#pragma once

/* hypercall 功能号 */
#define HVC_CONSOLE_PUTC 1 /* x0 = 字符，输出到 Hypervisor 控制台 */
#define HVC_GET_TIMER_FREQ 2 /* 返回值 x0 = 定时器频率（Hz） */
#define HVC_GET_VMID 3 /* 返回值 x0 = 当前 VM 的 ID（pid，Phase 3） */
#define HVC_PCI_DEBUG 4 /* PCI 直通中断链路诊断 dump（Phase 7 调试用） */

/* Phase 10: 新增 hypercall */
#define HVC_SCHED_YIELD     10  /* 主动让出 CPU，返回 0 */
#define HVC_SCHED_GET_PRIO  11  /* 获取当前任务优先级，返回 prio */
#define HVC_SCHED_SET_PRIO  12  /* x0 = prio，设置优先级，返回 0/-1 */
#define HVC_MEM_MAP         20  /* 映射内存页：x0=ipa x1=flags，返回 0/-1 */
#define HVC_MEM_UNMAP       21  /* 取消映射：x0=ipa，返回 0/-1 */
#define HVC_SHM_CREATE      30  /* 创建共享内存区：x0=key x1=size，返回 shmid/-1 */
#define HVC_SHM_ATTACH      31  /* 附加共享内存：x0=shmid x1=ipa，返回 0/-1 */
#define HVC_SHM_DETACH      32  /* 分离共享内存：x0=shmid，返回 0/-1 */
#define HVC_SHM_NOTIFY      33  /* 通知对端：x0=shmid，返回 0/-1 */

/* Phase 12: 虚拟 SGI（vSGI）hypercall */
#define HVC_VSGI_SEND       40  /* 发送虚拟 SGI：x0=target_vcpu_id x1=sgi_id，返回 0/-1 */
#define HVC_VSGI_GET_CPUID  41  /* 获取当前 vCPU ID，返回 vcpu_id */

/* Phase 15: 虚拟 GICD 测试 hypercall */
#define HVC_GICD_INJECT     42  /* x0=vintid，直接经 vgic_inject_virq 注入，返回 0/-1 */
