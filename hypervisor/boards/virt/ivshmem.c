// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - ivshmem 风格共享内存设备实现 (Phase 4)
 *
 * 两个 VM 共享同一块物理内存，通过 doorbell 寄存器互相发中断。
 */

#include "boards/virt/ivshmem.h"
#include "boards/virt/vgic.h"
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
     * 向目标 VM 注入虚拟中断
     *
     * - 如果目标 VM 当前正在运行 → 直接写硬件 LR
     * - 否则 → 修改目标 VM 保存的 virt 状态中的 LR
     *   下次进入该 VM 时（vgic_restore_state）会自动加载到硬件
     */
    if (target->vm == current) {
        /* 目标就是当前 VM，直接注入硬件 LR */
        vgic_inject_virq(VIRQ_IVSHMEM);
    } else {
        /* 目标不是当前 VM，修改其保存的状态 */
        struct vm_virt_state *virt = &target->vm->virt;
        int injected = 0;

        /* 找一个空闲 LR（LR1；LR0=vtimer，LR2=PCI INTx） */
        for (int i = 1; i < NR_VGIC_LRS - 1; i++) {
            if ((virt->gic_lr[i] & ICH_LR_STATE_MASK) == 0) {
                virt->gic_lr[i] =
                    ((uint64_t)VIRQ_IVSHMEM & 0xffffffffULL)
                    | ICH_LR_GROUP1
                    | ICH_LR_PENDING;
                injected = 1;
                break;
            }
        }

        if (!injected) {
            WARN("ivshmem: no free LR for peer %d", target_peer);
        }
    }
}
