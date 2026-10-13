// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - GICv3 ITS (Interrupt Translation Service)
 *
 * Phase 8: MSI/MSI-X 中断直通支持
 *
 * ITS 是 GICv3 处理 MSI 的硬件模块，工作流程：
 *
 *   1. Hypervisor 通过命令队列（MAPD/MAPC/MAPTI）建立映射：
 *        DeviceID -> ITT 表（EventID -> LPI + Collection）
 *        CollectionID -> 目标 Redistributor（procnum）
 *   2. 设备发起 MSI：向 GITS_TRANSLATER 地址写 EventID
 *      （PCIe 事务携带 DeviceID = requester_id）
 *   3. ITS 查 DTE -> ITT -> CTE，翻译出 (LPI, 目标 CPU)
 *   4. 目标 Redistributor 置 LPI pending 位 -> CPU 中断
 *   5. CPU 读 ICC_IAR1 拿到 LPI 号（>= 8192）处理
 *
 * 关键约束（依据 QEMU 6.2 hw/intc/arm_gicv3_its.c 逐条验证）：
 *   - LPI INTID 必须 >= 8192（GICV3_LPI_INTID_START）
 *   - LPI 无 active 状态：IAR 读即清 pending，EOIR 即结束
 *   - GITS_BASERn 的 TYPE/ENTRYSIZE 位只读（复位值：
 *     BASER0=Device 表, BASER1=Collection 表, entry 8 字节）
 *   - BASER 的 PAGESIZE 复位为 64KB，必须显式写 0（4KB）
 *   - PENDBASER 物理地址字段是 [51:16]：pending 表必须 64KB 对齐
 *   - Redistributor 必须置 GICR_CTLR.EnableLPIs 才会投递 LPI
 *   - CWRITER/CREADR 的 OFFSET 字段在 [19:5]（命令 32 字节对齐，
 *     直接写字节偏移即可）
 */

#pragma once

#include "common/types.h"

/*
 * ITS 初始化（在 gic_init 之后、pci_init 之前调用）：
 *   - 分配并安装 CPU0 的 LPI 配置表/挂起表
 *   - 分配命令队列 + 设备表 + 集合表并编程 BASER/CBASER
 *   - 使能 ITS（GITS_CTLR）
 */
void its_init(void);

/*
 * 为指定 PCI 设备配置 MSI，返回分配的 LPI 基址（0 = 失败）：
 *   - MAPC: collection 0 -> procnum target_cpu
 *   - MAPD: device_id -> ITT
 *   - MAPTI: eventid 0..num_vectors-1 -> LPI，同时配置 LPI 属性表
 *
 * device_id 按 QEMU 的 requester_id 编码：(bus << 8) | (dev << 3) | func
 */
unsigned int its_setup_pci_msi(unsigned int bus, unsigned int dev,
                               unsigned int func, unsigned int num_vectors,
                               unsigned int target_cpu);

/* MSI 消息地址：GITS_TRANSLATER 的物理地址（写入 PCI MSI Capability） */
uint64_t its_get_msi_address(void);

/* MSI 消息数据：EventID（写入 PCI MSI Capability） */
uint32_t its_get_msi_data(unsigned int event_id);
