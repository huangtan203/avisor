// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - 跨 VM 共享内存（Phase 10）
 *
 * 实现类似 POSIX shm 的机制：
 *   - HVC_SHM_CREATE(key, size) : 创建/获取共享内存区，返回 shmid
 *   - HVC_SHM_ATTACH(shmid, ipa) : 将共享内存映射到当前 VM 的指定 IPA
 *   - HVC_SHM_DETACH(shmid)      : 取消映射
 *   - HVC_SHM_NOTIFY(shmid)      : 给所有 attach 的对端 VM 注入虚拟中断
 *
 * 设计要点：
 *   - 共享内存由 Hypervisor 管理物理页
 *   - 每个 shmid 对应一个物理页（4KB），简化实现
 *   - 最多支持 8 个共享内存区
 *   - 通过 key 查找（类似 System V IPC）
 *   - Notify 机制：向所有 attach 了该 shm 的其他 VM 注入虚拟 SPI 60
 */

#include "common/sched.h"
#include "common/mm.h"
#include "common/debug.h"
#include "common/task.h"
#include "common/utils.h"
#include "arch/aarch64/mmu.h"
#include "boards/virt/virt_mm.h"
#include "boards/virt/vgic.h"
#include "boards/virt/virt_board.h"

#define SHM_MAX_COUNT       8       /* 最多 8 个共享内存区 */
#define SHM_MAX_ATTACH      4       /* 每个 shm 最多 attach 4 个 VM */
#define SHM_NOTIFY_VIRQ     60      /* shm notify 使用的虚拟 SPI 号 */

struct shm_entry {
    int used;           /* 是否使用 */
    uint32_t key;       /* 全局唯一 key */
    paddr_t page_pa;    /* 物理页地址 */
    int refcount;       /* 引用计数（attach 数量） */

    /* attach 列表：每个 attach 记录 VM 和映射的 IPA */
    struct {
        struct task_struct *vm;  /* NULL = 空槽 */
        paddr_t ipa;
    } attach[SHM_MAX_ATTACH];
};

static struct shm_entry shm_table[SHM_MAX_COUNT];

/* 初始化共享内存子系统 */
void shmem_init(void)
{
    for (int i = 0; i < SHM_MAX_COUNT; i++) {
        shm_table[i].used = 0;
        for (int j = 0; j < SHM_MAX_ATTACH; j++) {
            shm_table[i].attach[j].vm = NULL;
            shm_table[i].attach[j].ipa = 0;
        }
    }
    INFO("shmem: initialized (max %d shms, %d attaches each)",
         SHM_MAX_COUNT, SHM_MAX_ATTACH);
}

/*
 * shm_create: 创建或获取共享内存区
 *   key: 标识符（相同 key 返回同一个 shmid）
 *   size: 大小（字节，简化：当前固定 4KB，忽略 size）
 * 返回 shmid (>=0)，失败返回 -1
 */
int shm_create(uint32_t key, uint32_t size)
{
    (void)size;  /* 简化：固定 4KB */

    /* 1. 先查找是否已存在 */
    for (int i = 0; i < SHM_MAX_COUNT; i++) {
        if (shm_table[i].used && shm_table[i].key == key) {
            INFO("shmem: create key=0x%x -> existing shmid=%d", key, i);
            return i;
        }
    }

    /* 2. 找一个空槽 */
    int shmid = -1;
    for (int i = 0; i < SHM_MAX_COUNT; i++) {
        if (!shm_table[i].used) {
            shmid = i;
            break;
        }
    }
    if (shmid < 0) {
        WARN("shmem: no free slot for key=0x%x", key);
        return -1;
    }

    /* 3. 分配物理页 */
    paddr_t page;
    {
        void *va = allocate_page();
        if (!va) {
            WARN("shmem: out of memory for key=0x%x", key);
            return -1;
        }
        page = TO_PADDR((uint64_t)va);
        memset(va, 0, PAGE_SIZE);
    }

    /* 4. 初始化 shm entry */
    shm_table[shmid].used = 1;
    shm_table[shmid].key = key;
    shm_table[shmid].page_pa = page;
    shm_table[shmid].refcount = 0;

    INFO("shmem: created key=0x%x shmid=%d pa=0x%lx",
         key, shmid, (unsigned long)page);
    return shmid;
}

/*
 * shm_attach: 将共享内存映射到当前 VM 的指定 IPA
 *   shmid: 共享内存 ID
 *   ipa: 映射到的 Guest IPA（4KB 对齐）
 * 返回 0 成功，-1 失败
 */
