// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - ARM 架构定时器驱动
 *
 * ARMv8-A 架构定义了通用定时器（Generic Timer），它是 CPU 的一部分，
 * 通过系统寄存器访问，而不是 MMIO。每个 CPU 都有自己的定时器。
 *
 * 学习要点：
 *   架构定时器是 ARMv8 标准特性，所有 ARMv8 CPU 都有。
 *   树莓派的 BCM2837 系统定时器是 MMIO 外设，那是芯片厂商加的额外定时器。
 *   架构定时器才是标准的、跨平台的。
 *
 * 定时器级别：
 *   - EL2 物理定时器 (CNTHP_CTL/TVAL/CVAL_EL2)  : Hypervisor 用
 *   - EL1 物理定时器 (CNTP_CTL/TVAL/CVAL_EL0)   : OS 内核用
 *   - EL1 虚拟定时器 (CNTV_CTL/TVAL/CVAL_EL0)   : Guest OS 用
 *
 * 在虚拟化中：
 *   - Hypervisor 用 EL2 物理定时器（作为自己的 tick）
 *   - Guest 的虚拟定时器由 Hypervisor 注入虚拟中断来模拟
 */

#pragma once

#include "common/types.h"

/* 系统寄存器定义 */

/* EL2 物理定时器（Hypervisor 使用） */
#define CNTHP_CTL_EL2    S3_4_C14_C2_1   /* 控制寄存器 */
#define CNTHP_TVAL_EL2   S3_4_C14_C2_0   /* 倒计时值 */
#define CNTHP_CVAL_EL2   S3_4_C14_C2_2   /* 比较值（64位绝对时间） */

/* EL1 物理定时器（Guest EL1 物理定时器） */
#define CNTP_CTL_EL0     S3_3_C14_C2_1
#define CNTP_TVAL_EL0    S3_3_C14_C2_0
#define CNTP_CVAL_EL0    S3_3_C14_C2_2

/* EL1 虚拟定时器（Guest 虚拟定时器） */
#define CNTV_CTL_EL0     S3_3_C14_C3_1
#define CNTV_TVAL_EL0    S3_3_C14_C3_0
#define CNTV_CVAL_EL0    S3_3_C14_C3_2

/* 计数器频率 */
#define CNTFRQ_EL0       S3_3_C14_C0_0   /* 计数器频率 */

/* 当前计数值 */
#define CNTPCT_EL0       S3_3_C14_C0_1   /* 物理计数器 */
#define CNTVCT_EL0       S3_3_C14_C0_2   /* 虚拟计数器 */

/* 定时器控制寄存器位定义 */
#define TIMER_CTL_ENABLE   (1 << 0)   /* 定时器使能 */
#define TIMER_CTL_IMASK    (1 << 1)   /* 中断屏蔽 */
#define TIMER_CTL_ISTATUS  (1 << 2)   /* 中断状态（只读） */

/* Hypervisor 定时器配置 */
#define HYP_TIMER_HZ       100        /* Hypervisor tick 频率：100Hz (10ms) */

/* 函数声明 */
void timer_init(void);
void handle_timer_irq(void);
unsigned long get_physical_timer_count(void);
unsigned long get_timer_frequency(void);

/* 供调度器使用的 tick 回调 */
void timer_tick(void);
