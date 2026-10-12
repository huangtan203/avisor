// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - PCI/PCIe 直通 (Phase 7)
 *
 * 把 QEMU virt 上的 edu 教育设备（-device edu）直接透传给 Guest：
 *   - ECAM 配置空间直通（Guest 自行枚举）
 *   - BAR0 MMIO 直通（Guest 直接驱动寄存器）
 *   - INTx 传统线中断转发（物理 SPI -> 虚拟 SPI）
 */

#pragma once

struct task_struct;

/* 初始化 PCI：枚举 edu、编程 BAR0、使能 memory/bus master、路由并使能 INTx */
void pci_init(void);

/*
 * edu INTx 的物理 GIC 全局中断号。
 * 由配置空间的 interrupt pin + 设备 slot 经 PCI swizzle 在 pci_init()
 * 运行时计算填充；0 表示 edu 未找到（不转发中断）。
 */
extern unsigned int pci_edu_intx_irq;

/* 把 edu 的 ECAM + BAR0 映射进指定 VM 的 Stage 2（MMIO 直通） */
void pci_passthrough_map(struct task_struct *vm);

/* 调试：dump PCI 直通中断链路状态（HVC_PCI_DEBUG 触发） */
void pci_debug_dump(void);
