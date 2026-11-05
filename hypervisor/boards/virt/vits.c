// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - GICv3 vITS（虚拟 ITS）实现
 *
 * Phase 9: vITS 虚拟化
 *
 * 设计思路（简化但完整可用）：
 *
 *   Guest 视角：标准 GICv3 ITS，支持 MAPC / MAPD / MAPTI / SYNC 命令。
 *   Guest 分配自己的 vDevID / vEventID / vINTID（虚拟 LPI），
 *   通过 GITS_TRANSLATER 写 MSI doorbell 触发中断。
 *
 *   Hypervisor 实现：
 *     1. 陷获 Guest 对 GITS 寄存器的 MMIO 访问（trap-and-emulate）
 *     2. 解析 Guest 命令队列（命令内容在 Guest GPA 空间，需 ipa_to_pa 翻译）
 *     3. 为每条 vITS 命令分配/映射物理 ITS 资源：
 *          - vDevID → 物理 DevID（直通设备复用其 requester_id）
 *          - vEventID → 物理 EventID（1:1 映射，简单直接）
 *          - vICID → 物理 ICID（collection 0 → 目标 CPU）
 *          - vINTID → 物理 LPI（从物理 LPI 池动态分配）
 *     4. 下发物理 ITS 命令（MAPC/MAPD/MAPTI）建立真正的硬件映射
 *     5. 建立 pLPI → (vmid, vINTID) 映射表
 *
 *   中断路径（MSI 到来时）：
 *     设备写 GITS_TRANSLATER → 物理 ITS 翻译 → 物理 LPI → EL2 LPI 中断
 *     → 查 pLPI → (vmid, vINTID) 表 → 向目标 VM 注入虚拟中断
 *
 * 简化约束（Phase 9 MVP）：
 *   - 单 vITS 实例（VM1 直通 edu 设备）
 *   - 单 collection（collection 0 → CPU0）
 *   - vDevID 直接 = 物理 DevID（设备唯一，Guest 知道即可）
 *   - vEventID = 物理 EventID（1:1）
 *   - vINTID 由 Guest 指定，同时记录到 pLPI→vINTID 表
 *   - ITT 表由 Hypervisor 从物理页池分配（Guest 的 ITT GPA 只保存不使用）
 *     （因为物理 ITS 需要真实的 PA 来存 ITT 条目，而 Guest 的 GPA 映射
 *      到物理内存后可被 ITS 访问 —— 实际上 QEMU 的 ITS 可以访问 GPA，
 *      因为 GPA 经过 stage-2 映射后在物理内存中。但为了安全和简单，
 *      我们用 Hypervisor 自己分配的 ITT 页来给物理 ITS 用。）
 */

#include "boards/virt/vits.h"
#include "boards/virt/its.h"
#include "boards/virt/base.h"
#include "boards/virt/gicv3.h"
#include "boards/virt/vgic.h"
#include "boards/virt/irq_domain.h"
#include "boards/virt/pci.h"
#include "common/mm.h"
#include "common/debug.h"
#include "common/utils.h"

/* ---------- 全局映射表：pLPI -> (vmid, vINTID) ---------- */

/*
 * 物理 LPI 号范围 8192..16383（共 8192 个，足够用）
 * 用数组索引 = pLPI - 8192
 * 每个条目存 vmid（高16位） + vINTID（低32位）
 * vmid == 0 表示未使用
 */
#define LPI_MAP_BASE     8192
#define LPI_MAP_MAX      8192   /* 最多 8192 个映射 */

struct lpi_map_entry {
    uint16_t vmid;     /* 0 = 空闲 */
    uint32_t vintid;   /* Guest 视角的中断号 */
};

static struct lpi_map_entry lpi_map[LPI_MAP_MAX];
static volatile unsigned int lpi_map_lock;

static void lpi_map_acquire(void)
{
    unsigned int value, failed;

    for (;;) {
        asm volatile("1: ldaxr %w0, [%2]\n"
                     "cbnz %w0, 1b\n"
                     "mov %w0, #1\n"
                     "stxr %w1, %w0, [%2]\n"
                     "cbnz %w1, 1b\n"
                     : "=&r"(value), "=&r"(failed)
                     : "r"(&lpi_map_lock)
                     : "memory");
        if (!failed)
            return;
    }
}

static void lpi_map_release(void)
{
    asm volatile("stlr wzr, [%0]" :: "r"(&lpi_map_lock) : "memory");
}

/* 下一个可分配的物理 LPI（从 9000 开始，避开 Phase 8 的 edu 8192..8207） */
static volatile unsigned int next_alloc_plpi = 9000;

