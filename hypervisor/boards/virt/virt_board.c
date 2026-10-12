// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - QEMU virt 平台板级支持
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
 *
 *   Phase 2 新增 virt_board_ops：
 *   与 raspi 的 bcm2837_board_ops 对应，是 virt 平台的"设备模型"。
 *   Phase 2 的 virt guest 不访问 MMIO 外设（控制台走 HVC 半虚拟化，
 *   定时器走架构虚拟定时器，中断走 GICv3 虚拟接口），
 *   所以 mmio_read/mmio_write 为空，
 *   entering_vm 只负责恢复虚拟计数器偏移 CNTVOFF_EL2。
 */

#include "boards/virt/virt_board.h"
#include "boards/virt/virt_mm.h"
#include "boards/virt/vgic.h"
#include "arch/aarch64/sysregs.h"
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

/*
 * VM 创建时的初始化（create_task 中调用）
 *
 * 快照当前物理计数器作为该 VM 的虚拟计数器偏移，
 * 这样 Guest 读到的 CNTVCT 从 0 开始，
 * 符合"新机器刚开机"的直觉。
 */
static void virt_initialize(struct task_struct *tsk)
{
    tsk->mm.cntvoff = READ_SYSREG64(CNTPCT_EL0);
}

/*
 * 每次进入 VM 前调用（vm_entering_work → board_ops->entering_vm）
 *
 * 恢复该 VM 的 CNTVOFF_EL2。
 * CNTVOFF_EL2 是"每 VM"资源：物理计数器 - CNTVOFF = 虚拟计数器。
 * 多 VM 场景下，每个 VM 有自己的偏移，切换时必须恢复。
 */
static void virt_entering_vm(struct task_struct *tsk)
{
    WRITE_SYSREG64(tsk->mm.cntvoff, CNTVOFF_EL2);
    asm volatile("isb");

    /* 补注入 HV 上下文期间缓存的 vtimer 虚拟中断 */
    vgic_vtimer_flush();
}

/*
 * 每次离开 VM 时调用（vm_leaving_work → board_ops->leaving_vm）
 * Phase 2 无需额外处理（CNTVOFF 读取时机不受影响）。
 */
static void virt_leaving_vm(struct task_struct *tsk)
{
    (void)tsk;
}

const struct board_ops virt_board_ops = {
    .initialize = virt_initialize,
    .mmio_read = NULL,   /* Phase 2 无 MMIO 模拟（guest 走 HVC/系统寄存器） */
    .mmio_write = NULL,
    .entering_vm = virt_entering_vm,
    .leaving_vm = virt_leaving_vm,
    .is_irq_asserted = NULL, /* GICv3 硬件虚拟化注入，无需软件查询 */
    .is_fiq_asserted = NULL,
    .debug = NULL,
};
