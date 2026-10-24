// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - ivshmem 风格共享内存设备实现 (Phase 4)
 *
 * 两个 VM 共享同一块物理内存，通过 doorbell 寄存器互相发中断。
 */

#include "boards/virt/ivshmem.h"
#include "boards/virt/vgic.h"
#include "boards/virt/virt_board.h"
#include "arch/aarch64/sysregs.h"
#include "arch/aarch64/mmu.h"
#include "common/mm.h"
#include "common/debug.h"
#include "common/utils.h"

/* 全局 peer 数组 */
struct ivshmem_dev *ivshmem_peers[IVSHMEM_MAX_PEERS] = { NULL, NULL };

/*
 * 把共享内存映射到 guest 的 IPA 空间
 * （阶段 2 映射，两个 VM 映射到同一块物理内存）
 */
static void map_shared_memory(struct ivshmem_dev *dev,
                              uint64_t shmem_ipa,
                              uint64_t shmem_pa,
                              size_t size)
{
    for (size_t offset = 0; offset < size; offset += PAGE_SIZE) {
        map_stage2_page(dev->vm, shmem_ipa + offset,
                        shmem_pa + offset,
                        MMU_STAGE2_PAGE_FLAGS);
    }
}

/*
 * 初始化 ivshmem 设备
 */
void ivshmem_init(struct ivshmem_dev *dev,
                  struct task_struct *vm,
                  int peer_id,
                  uint64_t shmem_pa,
                  size_t shmem_size,
                  uint64_t reg_ipa,
                  uint64_t shmem_ipa)
{
    memzero(dev, sizeof(*dev));
    dev->vm = vm;
    dev->peer_id = peer_id;
    dev->shmem_pa = shmem_pa;
    dev->shmem_size = shmem_size;
    dev->shmem_ipa = shmem_ipa;

    /* 注册到全局 peer 数组 */
    if (peer_id >= 0 && peer_id < IVSHMEM_MAX_PEERS)
        ivshmem_peers[peer_id] = dev;

    /* 把共享内存映射到 guest IPA 空间 */
    map_shared_memory(dev, shmem_ipa, shmem_pa, shmem_size);

    (void)reg_ipa;  /* 寄存器区域通过 translation fault 模拟，不需要映射 */

    INFO("ivshmem: peer %d initialized (shmem=0x%lx, size=%dKB)",
         peer_id, (unsigned long)shmem_ipa, (int)(shmem_size / 1024));
}

/*
 * 重新设置 peer_id（用于 vmid 修正后更新 ivshmem 身份）
 *
 * 背景：create_task_on_cpu 里调用 initialize 时，vmid 还是自动分配的
 * 临时值（next_vmid 递增），调用方随后会覆盖为正确的 vmid。
 * 但 ivshmem 的 peer_id 是初始化时根据 vmid 计算的，所以需要
 * 在 vmid 修正后同步更新 peer_id。
 */
void ivshmem_set_peer_id(struct ivshmem_dev *dev, int new_peer_id)
{
    int old_peer_id = dev->peer_id;

    /* 从旧位置移除 */
    if (old_peer_id >= 0 && old_peer_id < IVSHMEM_MAX_PEERS &&
        ivshmem_peers[old_peer_id] == dev) {
        ivshmem_peers[old_peer_id] = NULL;
    }

    /* 设置新的 peer_id 并注册到新位置 */
    dev->peer_id = new_peer_id;
    if (new_peer_id >= 0 && new_peer_id < IVSHMEM_MAX_PEERS) {
        ivshmem_peers[new_peer_id] = dev;
    }

    INFO("ivshmem: peer reparented %d -> %d (vm=%p)",
         old_peer_id, new_peer_id, (void *)dev->vm);
}

/*
 * Phase 34: 注销 ivshmem 设备。
 *
 * 从全局 peer 数组移除，并 unmap 共享内存到该 VM 的 Stage-2 映射。
 * 注意：不释放共享物理页——共享内存页由 virt_board 的
 * ivshmem_shmem_pa 全局持有，多个 VM 共享同一块物理内存。
 */
void ivshmem_unregister(struct ivshmem_dev *dev)
{
    if (!dev)
        return;

    int peer_id = dev->peer_id;
    if (peer_id >= 0 && peer_id < IVSHMEM_MAX_PEERS &&
        ivshmem_peers[peer_id] == dev) {
        ivshmem_peers[peer_id] = NULL;
    }

    /* unmap 共享内存映射（不释放物理页） */
    if (dev->vm && dev->shmem_size > 0) {
        for (size_t offset = 0; offset < dev->shmem_size;
             offset += PAGE_SIZE) {
            unmap_stage2_page(dev->vm,
                              dev->shmem_ipa + offset, 0);
        }
    }

    dev->vm = NULL;
    dev->peer_id = -1;

    INFO("ivshmem: peer %d unregistered", peer_id);
}

/*
 * MMIO 读操作
 */
uint32_t ivshmem_mmio_read(struct ivshmem_dev *dev, uint32_t offset)
{
    switch (offset) {
    case IVSHMEM_REVISION:
        /* Revision 1, INTR mask 0 (legacy 格式) */
        return 0x01000000;

    case IVSHMEM_IVPOSITION:
        return (uint32_t)dev->peer_id;

    default:
        return 0;
    }
}

/*
 * MMIO 写操作
 *
 * Doorbell 寄存器格式 (简化):
 *   低 16 位 = 目标 peer ID
 *   写此寄存器会触发目标 VM 的虚拟中断
 */
void ivshmem_mmio_write(struct ivshmem_dev *dev, uint32_t offset, uint32_t val)
{
    if (offset != IVSHMEM_DOORBELL)
        return;

    int target_peer = val & 0xFFFF;

    if (target_peer < 0 || target_peer >= IVSHMEM_MAX_PEERS)
        return;

    struct ivshmem_dev *target = ivshmem_peers[target_peer];
    if (!target || !target->vm)
        return;

    /*
     * Phase 19: 统一注入路径——调用 vgic_inject_virq_to_vm()
     *
     * 该函数内部已处理三种情况：
     *   1. 目标就是 current → 直接写硬件 LR（raw 路径，无门控）
     *   2. 目标在本核但未运行 → 写 saved state
     *   3. 目标在另一物理核 → mailbox + 物理 SGI 跨核注入
     *
     * ivshmem doorbell 是 SPI，需要 GICD enable 门控。
     * 注意：vgic_inject_virq_to_vm 本身不带门控（用于 vSGI/LPI 等），
     * 所以这里在外层先做门控判断。
     */
    if (!virt_vgicd_spi_enabled(target->vm, VIRQ_IVSHMEM))
        return;

    vgic_inject_virq_to_vm(target->vm, VIRQ_IVSHMEM);
}