int shm_attach(int shmid, paddr_t ipa)
{
    if (shmid < 0 || shmid >= SHM_MAX_COUNT || !shm_table[shmid].used)
        return -1;

    struct shm_entry *shm = &shm_table[shmid];

    /* 检查当前 VM 是否已 attach */
    for (int i = 0; i < SHM_MAX_ATTACH; i++) {
        if (shm->attach[i].vm == current) {
            WARN("shmem: vm%lu already attached to shmid=%d",
                 current->vmid, shmid);
            return -1;
        }
    }

    /* 找空槽 */
    int slot = -1;
    for (int i = 0; i < SHM_MAX_ATTACH; i++) {
        if (shm->attach[i].vm == NULL) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        WARN("shmem: shmid=%d no free attach slot", shmid);
        return -1;
    }

    /* 检查 IPA 是否已映射（避免覆盖）
     * 用 ipa_to_pa 精确检查（page 级） */
    paddr_t dummy_pa;
    if (ipa_to_pa(current, ipa & PAGE_MASK, &dummy_pa) == 0) {
        WARN("shmem: ipa 0x%lx already mapped", (unsigned long)ipa);
        return -1;
    }

    /* 映射到 Stage-2 页表（共享同一个物理页） */
    map_stage2_page(current, ipa & PAGE_MASK, shm->page_pa,
                    MMU_STAGE2_PAGE_FLAGS);

    /* 记录 attach 信息 */
    shm->attach[slot].vm = current;
    shm->attach[slot].ipa = ipa & PAGE_MASK;
    shm->refcount++;

    INFO("shmem: vm%lu attach shmid=%d ipa=0x%lx (refcount=%d)",
         current->vmid, shmid, (unsigned long)(ipa & PAGE_MASK),
         shm->refcount);
    return 0;
}

/*
 * shm_detach: 取消共享内存映射
 *   shmid: 共享内存 ID
 * 返回 0 成功，-1 失败
 */
int shm_detach(int shmid)
{
    if (shmid < 0 || shmid >= SHM_MAX_COUNT || !shm_table[shmid].used)
        return -1;

    struct shm_entry *shm = &shm_table[shmid];

    /* 找到当前 VM 的 attach slot */
    int slot = -1;
    for (int i = 0; i < SHM_MAX_ATTACH; i++) {
        if (shm->attach[i].vm == current) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        WARN("shmem: vm%lu not attached to shmid=%d",
             current->vmid, shmid);
        return -1;
    }

    paddr_t ipa = shm->attach[slot].ipa;

    /*
     * Phase 20: 用通用 unmap_stage2_page() 清 Stage-2 映射
     *
     * 共享内存不释放物理页（free_page=0），物理页由 shm 子系统管理。
     * 自动获得页表自底向上回收能力。
     */
    unmap_stage2_page(current, ipa, 0);

    /* 清除 attach 记录 */
    shm->attach[slot].vm = NULL;
    shm->attach[slot].ipa = 0;
    shm->refcount--;

    /*
     * 注意：detach 只减引用计数，不自动销毁共享内存。
     *
     * 原因：在两个 VM 启动时间不一致的场景下，如果先 detach 的 VM
     * 触发销毁，后 attach 的 VM 就会拿到一个全新的 shm（key 相同
     * 但物理页不同），导致数据不互通。
     *
     * 改为全局持久化（直到系统重启），简化生命周期管理。
     * 8 个 slot 对于测试场景足够。
     */
    INFO("shmem: vm%lu detach shmid=%d (refcount=%d)",
         current->vmid, shmid, shm->refcount);

    return 0;
}

/*
 * shm_notify: 通知所有其他 attach 的 VM（注入虚拟 SPI 60）
 *   shmid: 共享内存 ID
 * 返回 0 成功，-1 失败
 *
 * 注意：只注入给"其他 VM"，不注入给自己（调用者自己知道何时更新）。
 */
int shm_notify(int shmid)
{
    if (shmid < 0 || shmid >= SHM_MAX_COUNT || !shm_table[shmid].used)
        return -1;

    struct shm_entry *shm = &shm_table[shmid];
    int notified = 0;

    for (int i = 0; i < SHM_MAX_ATTACH; i++) {
        struct task_struct *vm = shm->attach[i].vm;
        if (vm && vm != current && vm->state != TASK_ZOMBIE) {
            /* Phase 15: GICD 门控——目标 SPI (60) 被禁用则跳过注入 */
            if (!virt_vgicd_spi_enabled(vm, SHM_NOTIFY_VIRQ))
                continue;

            /*
             * Phase 19: 统一注入路径——调用 vgic_inject_virq_to_vm()
             *
             * 替代原有的直写 saved state 的老式注入方式，统一走
             * vgic_inject_virq_to_vm 的三路分派（同核运行 / 同核
             * 未运行 / 跨核 IPI），消除分散的 LR 操作逻辑。
             *
             * 唤醒逻辑保留在外层：注入成功后，若目标 VM 在等待，
             * 则唤醒它。
             */
            int ret = vgic_inject_virq_to_vm(vm, SHM_NOTIFY_VIRQ);
            if (ret == 0) {
                notified++;
                /* 如果目标 VM 在 waiting 状态，唤醒它 */
                if (vm->state == TASK_WAITING) {
                    sched_wakeup(vm);
                }
            }
        }
    }

    INFO("shmem: notify shmid=%d from vm%lu, notified=%d VMs",
         shmid, current->vmid, notified);
    return 0;
}