static int vits_reserve_plpi(unsigned int *plpi)
{
    unsigned int observed, next, failed;

    for (;;) {
        asm volatile("ldaxr %w0, [%1]"
                     : "=&r"(observed)
                     : "r"(&next_alloc_plpi)
                     : "memory");
        if (observed >= LPI_MAP_BASE + LPI_MAP_MAX) {
            asm volatile("clrex" ::: "memory");
            return -1;
        }
        next = observed + 1;
        asm volatile("stxr %w0, %w1, [%2]"
                     : "=&r"(failed)
                     : "r"(next), "r"(&next_alloc_plpi)
                     : "memory");
        if (!failed) {
            *plpi = observed;
            return 0;
        }
    }
}

/* ---------- 全局初始化 ---------- */

void vits_global_init(void)
{
    lpi_map_lock = 0;
    for (int i = 0; i < LPI_MAP_MAX; i++) {
        lpi_map[i].vmid = 0;
        lpi_map[i].vintid = 0;
    }
    INFO("vITS: global init (LPI map size=%d, start from %u)",
         LPI_MAP_MAX, next_alloc_plpi);
}

/* ---------- per-VM 初始化 ---------- */

void vits_init_vm(struct vits_state *vits, struct task_struct *vm)
{
    memzero(vits, sizeof(*vits));
    vits->vm = vm;
    vits->cmdq_bytes = PAGE_SIZE;  /* 默认 4KB 命令队列 */

    /*
     * TYPER 复位值（只读）：
     *   - Physical=1 (bit 12): 表示支持 LPIs
     *   - DevBits=8 (bits [21:17]): 支持 256 个 DeviceID
     *   - IDBits=12 (bits [25:22]): 支持 4096 个 EventID (LPI)
     *   - CIDBits=16 (bits [13:10]? 简化：不报告，按默认)
     *
     * 参考 QEMU GITS_TYPER reset = 0x00A10E17：
     *   bit0 = 1 (ITTyPIS, IMPDEF)
     *   bit1 = 1 (ITTyVPE)
     *   bit4 = 1 (CIL)
     *   bit8..10 = CIDbits=7 (256 collections)
     *   bit12 = 1 (Physical LPI)
     *   bit17..21 = 7 (DevBits, 256 devices)
     *   bit22..25 = 11 (IDBits, 4096 LPIs)
     *
     * 简化：只报告关键的 Physical + DevBits=8 + IDBits=12
     */
    /* TYPER 是只读寄存器，Guest 读时动态构造，不存 */

    INFO("vITS: initialized for VM%u", (unsigned int)vm->vmid);
}

/* ---------- 工具函数：读 Guest 命令队列 ---------- */

static void vits_lock(struct vits_state *vits)
{
    unsigned int ok;

    asm volatile(
        "1: ldaxr w0, [%1]\n"
        "   cbnz w0, 1b\n"
        "   mov  w0, #1\n"
        "   stxr %w0, w0, [%1]\n"
        "   cbnz %w0, 1b\n"
        : "=&r"(ok)
        : "r"(&vits->lock)
        : "x0", "memory");
}

static void vits_unlock(struct vits_state *vits)
{
    asm volatile("stlr wzr, [%0]" :: "r"(&vits->lock) : "memory");
}

static void vits_invalidate_cmd_range(void *start, size_t len)
{
    uint64_t ctr;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t line_size = 4UL << ((ctr >> 16) & 0xf);
    uintptr_t p = (uintptr_t)start & ~(line_size - 1);
    uintptr_t end = (uintptr_t)start + len;

    while (p < end) {
        asm volatile("dc ivac, %0" :: "r"(p) : "memory");
        p += line_size;
    }
    asm volatile("dsb sy" ::: "memory");
}

/*
 * 从 Guest 的命令队列（GPA）中读取一条命令（32 字节 = 4 x u64）。
 * 返回 0 成功，-1 失败（页未映射等）。
 */
static int vits_read_cmd(struct vits_state *vits, uint64_t offset,
                         uint64_t cmd[4])
{
    paddr_t cmdq_pa;
    paddr_t gpa;

    if (!vits->cbaser || !(vits->cbaser & (1ULL << 63)) ||
        offset >= vits->cmdq_bytes || offset % VITS_CMD_SIZE) {
        WARN("vITS: invalid command queue entry offset=0x%lx size=%u",
             (unsigned long)offset, vits->cmdq_bytes);
        return -1;
    }

    /* CBASER 格式：V[63] | PA[51:12] | SIZE[7:0] */
    paddr_t cbaser_pa = vits->cbaser & 0x000FFFFFFFFFF000ULL;
    gpa = cbaser_pa + offset;

    /* 翻译 GPA → PA。命令可能跨页吗？32 字节对齐且 < PAGE_SIZE，不会。 */
    if (ipa_to_pa(vits->vm, gpa, &cmdq_pa) < 0) {
        WARN("vITS: cmdq GPA 0x%lx not mapped", (unsigned long)gpa);
        return -1;
    }

    /* Invalidate stale command data before reading Guest-owned queue memory. */
    vits_invalidate_cmd_range((void *)(uintptr_t)cmdq_pa, VITS_CMD_SIZE);

    /* 读 4 个 u64（小端，直接内存拷贝） */
    uint64_t *p = (uint64_t *)(uintptr_t)cmdq_pa;
    for (int i = 0; i < 4; i++)
        cmd[i] = p[i];

    return 0;
}

