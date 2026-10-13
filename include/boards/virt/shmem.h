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
