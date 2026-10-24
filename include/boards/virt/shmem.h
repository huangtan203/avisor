// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - 跨 VM 共享内存（Phase 10）
 *
 * 接口定义：
 *   shm_create(key, size) -> shmid
 *   shm_attach(shmid, ipa) -> 0/-1
 *   shm_detach(shmid) -> 0/-1
 *   shm_notify(shmid) -> 0/-1
 *
 * 通知使用的虚拟中断号：SHM_NOTIFY_VIRQ = 60
 */

#pragma once

#include "common/types.h"

#define SHM_NOTIFY_VIRQ 60

void shmem_init(void);
int shm_create(uint32_t key, uint32_t size);
int shm_attach(int shmid, uint64_t ipa);
int shm_detach(int shmid);
int shm_notify(int shmid);

/*
 * Phase 34: 清除指定 VM 对全部共享内存区的 attach（源 VM 回收用）。
 *
 * 遍历 shm_table，把 attach[i].vm == vm 的记录全部清除：
 *   - unmap_stage2_page(vm, ipa, 0)（不释放共享物理页）
 *   - 清 attach 槽位 + refcount--
 *
 * 返回清除的 attach 数量。
 */
int shm_detach_vm(struct task_struct *vm);
