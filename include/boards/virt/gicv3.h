// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - GICv3 中断控制器驱动
 *
 * GIC (Generic Interrupt Controller) 是 ARM 官方的通用中断控制器标准。
 * GICv3 是 ARMv8-A 平台的标准版本，支持：
 *   - 多达 1024 个 SPI 中断
 *   - 消息信号中断 (MSI/LPI)
 *   - 亲和路由（affinity routing），支持多核
 *
 * 学习要点：
 *   真实 ARM 平台（包括 AGX Orin）几乎都用 GIC，掌握 GIC 编程
 *   比学 BCM2837 那种自定义中断控制器更有通用价值。
 *   GICv3 的关键模块：
 *     - Distributor (GICD)：全局中断分发，配置所有中断
 *     - Redistributor (GICR)：每个 CPU 一个，管理私有中断
 *     - CPU Interface (ICC)：每个 CPU 一个，系统寄存器访问
 *
 * 中断类型：
 *   - SGI (0-15)  : Software Generated Interrupt，软中断，用于核间通信
 *   - PPI (16-31) : Private Peripheral Interrupt，每个CPU私有（如定时器）
 *   - SPI (32-1019): Shared Peripheral Interrupt，共享外设中断
 *
 * QEMU virt 平台上 GICv3 的地址：
 *   GICD: 0x08000000 (Distributor)
 *   GICR: 0x080A0000 (Redistributor base, 每个 CPU 0x20000 间距)
 */

#pragma once

#include "boards/virt/base.h"
#include "common/types.h"

/* ============ GIC Distributor (GICD) 寄存器 ============ */

#define GICD_CTLR           (GICD_BASE + 0x0000)  /* 控制寄存器 */
#define GICD_TYPER          (GICD_BASE + 0x0004)  /* 类型寄存器 */
#define GICD_IIDR           (GICD_BASE + 0x0008)  /* 实现者ID */
#define GICD_IGROUPR(n)     (GICD_BASE + 0x0080 + (n) * 4)  /* 中断组 */
#define GICD_ISENABLER(n)   (GICD_BASE + 0x0100 + (n) * 4)  /* 中断使能置位 */
#define GICD_ICENABLER(n)   (GICD_BASE + 0x0180 + (n) * 4)  /* 中断使能清除 */
#define GICD_ISPENDR(n)     (GICD_BASE + 0x0200 + (n) * 4)  /* 中断挂起置位 */
#define GICD_ICPENDR(n)     (GICD_BASE + 0x0280 + (n) * 4)  /* 中断挂起清除 */
#define GICD_ISACTIVER(n)   (GICD_BASE + 0x0300 + (n) * 4)  /* 中断活动置位 */
#define GICD_ICACTIVER(n)   (GICD_BASE + 0x0380 + (n) * 4)  /* 中断活动清除 */
#define GICD_IPRIORITYR(n)  (GICD_BASE + 0x0400 + (n) * 4)  /* 中断优先级 */
#define GICD_ITARGETSR(n)   (GICD_BASE + 0x0800 + (n) * 4)  /* 中断目标CPU (GICv2) */
#define GICD_ICFGR(n)       (GICD_BASE + 0x0C00 + (n) * 4)  /* 中断配置 */
#define GICD_IROUTER(n)     (GICD_BASE + 0x6000 + (n) * 8)  /* 中断路由 (GICv3) */

/* GICD_CTLR 位 */
#define GICD_CTLR_ENABLE_G0       (1 << 0)   /* 使能 Group 0 */
#define GICD_CTLR_ENABLE_G1NS     (1 << 1)   /* 使能 Group 1 Non-secure */
#define GICD_CTLR_ENABLE_G1S      (1 << 2)   /* 使能 Group 1 Secure */
#define GICD_CTLR_ARE_NS          (1 << 4)   /* 亲和路由使能 (Non-secure) */
#define GICD_CTLR_ARE_S           (1 << 5)   /* 亲和路由使能 (Secure) */

/* ============ GIC Redistributor (GICR) 寄存器 ============ */

/* GICR 有两个 64KB 的帧：RD_base 和 SGI_base */
#define GICR_CTLR           0x0000  /* 控制 */
#define GICR_IIDR           0x0004  /* 实现者ID */
#define GICR_TYPER          0x0008  /* 类型 */
#define GICR_WAKER          0x0014  /* 唤醒控制 */
#define GICR_PROPBASER      0x0070  /* LPI 配置表基址 */
#define GICR_PENDBASER      0x0078  /* LPI 挂起表基址 */

