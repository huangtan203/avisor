// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - GICv3 虚拟化接口 (vGIC)
 *
 * 学习要点：
 *   GICv3 的虚拟化由硬件直接支持：
 *   - Guest 用标准 ICC_*_EL1 系统寄存器访问虚拟 CPU 接口（不 trap）
 *   - Hypervisor 用 ICH_LR<n>_EL2（List Register）注入虚拟中断
 *   - Guest 的 EOI 由硬件自动处理 LR 失效
 */

#pragma once

#include "common/types.h"

struct vm_virt_state;

/* 虚拟中断号 */
#define VIRQ_VTIMER     27  /* 虚拟定时器 PPI */
#define VIRQ_VIRTIO_NET 48  /* virtio-net SPI (用户自定义，>32) */
#define VIRQ_IVSHMEM    49  /* ivshmem doorbell SPI */
#define VIRQ_PCI_EDU    50  /* PCI 直通 edu INTx 转发的虚拟 SPI */

void vgic_init(void);
void vgic_inject_virq_hw(uint32_t vintid, uint32_t pintid);

/*
 * 向当前 Guest 注入一个 HW 映射的虚拟中断（PCI INTx 直通用，固定 LR2）
 *
 * 与 vgic_inject_virq_hw 语义一致：ICH_LR_HW=1，pINTID 放 bits[41:32]。
 * Guest 写虚拟 EOIR 时硬件自动 deactivate 对应物理中断（HW 联动），
 * 这是电平触发 INTx 转发的关键——避免物理中断一直 active 导致重触发风暴。
 * 固定使用 LR2（LR0=vtimer，LR1=软件注入）。
 */
void vgic_inject_virq_hw_eoi(uint32_t vintid, uint32_t pintid);

uint64_t vgic_get_lr0(void);

/*
 * 纯软件注入虚拟中断（不关联物理中断，用于 virtio 等设备）
 * 返回 0 成功，-1 没有空闲 LR
 */
int vgic_inject_virq(uint32_t vintid);

/*
 * 查询指定虚拟中断是否 pending
 */
int vgic_is_virq_pending(uint32_t vintid);

/*
 * vGIC 世界切换：保存/恢复所有 LR + VMCR
 * （在 switch_to 的 leaving_vm / entering_vm 中调用）
 */
void vgic_save_state(struct vm_virt_state *state);
void vgic_restore_state(struct vm_virt_state *state);
