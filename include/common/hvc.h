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
