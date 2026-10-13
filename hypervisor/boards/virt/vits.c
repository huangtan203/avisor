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

/* 下一个可分配的物理 LPI（从 9000 开始，避开 Phase 8 的 edu 8192..8207） */
static unsigned int next_alloc_plpi = 9000;

/* ---------- 全局初始化 ---------- */

void vits_global_init(void)
{
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

/*
 * 从 Guest 的命令队列（GPA）中读取一条命令（32 字节 = 4 x u64）。
 * 返回 0 成功，-1 失败（页未映射等）。
 */
static int vits_read_cmd(struct vits_state *vits, uint64_t offset,
                         uint64_t cmd[4])
{
    paddr_t cmdq_pa;
    paddr_t gpa;

    if (!vits->cbaser || !(vits->cbaser & (1ULL << 63))) {
        WARN("vITS: command queue not configured");
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
 * MAPTI: 映射事件到 LPI（EventID → (pINTID, ICID)）
 *   dw0[63:32] = DeviceID
 *   dw1[63:32] = pINTID（Guest 指定的 vINTID/LPI 号）
 *   dw1[31:0]  = EventID
 *   dw2[15:0]  = ICID
 *
 * 处理：
 *   1. 分配一个物理 LPI 号（从 next_alloc_plpi 递增）
 *   2. 建立 pLPI → (vmid, vINTID) 映射
 *   3. 向物理 ITS 发 MAPTI 命令（使用物理 DevID + 分配的 pLPI）
 *   4. 配置 LPI 属性表（优先级 + 使能）
 */
static void vits_cmd_mapti(struct vits_state *vits, uint64_t cmd[4])
{
    uint32_t vdevid = (uint32_t)(cmd[0] >> 32);
    uint32_t veventid = (uint32_t)(cmd[1] & 0xFFFFFFFFULL);
    uint32_t vintid = (uint32_t)(cmd[1] >> 32);
    uint16_t vicid = (uint16_t)(cmd[2] & 0xFFFF);

    INFO("vITS: MAPTI vdevid=%u veventid=%u vintid=%u vicid=%u",
         vdevid, veventid, vintid, vicid);

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

    /* 分配物理 LPI */
    if (next_alloc_plpi >= LPI_MAP_BASE + LPI_MAP_MAX) {
        WARN("vITS: no free physical LPI");
        return;
    }
    unsigned int plpi = next_alloc_plpi++;

    /* 记录映射 */
    lpi_map[plpi - LPI_MAP_BASE].vmid = (uint16_t)vits->vm->vmid;
    lpi_map[plpi - LPI_MAP_BASE].vintid = vintid;

    /*
     * 配置物理 LPI 属性表 + 下发物理 MAPTI 命令。
     *
     * 注意：物理 ITS 的设备表已经有该 devid 的条目（Phase 8 MAPD 过了），
     * 且 collection 0 也存在。直接发 MAPTI 即可。
     *
     * 但 its_setup_pci_msi() 已经用了 LPI 8192..8207 给 edu 的 16 个 vector。
     * 我们现在分配的新 LPI 需要走完整的 MAPTI 路径。
     *
     * 为了避免重复造轮子，这里直接调用 its_send_cmd 的等价功能。
     * 但 its.c 里的 its_send_cmd 是 static 的...
     * 解决：直接读写物理 ITS 寄存器，发命令。
     */

    /* 物理 DevID = vDevID（直通场景，设备唯一） */
    uint32_t pdevid = vdevid;

    /*
     * 1. 配置 LPI 属性表
     *    （属性表基址在 its.c 里是 static 的，我们需要拿到它...
     *     简化：直接读写 GICR PROPBASER 拿到基址。
     *     或者：调用 its.c 暴露的接口。）
     *
     * 更好的方式：把 LPI 配置做成 its 层的公开 API。
     * 先这样：从 GICR_PROPBASER 读出来。
     */
    {
        uint64_t gicr = gic_get_redist_base(0);
        uint64_t propbaser = get64(gicr + 0x0070);
        uint64_t prop_base = propbaser & 0x000FFFFFFFFFF000ULL;

        /* LPI 优先级 + 使能 */
        volatile uint8_t *cfg =
            (volatile uint8_t *)(uintptr_t)(prop_base + (plpi - LPI_MAP_BASE));
        *cfg = 0x60 | 0x01;  /* prio=0x60, enable=1 */
    }

    /*
     * 2. 发物理 MAPTI 命令
     *    因为 its_send_cmd 是 static 的，我们这里复制一份实现？
     *    不好。改为：在 its.h 增加一个 its_send_mapti 接口。
     *
     * 临时方案（MVP）：直接操作物理 ITS 寄存器，写命令到命令队列。
     * 命令队列基址从 GITS_CBASER 读。
     */
    {
        uint64_t cbaser = get64(0x08080000 + 0x0080);  /* GITS_CBASER */
        uint64_t cmdq_pa = cbaser & 0x000FFFFFFFFFF000ULL;
        uint64_t cwriter = get64(0x08080000 + 0x0088); /* GITS_CWRITER */

        uint64_t *slot = (uint64_t *)(uintptr_t)(cmdq_pa + cwriter);
        slot[0] = 0x0A | ((uint64_t)pdevid << 32);  /* MAPTI + DeviceID */
        slot[1] = ((uint64_t)plpi << 32) | veventid; /* pINTID + EventID */
        slot[2] = 0;  /* ICID = 0 */
        slot[3] = 0;
        asm volatile("dsb sy" ::: "memory");

        cwriter += 32;
        if (cwriter >= PAGE_SIZE)
            cwriter = 0;
        put64(0x08080000 + 0x0088, cwriter);

        /* 等处理完成（CREADR == CWRITER） */
        int timeout = 1000000;
        while (get64(0x08080000 + 0x0090) != cwriter && --timeout > 0)
            ;
        if (timeout == 0)
            WARN("vITS: physical MAPTI timeout");
    }

    INFO("vITS: MAPTI done: vINTID=%u -> pLPI=%u (vmid=%u)",
         vintid, plpi, (unsigned)vits->vm->vmid);
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
        vits_cmd_mapti(vits, cmd);
        break;
    default:
        WARN("vITS: unknown command opcode 0x%02x", opcode);
        break;
    }

    return 0;
}

/*
 * 处理命令队列：从 CREADR 到 CWRITER 之间的所有命令
 */
static void vits_process_cmdq(struct vits_state *vits)
{
    uint64_t cw = vits->cwriter;
    uint64_t cr = vits->creadr;

    while (cr != cw) {
        vits_process_one(vits, cr);

        cr += VITS_CMD_SIZE;
        if (cr >= vits->cmdq_bytes)
            cr = 0;
    }

    /* 更新 CREADR = CWRITER（同步处理，读完了） */
    vits->creadr = cw;
}

/* ---------- MMIO 读 ---------- */

/*
 * MMIO 读可能是 32 位或 64 位的。
 * Guest 可能分两次读 64 位寄存器（先读低 32 位，再读高 32 位）。
 * 我们把 offset 向下对齐到 8 字节，根据偏移判断是高/低 32 位。
 */
uint64_t vits_mmio_read(struct vits_state *vits, uint32_t offset)
{
    uint32_t reg_off = offset & ~7;  /* 对齐到 8 字节 */
    int hi_word = (offset & 4) ? 1 : 0;
    uint64_t val = 0;

    switch (reg_off) {
    case VITS_CTLR:
        val = vits->ctlr;
        break;

    case VITS_TYPER:
        /*
         * 简化的 TYPER 值：
         *   bit0 = 1 (ITTyPIS, IMPDEF)
         *   bit4 = 1 (CIL, collection is level-triggered)
         *   bit12 = 1 (Physical LPI supported)
         *   bits[10:8] = CIDbits=7 (256 collections)
         *   bits[21:17] = DevBits=7 (256 devices)
         *   bits[25:22] = IDBits=11 (4096 LPIs per device)
         *   = 0x00A10E11
         */
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
    case VITS_BASER(7): {
        int n = (reg_off - VITS_BASER(0)) / 8;
        val = vits->baser[n];
        break;
    }

    case VITS_TRANSLATER:
        val = 0;  /* WO，读返回 0 */
        break;

    default:
        WARN("vITS: unknown read offset 0x%x", offset);
        return 0;
    }

    if (hi_word)
        return (val >> 32) & 0xFFFFFFFFULL;
    return val & 0xFFFFFFFFULL;
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
    int cache_idx = offset_to_cache_idx(reg_off);

    if (cache_idx >= 0) {
        if (!hi_word) {
            /* 低 32 位：缓存（per-VM 缓存，避免多 VM 串扰） */
            vits->write_lo_cache[cache_idx] = val32;
            return;
        }
        /* 高 32 位：合并低 32 位 + 高 32 位，构造完整 64 位值 */
        uint64_t full_val =
            ((uint64_t)val32 << 32) | vits->write_lo_cache[cache_idx];
        val = full_val;
    }

    switch (reg_off) {
    case VITS_CTLR:
        vits->ctlr = val32 & 0x1;  /* 只保留 Enabled 位 */
        INFO("vITS: CTLR = %u", vits->ctlr);
        break;

    case VITS_CBASER: {
        vits->cbaser = val;
        /* 计算命令队列大小：SIZE[7:0] = 页数 - 1 */
        uint32_t size = (uint32_t)(val & 0xFF);
        vits->cmdq_bytes = (size + 1) * PAGE_SIZE;
        INFO("vITS: CBASER = 0x%lx (cmdq %u bytes)",
             (unsigned long)val, vits->cmdq_bytes);
        break;
    }

    case VITS_CWRITER:
        /* 写 CWRITER = Guest 提交了新命令，需要处理 */
        vits->cwriter = val & 0x000FFFFFULL;  /* OFFSET [19:0] */
        INFO("vITS: CWRITER = 0x%lx", (unsigned long)vits->cwriter);

        if (vits->ctlr & 0x1) {
            vits_process_cmdq(vits);
        }
        break;

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
        /*
         * Guest 写 MSI doorbell（EventID）。
         * 但在直通场景下，是设备（如 edu）发起 MSI，不是 Guest 直接写。
         * Guest 写 TRANSLATER 通常用于触发软件 MSI，这里忽略。
         *
         * 注意：真实的 MSI 路径是：
         *   设备写 GITS_TRANSLATER（物理地址）
         *   → 物理 ITS 翻译 → 物理 LPI → EL2 中断
         *   → vits_lpi_to_virq 查表 → 注入 vINTID
         *
         * 所以这里不需要处理。
         */
        break;

    default:
        WARN("vITS: unknown write offset 0x%x val=0x%lx",
             offset, (unsigned long)val);
        break;
    }
}

/* ---------- 中断注入查表 ---------- */

int vits_lpi_to_virq(unsigned int plpi, unsigned int *out_vmid,
                     unsigned int *out_vintid)
{
    if (plpi < LPI_MAP_BASE || plpi >= LPI_MAP_BASE + LPI_MAP_MAX)
        return -1;

    struct lpi_map_entry *e = &lpi_map[plpi - LPI_MAP_BASE];
    if (e->vmid == 0)
        return -1;

    *out_vmid = e->vmid;
    *out_vintid = e->vintid;
    return 0;
}
