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
#define VIRT_GITS_BASE     0x08080000UL   /* GICv3 Interrupt Translation Service (ITS) */
#define VIRT_GITS_SIZE     0x00020000UL   /* ITS 寄存器空间大小 (128KB) */
#define VIRT_UART_BASE     0x09000000UL   /* PL011 UART */
#define VIRT_RTC_BASE      0x09010000UL   /* PL031 RTC */
#define VIRT_GPIO_BASE     0x09030000UL   /* PL061 GPIO */

/* 主内存起始地址 */
#define VIRT_DRAM_BASE     0x40000000UL

/* MMIO 外设区域 */
#define VIRT_MMIO_BASE     0x08000000UL   /* MMIO 外设起始物理地址 */
#define VIRT_MMIO_SIZE     0x02000000UL   /* MMIO 外设区域大小 (32MB) */

/*
 * PCIe / PCI 直通相关布局（QEMU virt, highmem=off）
 *
 * 学习要点：
 *   highmem=off 时，QEMU 把 PCI 相关的所有地址都压在 4GB 以下，
 *   这样它们可以被 Stage 2 的 2^38 IPA 空间直接映射（等值映射）。
 *     - ECAM：PCIe 增强配置访问机制，用于读/写设备配置空间
 *     - MMIO 窗口：设备 BAR 映射的目标地址范围
 */
#define VIRT_PCIE_ECAM_BASE  0x3f000000UL   /* ECAM 配置空间基址 */
#define VIRT_PCIE_ECAM_SIZE  0x01000000UL   /* ECAM 大小 (16MB) */
#define VIRT_PCIE_MMIO_BASE  0x10000000UL   /* PCI MMIO 窗口基址 */
#define VIRT_PCIE_MMIO_SIZE  0x2eff0000UL   /* PCI MMIO 窗口大小 (752MB) */

/*
 * edu 教育设备（QEMU -device edu）直通参数
 *
 *   vendor/device = 0x1234 / 0x11e8（QEMU 内置 edu 设备的固定 ID）
 *   BAR0 = 1MB MMIO，编程到 PCI MMIO 窗口起始地址 0x10000000
 *   INTx 中断号不能硬编码：QEMU virt 的 gpex 主桥用标准 PCI swizzle
 *   （host_pin = (device_pin + slot) & 3）把设备 INTx 旋到主桥的
 *   INTA/B/C/D 线，再分别接到 GIC SPI 3/4/5/6。edu 通常枚举到
 *   slot 2，其 INTA 旋到主桥 INTC -> SPI 5 -> GIC 全局中断号 37。
 *   因此中断号必须在枚举后动态计算（见 pci.c 的 pci_edu_intx_irq）。
 */
#define PCI_EDU_VENDOR       0x1234UL
#define PCI_EDU_DEVICE       0x11e8UL
#define PCI_EDU_BAR0_IPA     0x10000000UL   /* 编程到 BAR0 的地址 */
#define PCI_EDU_BAR0_SIZE    0x00100000UL   /* edu BAR0 大小 (1MB) */

/* virt 主桥 INTA 对应的 GIC SPI 号（INTA/B/C/D = SPI 3/4/5/6） */
#define VIRT_PCIE_INTX_SPI_BASE  3

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
#ifdef __ASSEMBLER__
#define UART_BASE          (VA_START + VIRT_UART_BASE)
#define GICD_BASE          (VA_START + VIRT_GICD_BASE)
#define GICR_BASE          (VA_START + VIRT_GICR_BASE)
#else
extern unsigned long virt_uart_base;
extern unsigned long virt_gicd_base;
extern unsigned long virt_gicr_base;

#define UART_BASE          (virt_uart_base)
#define GICD_BASE          (virt_gicd_base)
#define GICR_BASE          (virt_gicr_base)
#endif
#define GITS_BASE          (VA_START + VIRT_GITS_BASE)