/* ---------- 命令处理 ---------- */

static void vits_cmd_sync(struct vits_state *vits)
{
    /* SYNC 空操作（所有命令同步处理） */
    (void)vits;
}

/*
 * MAPC: 映射 collection 到目标 Redistributor
 *   dw2 = V[63] | RDBASE(procnum)[47:16] | ICID[15:0]
 *
 * 简化：只支持 collection 0 → CPU0。
 * 物理 ITS 已经在 its_setup_pci_msi 里配置过 collection 0。
 */
static void vits_cmd_mapc(struct vits_state *vits, uint64_t cmd[4])
{
    uint16_t icid = (uint16_t)(cmd[2] & 0xFFFF);
    uint64_t rdbase = (cmd[2] >> 16) & 0xFFFFFFFFULL;
    int valid = (cmd[2] >> 63) & 1;

    INFO("vITS: MAPC icid=%u rdbase=0x%lx valid=%d",
         icid, (unsigned long)rdbase, valid);

    if (icid != 0) {
        WARN("vITS: MAPC only supports ICID 0 (got %u)", icid);
        return;
    }

    /* 物理 ITS 的 collection 0 已经在 pci_init 阶段配置好了（到 CPU0），
     * 这里不需要重复配置。 */
}

/*
 * MAPD: 映射设备（DeviceID → ITT）
 *   dw0[63:32] = DeviceID
 *   dw1[4:0] = ITT size
 *   dw2 = V[63] | ITT addr[51:8]
 *
 * 简化：
 *   - vDevID 直接 = 物理 DevID（设备唯一，且直通）
 *   - 物理 ITS 已经在 its_setup_pci_msi 里为该设备建了 ITT
 *   - 这里只做有效性检查，不重复下发命令
 */
static void vits_cmd_mapd(struct vits_state *vits, uint64_t cmd[4])
{
    uint32_t vdevid = (uint32_t)(cmd[0] >> 32);
    uint8_t itt_size = (uint8_t)(cmd[1] & 0x1F);
    uint64_t itt_addr = (cmd[2] & ~0xFFULL);
    int valid = (cmd[2] >> 63) & 1;

    INFO("vITS: MAPD vdevid=%u itt_size=%u itt_addr=0x%lx valid=%d",
         vdevid, itt_size, (unsigned long)itt_addr, valid);

    if (!vits->vm || vits->vm->vmid != 1 ||
        !pci_edu_owns_requester(vdevid)) {
        WARN("vITS: MAPD rejected for unowned requester %u", vdevid);
        return;
    }

    if (!valid) {
        WARN("vITS: MAPD invalid not supported");
        return;
    }

    /*
     * 物理 ITS 已经在 pci_init() -> its_setup_pci_msi() 中为 edu 设备
     * 配置了 MAPD。这里不再重复下发，避免覆盖已有映射。
     *
     * 扩展点：未来多设备时，在这里调用 its_setup_pci_msi() 分配。
     */
}

/*
 * MAPTI 失败回滚：
 *   替换路径（have_prev）恢复替换前的 vINTID，保留反查表仍指向的映射；
 *   新分配路径删除路由并清空 shadow。
 */
static void vits_mapti_rollback(struct vits_state *vits, unsigned int plpi,
                                int have_prev,
                                const struct irq_route *prev_route)
{
    if (have_prev) {
        irq_domain_register(plpi, vits->vm->vmid, 0, prev_route->vintid);
        lpi_map_acquire();
        lpi_map[plpi - LPI_MAP_BASE].vmid = (uint16_t)vits->vm->vmid;
        lpi_map[plpi - LPI_MAP_BASE].vintid = prev_route->vintid;
        lpi_map_release();
        WARN("vITS: MAPTI rolled back replacement to vINTID=%u",
             prev_route->vintid);
        return;
    }

    irq_domain_unregister_owned(plpi, vits->vm->vmid);
    lpi_map_acquire();
    lpi_map[plpi - LPI_MAP_BASE].vmid = 0;
    lpi_map[plpi - LPI_MAP_BASE].vintid = 0;
    lpi_map_release();
    WARN("vITS: MAPTI rolled back new mapping for pLPI %u", plpi);
}

