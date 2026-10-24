// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - ivshmem 风格共享内存设备 (Phase 4)
 *
 * 模拟 QEMU ivshmem 设备 (Inter-VM Shared Memory)：
 *   - 一块物理内存被映射到多个 VM 的 IPA 空间
 *   - 每个 VM 可以通过 doorbell 寄存器触发对方的中断
 *   - 用于 VM 间高速通信
 *
 * 简化版寄存器布局 (MMIO 4KB):
 *   0x00: Revision ID / INTR mask (RO) - 读 0x01000000
 *   0x04: Doorbell (WO) - 写低16位 = 对端 VM 索引 + 中断向量
 *         简化：写 0 = 触发 VM1 中断, 写 1 = 触发 VM2 中断
 *   0x08: IVPosition (RO) - 本 VM 的 peer ID (0 或 1)
 *
 * 共享内存区域（单独的物理地址空间）：
 *   从 0x0B000000 开始，大小 1MB
 *   映射到所有 VM 的相同 IPA 地址
 */

#pragma once

#include "common/types.h"
#include "common/sched.h"
#include <stddef.h>

/* ivshmem 控制寄存器大小 */
#define IVSHMEM_REG_SIZE   0x1000  /* 4KB 控制寄存器 */

/* 寄存器偏移 */
#define IVSHMEM_REVISION   0x00  /* RO: revision + intr mask */
#define IVSHMEM_DOORBELL   0x04  /* WO: doorbell */
#define IVSHMEM_IVPOSITION 0x08  /* RO: peer ID */

/* ivshmem 设备状态 */
struct ivshmem_dev {
    struct task_struct *vm;    /* 所属 VM */
    int peer_id;               /* 本端 peer ID (0 或 1) */
    uint64_t shmem_pa;         /* 共享内存物理地址 */
    size_t   shmem_size;       /* 共享内存大小 */
    uint64_t shmem_ipa;        /* 共享内存在 guest IPA 中的地址 */
};

/* 全局 ivshmem 设备数组（两个 VM 各一个） */
#define IVSHMEM_MAX_PEERS 2
extern struct ivshmem_dev *ivshmem_peers[IVSHMEM_MAX_PEERS];

/*
 * 初始化 ivshmem 设备
 *   dev: 设备结构体
 *   vm:  所属 VM
 *   peer_id: 本端 ID (0 或 1)
 *   shmem_pa: 共享内存物理基地址
 *   shmem_size: 共享内存大小
 *   reg_ipa:  控制寄存器在 guest IPA 中的地址
 *   shmem_ipa: 共享内存在 guest IPA 中的地址
 */
void ivshmem_init(struct ivshmem_dev *dev,
                  struct task_struct *vm,
                  int peer_id,
                  uint64_t shmem_pa,
                  size_t shmem_size,
                  uint64_t reg_ipa,
                  uint64_t shmem_ipa);

/*
 * 重新设置 peer_id（vmid 修正后调用）
 */
void ivshmem_set_peer_id(struct ivshmem_dev *dev, int new_peer_id);

/*
 * Phase 34: 注销 ivshmem 设备（源 VM 回收用）。
 *
 * 从全局 peer 数组移除，并 unmap 共享内存到该 VM 的 Stage-2 映射
 * （不释放共享物理页，共享页由 ivshmem 全局持有）。
 */
void ivshmem_unregister(struct ivshmem_dev *dev);

/*
 * MMIO 读操作
 */
uint32_t ivshmem_mmio_read(struct ivshmem_dev *dev, uint32_t offset);

/*
 * MMIO 写操作
 */
void ivshmem_mmio_write(struct ivshmem_dev *dev, uint32_t offset, uint32_t val);
