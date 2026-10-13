// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - QEMU virt 平台板级定义
 *
 * 这里定义 virt 平台的内存池和板级初始化接口。
 * 学习要点：
 *   这是"多平台抽象"的第一步——不同平台有不同的物理内存布局，
 *   但上层的内存分配器（page pool）接口是统一的。
 */

#pragma once

#include "common/mm.h"
#include "common/types.h"
#include "common/board.h"

struct page_pool {
    paddr_t start_addr;
    uint64_t page_nr;
    uint8_t *memap;
    uint64_t last_page_id;
};

struct page_pool *get_virt_page_pool(void);

/* virt 平台的 VM 设备模型（Phase 2） */
extern const struct board_ops virt_board_ops;

/* 次级 vCPU 挂接主 vCPU 的设备数据（虚拟 SMP） */
void virt_attach_secondary(struct task_struct *tsk, struct task_struct *master);

/* 更新 VM 的 ivshmem peer_id（vmid 修正后调用） */
void virt_board_set_ivshmem_peer(struct task_struct *tsk, int peer_id);

/* Phase 12: 虚拟 SGI（vSGI）支持 */
#define MAX_VMS_PER_BOARD     4   /* 板级最多支持的 VM 数 */
#define MAX_VCPUS_PER_VM      4   /* 每个 VM 最多支持的 vCPU 数 */

/* 注册 vCPU 到板级查找表（创建 vCPU 时调用） */
void virt_register_vcpu(struct task_struct *tsk, int vmid, int vcpu_id);

/* 根据 vmid + vcpu_id 查找 vCPU（返回 NULL 表示不存在） */
struct task_struct *virt_find_vcpu(int vmid, int vcpu_id);

/* 向同 VM 的指定 vCPU 注入虚拟中断 */
int virt_inject_virq_to_vcpu(int vmid, int vcpu_id, uint32_t vintid);

/* Phase 15: 查询指定 VM 的虚拟 SPI 是否在 GICD 中被使能
 * （返回 1=使能/放行，0=禁用；SGI/PPI/LPI 等非 GICD 门控范围恒返回 1）
 * 供 vgic.c 注入路径做 enable 门控。 */
int virt_vgicd_spi_enabled(struct task_struct *vm, uint32_t vintid);

/* Phase 16: 查询指定 VM 的虚拟 PPI 是否在 GICR 中被使能
 * （返回 1=使能/放行，0=禁用；仅 PPI 16-31 走此门控）
 * 供 vgic.c 注入路径做 enable 门控。 */
int virt_vgicr_ppi_enabled(struct task_struct *vm, uint32_t vintid);

/* Phase 18: 查询指定 VM 的虚拟中断优先级（8 位，数值越小优先级越高）。
 * SGI/PPI 查 GICR、SPI 查 GICD、LPI 返回默认 0xA0。
 * 供 vgic.c 注入路径把优先级写入 ICH_LR 的 Priority 字段。 */
uint8_t virt_vgic_vintid_priority(struct task_struct *vm, uint32_t vintid);
