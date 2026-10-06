// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - QEMU virt 平台板级初始化
 *
 * 学习要点：
 *   每个平台需要提供自己的 page pool（物理页面分配池），
 *   因为不同平台的内存布局不同。
 *
 *   这里的 page pool 是最简单的位图分配器：
 *   - 一个字节数组，每个字节代表一个页面（1=已分配, 0=空闲）
 *   - 线性扫描找第一个空闲页
 *   - 分配/释放都是 O(n)，简单但效率不高
 *   对于教学用途完全够用。
 */

#include "boards/virt/virt_board.h"
#include "boards/virt/virt_mm.h"
#include "common/mm.h"
#include "common/types.h"

/*
 * 页面分配位图
 * PAGING_PAGES 个页面，每个页面 1 字节状态
 * （这里用字节而不是位，是为了代码简单）
 */
static uint8_t virt_page_memap[PAGING_PAGES] = { 0 };

static struct page_pool virt_page_pool = {
    .start_addr = LOW_MEMORY,
    .page_nr = PAGING_PAGES,
    .memap = virt_page_memap,
    .last_page_id = 0,
};

struct page_pool *get_virt_page_pool(void)
{
    return &virt_page_pool;
}