/* SGI/PPI 帧偏移 (第二个 64KB) */
#define GICR_SGI_BASE       0x10000
/* 注意：以下寄存器偏移是相对于 SGI_base 的，即已经包含在 GICR_SGI_BASE 内 */
#define GICR_IGROUPR0       0x0080  /* SGI+PPI 中断组 */
#define GICR_ISENABLER0     0x0100  /* SGI+PPI 使能置位 */
#define GICR_ICENABLER0     0x0180  /* SGI+PPI 使能清除 */
#define GICR_ISPENDR0       0x0200  /* SGI+PPI 挂起置位 */
#define GICR_ICPENDR0       0x0280  /* SGI+PPI 挂起清除 */
#define GICR_ISACTIVER0     0x0300  /* SGI+PPI 活动置位 */
#define GICR_ICACTIVER0     0x0380  /* SGI+PPI 活动清除 */
#define GICR_IPRIORITYR(n)  (0x0400 + (n) * 4)  /* 优先级 */
#define GICR_ICFGR0         0x0C00  /* SGI 配置 */
#define GICR_ICFGR1         0x0C04  /* PPI 配置 */

/* GICR_WAKER 位 */
#define GICR_WAKER_ProcessorSleep  (1 << 1)  /* CPU 睡眠状态 */
#define GICR_WAKER_ChildrenAsleep  (1 << 2)  /* 子通道睡眠 */

/* ============ CPU Interface 系统寄存器 (ICC_xxx_EL1) ============
 *
 * GICv3 的 CPU 接口通过系统寄存器访问，而不是 MMIO。
 * 这是 GICv3 相比 GICv2 的重要变化之一。
 */

/* ICC_SRE_EL2: System Register Enable (EL2)
 * 必须设置 SRE 位，否则 ICC 系统寄存器访问会被忽略
 */
#define ICC_SRE_EL2        S3_4_C12_C9_5
#define ICC_SRE_SRE        (1 << 0)   /* 系统寄存器使能 */
#define ICC_SRE_DFB        (1 << 1)   /* 禁用 FIQ 旁路 */
#define ICC_SRE_DIB        (1 << 2)   /* 禁用 IRQ 旁路 */
#define ICC_SRE_EL3_EN     (1 << 3)   /* EL3 使能（保留，EL2 不用） */

/* ICC_CTLR_EL1: CPU 接口控制 */
#define ICC_CTLR_EL1       S3_0_C12_C12_4

/* ICC_PMR_EL1: 中断优先级掩码（低于此优先级的中断被屏蔽） */
#define ICC_PMR_EL1        S3_0_C4_C6_0

/* ICC_IAR1_EL1: 中断应答寄存器（读这个来获取当前中断号） */
#define ICC_IAR1_EL1       S3_0_C12_C12_0

/* ICC_EOIR1_EL1: 中断结束寄存器（写完表示中断处理完成） */
#define ICC_EOIR1_EL1      S3_0_C12_C12_1

/* ICC_DIR_EL1: 解除激活（EOI 模式拆分为两步时用） */
#define ICC_DIR_EL1        S3_0_C12_C11_1

/* ICC_SGI1R_EL1: 触发 SGI 中断 */
#define ICC_SGI1R_EL1      S3_0_C12_C11_5

/* ICC_IGRPEN1_EL1: Group 1 中断使能 */
#define ICC_IGRPEN1_EL1    S3_0_C12_C12_7

/* ICC_SRE_EL2: 系统寄存器使能（EL2）
 * 必须先设置 SRE=1，才能通过系统寄存器访问 ICC 接口
 * 否则 GIC 处于 MMIO 兼容模式（GICv2 风格）
 */
#define ICC_SRE_EL2        S3_4_C12_C9_5
#define ICC_SRE_SRE        (1 << 0)   /* 系统寄存器使能 */
#define ICC_SRE_DFB        (1 << 1)   /* 禁用 FIQ 旁路 */
#define ICC_SRE_DIB        (1 << 2)   /* 禁用 IRQ 旁路 */
#define ICC_SRE_Enable     (1 << 3)   /* 使能（与 SRE 类似，某些实现用） */

/* 特殊中断号 */
#define GIC_SPURIOUS_IRQ   1023  /* 虚假中断（没有真正的中断） */

/* PPI 中断号（每个 CPU 私有的） */
#define PPI_VIRT_TIMER      27   /* 虚拟定时器 (EL1/EL0) */
#define PPI_PHYS_TIMER      30   /* 物理定时器 (EL1/EL0) */
#define PPI_HYP_TIMER       26   /* Hypervisor 定时器 (EL2) - GICv2 命名 */
#define PPI_NS_PHYS_TIMER   30   /* 非安全物理定时器 */
#define PPI_HYP_VIRT_TIMER  26   /* EL2 虚拟定时器 (GICv3 里叫 EL2 Physical Timer) */
#define PPI_EL2_PHYS_TIMER  26   /* EL2 物理定时器 (PPI #26) */

/* 函数声明 */
void gic_init(void);
void gic_enable_irq(unsigned int irq);
void gic_disable_irq(unsigned int irq);
void gic_set_priority(unsigned int irq, unsigned int priority);
unsigned int gic_ack_irq(void);
void gic_eoi_irq(unsigned int irq);
void gic_send_sgi(unsigned int cpu_id, unsigned int sgi_id);

/* 获取当前 CPU 的 GICR 基地址 */
uint64_t gic_get_redist_base(int cpu_id);