/*
 * MAPTI: 映射事件到 LPI（EventID → (pINTID, ICID)）
 *   dw0[63:32] = DeviceID
 *   dw1[63:32] = pINTID（Guest 指定的 vINTID/LPI 号）
 *   dw1[31:0]  = EventID
 *   dw2[15:0]  = ICID
 *
 * 处理：
 *   1. 分配一个物理 LPI 号（从 next_alloc_plpi 递增）
 *   2. 建立 pLPI → (vmid, vINTID) 映射
 *   4. 配置 LPI 属性表并等待物理 ITS MAPTI 完成
 *   5. 物理完成后提交 vITS shadow mapping
 */
static int vits_cmd_mapti(struct vits_state *vits, uint64_t cmd[4],
                          uint64_t cmd_offset)
{
    uint32_t vdevid = (uint32_t)(cmd[0] >> 32);
    uint32_t veventid = (uint32_t)(cmd[1] & 0xFFFFFFFFULL);
    uint32_t vintid = (uint32_t)(cmd[1] >> 32);
    uint16_t vicid = (uint16_t)(cmd[2] & 0xFFFF);

    INFO("vITS: MAPTI vdevid=%u veventid=%u vintid=%u vicid=%u",
         vdevid, veventid, vintid, vicid);

    if (!vits->vm || vits->vm->vmid != 1 ||
        !pci_edu_owns_requester(vdevid)) {
        if (!vits->halted || vits->halted_at != cmd_offset)
            WARN("vITS: MAPTI rejected for unowned requester %u", vdevid);
        vits->halt_reason = VITS_ERR_UNOWNED_DEVICE;
        return -1;
    }
    if (veventid == 0) {
        unsigned int plpi = pci_edu_lpi_base;
        if (!plpi || irq_domain_register(plpi, vits->vm->vmid, 0,
                                         vintid) < 0)
            return -1;
        lpi_map_acquire();
        lpi_map[plpi - LPI_MAP_BASE].vmid = (uint16_t)vits->vm->vmid;
        lpi_map[plpi - LPI_MAP_BASE].vintid = vintid;
        lpi_map_release();
        if (vits_lpi_audit_owned_range(plpi, 1, vits->vm->vmid,
                                       vintid) < 0) {
            irq_domain_unregister_owned(plpi, vits->vm->vmid);
            lpi_map_acquire();
            lpi_map[plpi - LPI_MAP_BASE].vmid = 0;
            lpi_map[plpi - LPI_MAP_BASE].vintid = 0;
            lpi_map_release();
            return -1;
        }
        INFO("vITS: MAPTI uses PCI MSI LPI %u for event 0", plpi);
        return 0;
    }

    /*
     * vINTID 检查：
     * 物理 LPI 必须 >= 8192（规范 + QEMU 强制），但 Guest 视角的
     * vINTID 可以是任意值（由 Hypervisor 决定注入什么虚拟中断号）。
     *
     * Phase 9 MVP：Guest 可以自由指定 vINTID（我们直接保存到映射表）。
     * 但为了符合 ITS 规范的预期（MAPTI 配置的是 LPI），Guest 一般会
     * 用 >= 8192 的值。irq.c 里注入时会转换为虚拟 SPI 号。
     *
     * 这里不做范围限制，Guest 传什么存什么。
     */

    /*
     * 替换语义：同一 (vdevid, veventid) 已有映射时复用其 pLPI，
     * 不消耗新 pLPI；irq_domain 与 shadow 以新 vINTID 原位覆盖。
     */
    int evt_idx = -1;
    for (unsigned int i = 0; i < VITS_EVT_MAP_MAX; i++) {
        if (vits->evt_map[i].plpi && vits->evt_map[i].devid == vdevid &&
            vits->evt_map[i].eventid == veventid) {
            evt_idx = (int)i;
            break;
        }
    }

    unsigned int plpi;
    if (evt_idx >= 0) {
        plpi = vits->evt_map[evt_idx].plpi;
    } else {
        /* 原子保留一个唯一的物理 LPI；失败后不回退全局 cursor。 */
        if (vits_reserve_plpi(&plpi) < 0) {
            WARN("vITS: no free physical LPI");
            return -1;
        }
    }

    /*
     * 替换路径先记下旧路由：本次 MAPTI 失败时要用旧 vINTID 恢复，
     * 而不是删除路由——反查表仍指向该 pLPI，删除会让映射静默失效。
     */
    struct irq_route prev_route;
    int have_prev = (evt_idx >= 0) &&
                    irq_domain_lookup(plpi, &prev_route) == 0;

    /* 记录映射 */
    if (irq_domain_register(plpi, vits->vm->vmid, 0, vintid) < 0) {
        WARN("vITS: LPI ownership registration failed for %u", plpi);
        return -1;
    }

    {
        uint64_t gicr = gic_get_redist_base(0);
        uint64_t propbaser = get64(gicr + 0x0070);
        uint64_t prop_base = propbaser & 0x000FFFFFFFFFF000ULL;
        volatile uint8_t *cfg =
            (volatile uint8_t *)(uintptr_t)(prop_base + (plpi - LPI_MAP_BASE));
        *cfg = 0x60 | 0x01;
    }
    asm volatile("dsb sy" ::: "memory");

    if (its_send_mapti(vdevid, plpi, veventid, vicid) < 0) {
        vits_mapti_rollback(vits, plpi, have_prev, &prev_route);
        return -1;
    }
    lpi_map_acquire();
    lpi_map[plpi - LPI_MAP_BASE].vmid = (uint16_t)vits->vm->vmid;
    lpi_map[plpi - LPI_MAP_BASE].vintid = vintid;
    lpi_map_release();
    if (vits_lpi_audit_owned_range(plpi, 1, vits->vm->vmid,
                                   vintid) < 0) {
        vits_mapti_rollback(vits, plpi, have_prev, &prev_route);
        return -1;
    }

    /* 记录 (vdevid, veventid) -> pLPI，供后续替换复用。 */
    if (evt_idx < 0) {
        for (unsigned int i = 0; i < VITS_EVT_MAP_MAX; i++) {
            if (!vits->evt_map[i].plpi) {
                vits->evt_map[i].plpi = plpi;
                vits->evt_map[i].devid = vdevid;
                vits->evt_map[i].eventid = veventid;
                evt_idx = (int)i;
                break;
            }
        }
        if (evt_idx < 0) {
            irq_domain_unregister_owned(plpi, vits->vm->vmid);
            lpi_map_acquire();
            lpi_map[plpi - LPI_MAP_BASE].vmid = 0;
            lpi_map[plpi - LPI_MAP_BASE].vintid = 0;
            lpi_map_release();
            WARN("vITS: MAPTI reverse map full for devid=%u eventid=%u",
                 vdevid, veventid);
            return -1;
        }
    }

    INFO("vITS: MAPTI done: vINTID=%u -> pLPI=%u (vmid=%u)",
         vintid, plpi, (unsigned)vits->vm->vmid);
    return 0;
}

