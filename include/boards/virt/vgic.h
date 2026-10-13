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

struct task_struct;
struct vm_virt_state;

/* 虚拟中断号 */
#define VIRQ_VTIMER     27  /* 虚拟定时器 PPI */
#define VIRQ_VIRTIO_NET 48  /* virtio-net SPI (用户自定义，>32) */
#define VIRQ_IVSHMEM    49  /* ivshmem doorbell SPI */
#define VIRQ_PCI_EDU    50  /* PCI 直通 edu INTx 转发的虚拟 SPI */

/*
 * Phase 15: 虚拟 GIC Distributor (GICD) 状态
 *
 * GICv3 的 CPU 接口 (ICC_*_EL1) 由硬件自动虚拟化（直通），但
 * Distributor (GICD) 是 MMIO 设备，Guest 访问会 trap 到 EL2，
 * 必须软件模拟（trap-and-emulate）。这里保存每个 VM 的 GICD 状态，
 * 核心是 SPI 使能位图，用于在注入虚拟 SPI 时做 enable 门控。
 */
struct vgic_dist {
    uint32_t ctlr;             /* GICD_CTLR：全局使能 + 亲和路由 */
    uint32_t spi_enable[32];   /* GICD_ISENABLERn：INTID 0..1023 使能位图 */
};

/*
 * Phase 16: 虚拟 GIC Redistributor (GICR) 状态
 *
 * GICv3 每个 PE 有一个 Redistributor，管理 SGI/PPI 私有中断，与 GICD
 * （SPI 全局中断）对称。这里保存 SGI+PPI 使能位图，用于在注入虚拟 PPI
 * 时做 enable 门控。
 *
 * 简化：per-VM 一份（真实硬件是 per-PE 一份）。本项目只有 vtimer 一个
 * 真实 PPI，它走 HW 直通（vgic_inject_virq_hw），不经过此软件位图；
 * 软件 PPI 门控主要用于 Guest 通过 GICR_ICENABLER0 显式禁用后的注入丢弃。
 */
struct vgic_rdist {
    uint32_t ctlr;             /* GICR_CTLR */
    uint32_t ppi_enable;       /* GICR_ISENABLER0：INTID 0..31 使能位图 */
};

void vgic_init(void);
void vgic_inject_virq_hw(uint32_t vintid, uint32_t pintid);

/*
 * Phase 15: 虚拟 GICD 寄存器读写模拟（trap-and-emulate）
 *   offset 为相对 GICD 基址 (0x08000000) 的偏移。
 *   由 virt_board 的 MMIO 分派调用。
 */
void vgicd_init(struct vgic_dist *d);
uint32_t vgicd_read(struct vgic_dist *d, uint32_t offset);
void vgicd_write(struct vgic_dist *d, uint32_t offset, uint32_t val);

/*
 * Phase 16: 虚拟 GICR 寄存器读写模拟（trap-and-emulate）
 *   offset 为相对 GICR 基址 (0x080A0000) 的偏移。
 *   由 virt_board 的 MMIO 分派调用。
 */
void vgicr_init(struct vgic_rdist *r);
uint32_t vgicr_read(struct vgic_rdist *r, uint32_t offset);
void vgicr_write(struct vgic_rdist *r, uint32_t offset, uint32_t val);

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
 * Phase 10: 向指定 VM 注入虚拟中断（不要求目标 VM 正在运行）
 *   - 如果目标 VM 是 current：直接写 LR
 *   - 否则：写入目标 VM 的 saved gic_lr[]，下次调度时自动生效
 *   - Phase 13：若目标 vCPU 在别的物理核上运行，额外发 IPI 立即生效
 * 返回 0 成功，-1 没有空闲 LR
 */
int vgic_inject_virq_to_vm(struct task_struct *vm, uint32_t vintid);

/*
 * Phase 13: 处理跨核 vGIC 维护 IPI（SGI_VGIC_UPDATE）
 * 由 smp_ipi_handle() 在目标核的中断上下文中调用。
 */
void vgic_ipi_update_handle(void);

/*
 * vGIC 世界切换：保存/恢复所有 LR + VMCR
 * （在 switch_to 的 leaving_vm / entering_vm 中调用）
 */
void vgic_save_state(struct vm_virt_state *state);
void vgic_restore_state(struct vm_virt_state *state);

/*
 * Phase 10: 中断性能统计
 */
void irq_stats_record_lpi(uint64_t cycles);
void irq_stats_dump(void);
