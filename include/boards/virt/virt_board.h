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