/*
 * DISCARD: 清除 (DeviceID, EventID) 的映射
 *   dw0[63:32] = DeviceID
 *   dw1[31:0]  = EventID
 *
 * 处理：
 *   1. 查反查表定位 pLPI；未映射视为成功（规范允许 DISCARD 不存在的映射）
 *   2. 下发物理 DISCARD
 *   3. 撤销 irq_domain 路由与 lpi_map shadow
 *   4. 清空反查表项；pLPI 保留不复用（避免复用后收到过期中断）
 */
static int vits_cmd_discard(struct vits_state *vits, uint64_t cmd[4],
                            uint64_t cmd_offset)
{
    uint32_t vdevid = (uint32_t)(cmd[0] >> 32);
    uint32_t veventid = (uint32_t)(cmd[1] & 0xFFFFFFFFULL);

    INFO("vITS: DISCARD vdevid=%u veventid=%u", vdevid, veventid);

    if (!vits->vm || vits->vm->vmid != 1 ||
        !pci_edu_owns_requester(vdevid)) {
        if (!vits->halted || vits->halted_at != cmd_offset)
            WARN("vITS: DISCARD rejected for unowned requester %u", vdevid);
        vits->halt_reason = VITS_ERR_UNOWNED_DEVICE;
        return -1;
    }

    int evt_idx = -1;
    for (unsigned int i = 0; i < VITS_EVT_MAP_MAX; i++) {
        if (vits->evt_map[i].plpi && vits->evt_map[i].devid == vdevid &&
            vits->evt_map[i].eventid == veventid) {
            evt_idx = (int)i;
            break;
        }
    }
    if (evt_idx < 0) {
        INFO("vITS: DISCARD no mapping for devid=%u eventid=%u",
             vdevid, veventid);
        return 0;
    }

    unsigned int plpi = vits->evt_map[evt_idx].plpi;

    if (its_send_discard(vdevid, veventid) < 0) {
        WARN("vITS: physical DISCARD failed for devid=%u eventid=%u",
             vdevid, veventid);
        return -1;
    }
    if (vits_lpi_revoke_owned(plpi, vits->vm->vmid) < 0) {
        WARN("vITS: DISCARD route revoke failed for pLPI=%u", plpi);
        return -1;
    }

    vits->evt_map[evt_idx].plpi = 0;
    vits->evt_map[evt_idx].devid = 0;
    vits->evt_map[evt_idx].eventid = 0;

    INFO("vITS: DISCARD done: devid=%u eventid=%u pLPI=%u (pLPI retained)",
         vdevid, veventid, plpi);
    return 0;
}

