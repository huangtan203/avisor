// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - QEMU virt 平台支持
 *
 * QEMU 'virt' 机器是一个虚拟的 ARM64 平台，使用标准 ARM 外设：
 *   - PL011 UART
 *   - GICv3 中断控制器
 *   - ARM 架构定时器（CNTP/CNTV）
 *   - 内存从 0x40000000 开始（QEMU virt 默认布局）
 *
 * 学习要点：
 *   真实硬件（树莓派、AGX Orin 等）的内存映射和外设各不相同，
 *   但 QEMU virt 使用的都是 ARM 标准外设，是学习虚拟化的理想平台。
 */

#pragma once

/*
 * 注意：这些基础地址宏必须在 include common/mm.h 之前定义，
 * 因为 virt_mm.h（被 common/mm.h 间接包含）会用到它们。
 */

/* 外设基地址（物理地址） */
#define VIRT_GICD_BASE     0x08000000UL   /* GICv3 Distributor */
#define VIRT_GICR_BASE     0x080A0000UL   /* GICv3 Redistributor (CPU0基址) */
#define VIRT_GICR_STRIDE   0x20000UL      /* 每个CPU的Redistributor间距 */
#define VIRT_UART_BASE     0x09000000UL   /* PL011 UART */
#define VIRT_RTC_BASE      0x09010000UL   /* PL031 RTC */
#define VIRT_GPIO_BASE     0x09030000UL   /* PL061 GPIO */

/* 主内存起始地址 */
#define VIRT_DRAM_BASE     0x40000000UL

/* MMIO 外设区域 */
#define VIRT_MMIO_BASE     0x08000000UL   /* MMIO 外设起始物理地址 */
#define VIRT_MMIO_SIZE     0x02000000UL   /* MMIO 外设区域大小 (32MB) */

#include "common/mm.h"

/*
 * QEMU virt 平台内存布局（简化版）
 *
 * 0x0000_0000 - 0x0800_0000  128MB  Flash (低地址区，一般不用)
 * 0x0800_0000 - 0x0801_0000  64KB   GICv3 Distributor
 * 0x0801_0000 - 0x0802_0000  64KB   GICv3 Redistributor (CPU0)
 * ...
 * 0x0900_0000 - 0x0901_0000  64KB   PL011 UART
 * 0x0A00_0000 - 0x0A01_0000  64KB   RTC
 * 0x4000_0000 - ...          DRAM 起始（主内存）
 *
 * 注意：实际布局通过设备树(DTS)描述，这里是常用的固定地址，
 *       QEMU virt 机器默认这些地址是固定的。
 */

/* 设备地址映射总大小（用于MMIO映射范围） */
#define VIRT_DEV_SIZE      0x10000000   /* 256MB 外设空间 */

/*
 * 外设基地址的虚拟地址
 * 与 raspi 平台不同，virt 平台外设不在 0x3F000000
 * 我们保持 DEVICE_BASE 宏的命名方式以兼容上层代码
 */
#define DEVICE_BASE        VIRT_DRAM_BASE  /* 兼容宏，指向主内存 */

/* 转换到虚拟地址 */
#define PBASE              (VA_START + VIRT_MMIO_BASE)
#define UART_BASE          (VA_START + VIRT_UART_BASE)
#define GICD_BASE          (VA_START + VIRT_GICD_BASE)
#define GICR_BASE          (VA_START + VIRT_GICR_BASE)
