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

struct page_pool {
    paddr_t start_addr;
    uint64_t page_nr;
    uint8_t *memap;
    uint64_t last_page_id;
};

struct page_pool *get_virt_page_pool(void);