/*
 * 处理一条命令
 */
static int vits_process_one(struct vits_state *vits, uint64_t offset)
{
    uint64_t cmd[4];

    if (vits_read_cmd(vits, offset, cmd) < 0)
        return -1;

    uint8_t opcode = (uint8_t)(cmd[0] & 0xFF);

    switch (opcode) {
    case VITS_CMD_SYNC:
        vits_cmd_sync(vits);
        break;
    case VITS_CMD_MAPC:
        vits_cmd_mapc(vits, cmd);
        break;
    case VITS_CMD_MAPD:
        vits_cmd_mapd(vits, cmd);
        break;
    case VITS_CMD_MAPTI:
        if (vits_cmd_mapti(vits, cmd, offset) < 0)
            return -1;
        break;
    case VITS_CMD_DISCARD:
        if (vits_cmd_discard(vits, cmd, offset) < 0)
            return -1;
        break;
    default:
        /*
         * 未知 opcode 视为命令失败：不猜测其语义，也不消费该槽位。
         * 与其它失败路径一致，由 vits_process_cmdq() 停止队列并保留 CREADR。
         * 同一停止点被重复访问时不再重复告警。
         */
        if (!vits->halted || vits->halted_at != offset)
            WARN("vITS: unknown command opcode 0x%02x at offset 0x%lx",
                 opcode, (unsigned long)offset);
        vits->halt_reason = VITS_ERR_UNKNOWN_OPCODE;
        return -1;
    }

    return 0;
}

/*
 * 处理命令队列：从 CREADR 到 CWRITER 之间的命令。
 * 遇到第一条失败命令即停止，CREADR 保持在该命令偏移，
 * 后续命令不再执行，直到 Guest 修复状态并重新推进 CWRITER。
 */
static void vits_process_cmdq(struct vits_state *vits)
{
    uint64_t cw = vits->cwriter;
    uint64_t cr = vits->creadr;

    if (!vits->cmdq_bytes || vits->cmdq_bytes % VITS_CMD_SIZE ||
        cw >= vits->cmdq_bytes || cw % VITS_CMD_SIZE ||
        cr >= vits->cmdq_bytes || cr % VITS_CMD_SIZE) {
        WARN("vITS: reject invalid command queue pointers CR=0x%lx CW=0x%lx size=%u",
             (unsigned long)cr, (unsigned long)cw, vits->cmdq_bytes);
        return;
    }

    while (cr != cw) {
        if (vits_process_one(vits, cr) < 0) {
            /* 同一停止点被 Guest 重复推进 CWRITER 时不重复告警。 */
            if (!vits->halted || vits->halted_at != cr) {
                WARN("vITS: command at 0x%lx failed; halting, CREADR retained",
                     (unsigned long)cr);
                vits->halted = 1;
                vits->halted_at = cr;
            }
            vits->creadr = cr;
            return;
        }

        cr += VITS_CMD_SIZE;
        if (cr >= vits->cmdq_bytes)
            cr = 0;
    }

    vits->halted = 0;
    vits->halt_reason = VITS_ERR_OTHER;
    vits->creadr = cw;
}

/* ---------- MMIO 写 ---------- */

/*
 * MMIO 写可能是 32 位或 64 位的。
 * Guest 可能分两次写 64 位寄存器（先写低 32 位，再写高 32 位）。
 * 对于 64 位寄存器，我们需要缓存低 32 位，等写了高 32 位再合并。
 *
 * 简化方案：因为 QEMU/virt 平台下所有地址都是 32 位的（<4GB），
 * 高 32 位写 0 或 1（V 位）都可以。
 * 我们用一个缓存变量记录上次写的低 32 位，写高 32 位时合并。
 * 缓存在 per-VM 的 vits_state.write_lo_cache 中。
 */
#define VITS_REG_CBASER_IDX  0
#define VITS_REG_BASER0_IDX  1
#define VITS_REG_BASER1_IDX  2

static int offset_to_cache_idx(uint32_t reg_off)
{
    if (reg_off == VITS_CBASER) return VITS_REG_CBASER_IDX;
    if (reg_off == VITS_BASER(0)) return VITS_REG_BASER0_IDX;
    if (reg_off == VITS_BASER(1)) return VITS_REG_BASER1_IDX;
    return -1;
}

void vits_mmio_write(struct vits_state *vits, uint32_t offset, uint64_t val)
{
    uint32_t reg_off = offset & ~7;
    int hi_word = (offset & 4) ? 1 : 0;
    uint32_t val32 = (uint32_t)(val & 0xFFFFFFFF);

    /*
     * 对于 64 位寄存器（CBASER/BASER），支持分两次写：
     *   - 写低 32 位：缓存起来
     *   - 写高 32 位：和缓存的低 32 位合并，然后执行实际写
     */
    vits_lock(vits);

    int cache_idx = offset_to_cache_idx(reg_off);

    if (cache_idx >= 0) {
        if (!hi_word) {
            vits->write_lo_cache[cache_idx] = val32;
            vits->write_lo_valid[cache_idx] = 1;
            vits_unlock(vits);
            return;
        }
        if (!vits->write_lo_valid[cache_idx]) {
            WARN("vITS: reject high-word write without low word (offset=0x%x)",
                 offset);
            vits_unlock(vits);
            return;
        }
        val = ((uint64_t)val32 << 32) | vits->write_lo_cache[cache_idx];
        vits->write_lo_valid[cache_idx] = 0;
    }

    switch (reg_off) {
    case VITS_CTLR: {
        uint32_t old_ctlr = vits->ctlr;

        for (unsigned int i = 0; i < ARRAY_SIZE(vits->write_lo_valid); i++)
            vits->write_lo_valid[i] = 0;
        vits->ctlr = val32 & 0x1;  /* 只保留 Enabled 位 */
        INFO("vITS: CTLR = %u", vits->ctlr);
        if (!(old_ctlr & 0x1) && (vits->ctlr & 0x1) &&
            vits->creadr != vits->cwriter)
            vits_process_cmdq(vits);
        break;
    }

    case VITS_CBASER: {
        if (vits->ctlr & 0x1) {
            WARN("vITS: reject CBASER write while enabled");
            break;
        }

        uint32_t size = (uint32_t)(val & 0xFF);
        uint32_t cmdq_bytes = (size + 1) * PAGE_SIZE;
        if (!(val & (1ULL << 63)) ||
            cmdq_bytes < VITS_CMD_SIZE || cmdq_bytes % VITS_CMD_SIZE) {
            WARN("vITS: reject invalid CBASER=0x%lx", (unsigned long)val);
            break;
        }

        vits->cbaser = val;
        vits->cmdq_bytes = cmdq_bytes;
        vits->cwriter = 0;
        vits->creadr = 0;
        vits->halted = 0;
        vits->halted_at = 0;
        vits->halt_reason = VITS_ERR_OTHER;
        INFO("vITS: CBASER = 0x%lx (cmdq %u bytes)",
             (unsigned long)val, vits->cmdq_bytes);
        break;
    }

    case VITS_CWRITER: {
        uint64_t new_cwriter = val & 0x000FFFFFULL;  /* OFFSET [19:0] */

        if (!vits->cmdq_bytes || new_cwriter >= vits->cmdq_bytes ||
            new_cwriter % VITS_CMD_SIZE) {
            WARN("vITS: reject invalid CWRITER=0x%lx size=%u",
                 (unsigned long)new_cwriter, vits->cmdq_bytes);
            break;
        }

        /* 发布有效 CWRITER 后，enabled 状态同步处理新命令。 */
        vits->cwriter = new_cwriter;
        INFO("vITS: CWRITER = 0x%lx", (unsigned long)vits->cwriter);

        if (vits->ctlr & 0x1)
            vits_process_cmdq(vits);
        break;
    }

    case VITS_CREADR:
        /* Guest 写 CREADR？一般不写。忽略。 */
        break;

    case VITS_BASER(0):
    case VITS_BASER(1):
    case VITS_BASER(2):
    case VITS_BASER(3):
    case VITS_BASER(4):
    case VITS_BASER(5):
    case VITS_BASER(6):
    case VITS_BASER(7): {
        int n = (reg_off - VITS_BASER(0)) / 8;
        vits->baser[n] = val;
        INFO("vITS: BASER%d = 0x%lx", n, (unsigned long)val);
        break;
    }

    case VITS_TRANSLATER:
        /* Guest software doorbell writes are ignored for passthrough devices. */
        break;

    case VITS_ERR_STATUS:
        /* 软件定义的只读状态寄存器，写忽略。 */
        break;

    default:
        WARN("vITS: unknown write offset 0x%x val=0x%lx",
             offset, (unsigned long)val);
        break;
    }

    vits_unlock(vits);
}

/* ---------- 中断注入查表 ---------- */

int vits_lpi_revoke_owned(unsigned int plpi, unsigned long vmid)
{
    if (plpi < LPI_MAP_BASE || plpi >= LPI_MAP_BASE + LPI_MAP_MAX ||
        irq_domain_unregister_owned(plpi, vmid) < 0)
        return -1;
    lpi_map_acquire();
    lpi_map[plpi - LPI_MAP_BASE].vmid = 0;
    lpi_map[plpi - LPI_MAP_BASE].vintid = 0;
    lpi_map_release();
    return 0;
}

int vits_lpi_revoke_range_owned(unsigned int first_plpi,
                                unsigned int count, unsigned long vmid)
{
    if (!count || first_plpi < LPI_MAP_BASE ||
        count > LPI_MAP_BASE + LPI_MAP_MAX - first_plpi ||
        irq_domain_unregister_owned_range(first_plpi, count, vmid) < 0)
        return -1;
    lpi_map_acquire();
    for (unsigned int i = 0; i < count; i++) {
        lpi_map[first_plpi + i - LPI_MAP_BASE].vmid = 0;
        lpi_map[first_plpi + i - LPI_MAP_BASE].vintid = 0;
    }
    lpi_map_release();
    return 0;
}

int vits_lpi_audit_owned_range(unsigned int first_plpi,
                                unsigned int count, unsigned long vmid,
                                unsigned int first_vintid)
{
    if (!count || first_plpi < LPI_MAP_BASE ||
        count > LPI_MAP_BASE + LPI_MAP_MAX - first_plpi)
        return -1;
    int shadow_valid = 1;
    lpi_map_acquire();
    for (unsigned int i = 0; i < count; i++) {
        struct lpi_map_entry *entry =
            &lpi_map[first_plpi + i - LPI_MAP_BASE];
        if (entry->vmid != vmid || entry->vintid != first_vintid + i) {
            shadow_valid = 0;
            break;
        }
    }
    lpi_map_release();
    if (!shadow_valid)
        return -1;
    return irq_domain_audit_owned_range(first_plpi, count, vmid,
                                        first_vintid);
}

int vits_lpi_revoke(unsigned int plpi)
{
    struct irq_route route;

    if (irq_domain_lookup(plpi, &route) < 0 ||
        route.owner != IRQ_DOMAIN_OWNER_VM)
        return -1;
    return vits_lpi_revoke_owned(plpi, route.vmid);
}

int vits_lpi_to_virq(unsigned int plpi, unsigned int *out_vmid,
                     unsigned int *out_vintid)
{
    struct irq_route route;

    if (irq_domain_lookup(plpi, &route) < 0 ||
        route.owner != IRQ_DOMAIN_OWNER_VM)
        return -1;
    *out_vmid = (unsigned int)route.vmid;
    *out_vintid = route.vintid;
    return 0;
}

/* ---------- MMIO 读 ---------- */

/*
 * MMIO 读可能是 32 位或 64 位的。
 * Guest 可能分两次读 64 位寄存器（先读低 32 位，再读高 32 位）。
 * 我们把 offset 向下对齐到 8 字节，根据偏移判断是高/低 32 位。
 */
uint64_t vits_mmio_read(struct vits_state *vits, uint32_t offset)
{
    uint32_t reg_off = offset & ~7;
    int hi_word = (offset & 4) ? 1 : 0;
    uint64_t val = 0;

    vits_lock(vits);

    switch (reg_off) {
    case VITS_CTLR:
        val = vits->ctlr;
        break;
    case VITS_TYPER:
        val = 0x00A10E11ULL;
        break;
    case VITS_CBASER:
        val = vits->cbaser;
        break;
    case VITS_CWRITER:
        val = vits->cwriter;
        break;
    case VITS_CREADR:
        val = vits->creadr;
        break;
    case VITS_BASER(0):
    case VITS_BASER(1):
    case VITS_BASER(2):
    case VITS_BASER(3):
    case VITS_BASER(4):
    case VITS_BASER(5):
    case VITS_BASER(6):
    case VITS_BASER(7):
        val = vits->baser[(reg_off - VITS_BASER(0)) / 8];
        break;
    case VITS_TRANSLATER:
        val = 0;
        break;
    case VITS_ERR_STATUS:
        /* bit0 = halted，[15:8] = 原因码，[31:16] = 失败命令槽位号 */
        val = (uint64_t)(vits->halted ? 1 : 0) |
              ((uint64_t)vits->halt_reason << 8) |
              ((vits->halted_at / VITS_CMD_SIZE) << 16);
        break;
    default:
        WARN("vITS: unknown read offset 0x%x", offset);
        val = 0;
        break;
    }

    if (hi_word)
        val = (val >> 32) & 0xFFFFFFFFULL;
    else
        val &= 0xFFFFFFFFULL;

    vits_unlock(vits);
    return val;
}
