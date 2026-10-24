// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - GICv3 ITS (Interrupt Translation Service)
 *
 * Phase 8: MSI 中断直通的最小可用 ITS 驱动
 *
 * 实现范围（单设备、单 Collection、少量 LPI）：
 *   - LPI 配置表（prop table）+ 挂起表（pend table）安装到 CPU0 Redistributor
 *   - ITS 命令队列 + 设备表 + 集合表的分配与编程
 *   - MAPC / MAPD / MAPTI / SYNC 命令
 *
 * 所有寄存器格式与 QEMU 6.2 的仿真行为对齐（hw/intc/arm_gicv3_its.c），
 * 具体验证依据见各函数内注释。
 */

#include "boards/virt/its.h"
#include "boards/virt/base.h"
#include "boards/virt/gicv3.h"
#include "common/mm.h"
#include "common/platform.h"
#include "common/debug.h"
#include "common/utils.h"

/* ---------- GITS 寄存器（VA = PA 等值映射，位于 MMIO 区域内） ---------- */

#define GITS_CTLR          (GITS_BASE + 0x0000)  /* 控制：bit0=Enabled */
#define GITS_TYPER         (GITS_BASE + 0x0008)  /* 能力（RO） */
#define GITS_CBASER        (GITS_BASE + 0x0080)  /* 命令队列基址 */
#define GITS_CWRITER       (GITS_BASE + 0x0088)  /* 命令队列写指针 */
#define GITS_CREADR        (GITS_BASE + 0x0090)  /* 命令队列读指针 */
#define GITS_BASER(n)      (GITS_BASE + 0x0100 + (n) * 8) /* 表基址 n=0..7 */
#define GITS_TRANSLATER    (GITS_BASE + 0x10040) /* MSI doorbell（WO） */

/* Redistributor RD_base 帧内的 LPI 寄存器偏移 */
#define GICR_CTLR_OFF       0x0000   /* bit0 = EnableLPIs */
#define GICR_PROPBASER_OFF  0x0070   /* LPI 配置表基址 */
#define GICR_PENDBASER_OFF  0x0078   /* LPI 挂起表基址 */

/* ---------- ITS 命令操作码（QEMU: GITS_CMD_*） ---------- */

#define ITS_CMD_SYNC   0x05
#define ITS_CMD_MAPD   0x08
#define ITS_CMD_MAPC   0x09
#define ITS_CMD_MAPTI  0x0A

/* GICv3 LPI INTID 起始（QEMU 强制 pINTID >= 8192 才接受 MAPTI） */
#define LPI_INTID_BASE   8192

/* LPI 属性表配置字节：[7:2]=优先级, bit1=Group1, bit0=使能 */
#define LPI_PROP_ENABLE   0x1
/* Phase 7 经验：物理直通中断优先级必须高于虚拟中断默认的 0x80，
 * 否则无法抢占 EL1 guest 上下文（RPR 卡在 0x80） */
#define LPI_PROP_PRIO     0x60

/* ITT 表大小字段（MAPD dw1[4:0]）：表项数 = 2^(size+1) = 16 */
#define ITS_ITT_SIZE_SHIFT  3
#define ITS_ITT_MAX_EVENTS  (1 << (ITS_ITT_SIZE_SHIFT + 1))

/* 命令队列容量：1 页 = 4096 / 32 = 128 条命令 */
#define ITS_CMDQ_BYTES  PAGE_SIZE

/* ---------- 全局状态 ---------- */

static paddr_t cmdq_pa;      /* 命令队列基址（PA） */
static paddr_t dt_pa;        /* 设备表（BASER0） */
static paddr_t ct_pa;        /* 集合表（BASER1） */
static paddr_t lpi_prop_pa;  /* LPI 配置表 */
static paddr_t lpi_pend_pa;  /* LPI 挂起表 */
static unsigned int cq_byte_off;  /* 命令队列写偏移（字节） */

/* LPI 分配游标（简单递增分配） */
static unsigned int next_lpi = LPI_INTID_BASE;

/* ---------- 命令发送 ---------- */

/*
 * 发送一条 ITS 命令并等待 QEMU 处理完成。
 *
 * 命令布局（32 字节 = 4 x u64，小端）：
 *   dw0: [7:0] 命令码，[63:32] DeviceID（MAPD/MAPTI）
 *   dw1: MAPD = ITT size[4:0]；MAPTI = pINTID[63:32] + EventID[31:0]
 *   dw2: MAPD = ITT 地址[51:8] + V[63]；MAPC = ICID[15:0] +
 *        RDBASE(procnum)[47:16] + V[63]；MAPTI = ICID[15:0]
 *
 * QEMU 在 CWRITER 写入时同步处理命令（process_cmdq），
 * 处理完更新 CREADR，因此轮询 CREADR == CWRITER 即可。
 *
 * CWRITER.Offset 字段位于 [19:5]（命令 32 字节对齐），
 * 直接写 32 对齐的字节偏移即可正确编码。
 */
static void its_send_cmd(uint64_t d0, uint64_t d1, uint64_t d2, uint64_t d3)
{
    uint64_t *slot = (uint64_t *)(uintptr_t)cmdq_pa + (cq_byte_off / 8);

    slot[0] = d0;
    slot[1] = d1;
    slot[2] = d2;
    slot[3] = d3;
    asm volatile("dsb sy" ::: "memory");

    cq_byte_off += 32;
    if (cq_byte_off >= ITS_CMDQ_BYTES)
        cq_byte_off = 0;

    put64(GITS_CWRITER, cq_byte_off);

    /* 等 QEMU 处理完（CREADR 追上 CWRITER） */
    int timeout = 1000000;
    while (get64(GITS_CREADR) != cq_byte_off && --timeout > 0)
        ;
    if (timeout == 0)
        WARN("ITS: command queue stalled (CWR=%u CRR=%lu)",
             cq_byte_off, (unsigned long)get64(GITS_CREADR));
}

/* ---------- 对外接口 ---------- */

/*
 * MSI 消息地址 = GITS_TRANSLATER 的物理地址。
 * 设备（edu）发起 MSI 时对这个地址做内存写，写入值是 EventID；
 * PCIe 事务的 requester_id（= bus<<8|dev<<3|func）作为 DeviceID。
 */
uint64_t its_get_msi_address(void)
{
    return platform_gits_base() + 0x10040;
}

/* MSI 消息数据 = EventID（QEMU msi_send_message 把它 32 位写入 doorbell） */
uint32_t its_get_msi_data(unsigned int event_id)
{
    return (uint32_t)event_id;
}

/*
 * 为 PCI 设备建立完整的 MSI 映射链：
 *
 *   DeviceID --(DTE)--> ITT --(ITE)--> (pINTID, ICID)
 *   ICID --(CTE)--> procnum（目标 Redistributor）
 *
 * QEMU 在处理 MAPD/MAPC/MAPTI 时自己写这三张表（flat 模式，
 * 表项格式为 IMPDEF），我们只发命令，不直接写表。
 */
unsigned int its_setup_pci_msi(unsigned int bus, unsigned int dev,
                               unsigned int func, unsigned int num_vectors,
                               unsigned int target_cpu)
{
    /* QEMU pci_requester_id() = (bus << 8) | (dev << 3) | fn */
    uint32_t devid = (bus << 8) | (dev << 3) | func;

    if (num_vectors == 0)
        num_vectors = 1;
    if (num_vectors > ITS_ITT_MAX_EVENTS)
        num_vectors = ITS_ITT_MAX_EVENTS;

    /* 1. MAPC：collection 0 -> procnum target_cpu
     *    dw2 = V[63] | procnum[47:16] | ICID[15:0] */
    its_send_cmd(ITS_CMD_MAPC, 0,
                 (1ULL << 63) | ((uint64_t)target_cpu << 16) | 0, 0);

    /* 2. 分配 ITT 页（MAPD 的地址字段按 256 字节对齐解码，页池 4K 对齐满足） */
    paddr_t itt_pa = (paddr_t)(uintptr_t)allocate_page();
    if (!itt_pa) {
        WARN("ITS: failed to allocate ITT");
        return 0;
    }

    /* 3. MAPD：devid -> ITT，size = 3（16 个 event）
     *    dw1 = size[4:0]；dw2 = V[63] | ITT 地址[51:8] */
    its_send_cmd(ITS_CMD_MAPD | ((uint64_t)devid << 32),
                 ITS_ITT_SIZE_SHIFT,
                 (1ULL << 63) | (itt_pa & ~0xFFULL), 0);

    /* 4. MAPTI：eventid i -> LPI（pINTID >= 8192），collection 0
     *    dw1 = pINTID[63:32] | EventID[31:0]；dw2 = ICID[15:0]
     *    同时配置 LPI 属性表（索引 = LPI - 8192）：优先级 + 使能 */
    unsigned int lpi_base = next_lpi;
    for (unsigned int i = 0; i < num_vectors; i++) {
        unsigned int lpi = next_lpi++;

        /* LPI 配置表按 (LPI - 8192) 索引（QEMU gicv3_redist_check_lpi_priority） */
        volatile uint8_t *cfg =
            (volatile uint8_t *)(uintptr_t)lpi_prop_pa + (lpi - LPI_INTID_BASE);
        *cfg = LPI_PROP_PRIO | LPI_PROP_ENABLE;

        its_send_cmd(ITS_CMD_MAPTI | ((uint64_t)devid << 32),
                     ((uint64_t)lpi << 32) | i,
                     0, 0);
    }

    /* 5. SYNC（QEMU 忽略其语义——命令本身就是同步处理的，发一条收尾） */
    its_send_cmd(ITS_CMD_SYNC, 0, 0, 0);

    INFO("ITS: MSI map devid=%u vec=%u lpi=%u..%u -> cpu%u",
         devid, num_vectors, lpi_base, lpi_base + num_vectors - 1, target_cpu);
    return lpi_base;
}

/* ---------- 初始化 ---------- */

void its_init(void)
{
    uint64_t typer = get64(GITS_TYPER);

    /*
     * 1. 分配 LPI 表并安装到 CPU0 Redistributor。
     *
     * 分配顺序至关重要：
     *   - 挂起表必须 64KB 对齐（PENDBASER.PA 字段是 [51:16]，
     *     低 16 位被丢弃）。its_init 是页池的第一个使用者，
     *     页池首页 LOW_MEMORY(0x41000000) 恰好 64KB 对齐，
     *     所以挂起表必须最先分配。
     *   - 配置表 4KB 对齐即可（PROPBASER.PA 字段是 [51:12]）。
     */
    lpi_pend_pa = (paddr_t)(uintptr_t)allocate_page();
    lpi_prop_pa = (paddr_t)(uintptr_t)allocate_page();
    if (!lpi_pend_pa || !lpi_prop_pa) {
        PANIC("ITS: failed to allocate LPI tables");
        return;
    }
    if (lpi_pend_pa & 0xFFFF) {
        PANIC("ITS: pend table PA 0x%lx not 64KB-aligned",
              (unsigned long)lpi_pend_pa);
        return;
    }

    uint64_t gicr = gic_get_redist_base(0);
    put64(gicr + GICR_PENDBASER_OFF, lpi_pend_pa);
    put64(gicr + GICR_PROPBASER_OFF, lpi_prop_pa | 13);
    /*
     * PROPBASER.IDbits = 13：覆盖 LPI 0..16383（2^14）。
     *   - 配置表 4KB 索引 LPI 8192..12287（足够 edu 用）
     *   - 挂起表扫描范围 2048 字节（bit 8192..16383）
     */

    /* EnableLPIs：置位后 QEMU 会扫描挂起表重算 hpplpi */
    put32(gicr + GICR_CTLR_OFF, 0x1);

    /*
     * 2. 分配 ITS 的三张表 + 命令队列。
     *    allocate_page() 已把页面清零。
     */
    cmdq_pa = (paddr_t)(uintptr_t)allocate_page();
    dt_pa   = (paddr_t)(uintptr_t)allocate_page();
    ct_pa   = (paddr_t)(uintptr_t)allocate_page();
    if (!cmdq_pa || !dt_pa || !ct_pa) {
        PANIC("ITS: failed to allocate tables");
        return;
    }

    /*
     * 3. 编程 BASER0（设备表）/ BASER1（集合表）/ CBASER（命令队列）。
     *
     * 格式（QEMU FIELD 定义）：
     *   BASER:  V[63] | PHYADDR[47:12] | PAGESIZE[9:8] | SIZE[7:0]
     *   CBASER: V[63] | PHYADDR[51:12] | SIZE[7:0]
     *
     * 注意（QEMU 6.2 的 IMPDEF 行为）：
     *   - TYPE/ENTRYSIZE 位只读：写 0 保留复位值
     *     （BASER0=Device 表、BASER1=Collection 表、entry 8 字节）
     *   - PAGESIZE 复位为 64KB！必须显式写 0（4KB），
     *     否则地址按 [47:16]+[15:12]->[63:48] 的 64KB 布局解码
     *   - ITS 使能后 BASER/CBASER 变为只读，所以必须先编程后使能
     */
    put64(GITS_BASER(0), (1ULL << 63) | (uint64_t)dt_pa);
    put64(GITS_BASER(1), (1ULL << 63) | (uint64_t)ct_pa);
    put64(GITS_CBASER, (1ULL << 63) | (uint64_t)cmdq_pa);

    /* 4. 使能 ITS：QEMU 在此刻解析 BASER/CBASER 并处理残留命令 */
    put32(GITS_CTLR, 0x1);
    asm volatile("dsb sy" ::: "memory");

    if (!(get32(GITS_CTLR) & 0x1)) {
        WARN("ITS: failed to enable (CTLR=0x%x)", get32(GITS_CTLR));
        return;
    }

    INFO("ITS: TYPER=0x%lx", (unsigned long)typer);
    INFO("ITS: LPI tables PROP=0x%lx PEND=0x%lx (IDbits=13)",
         (unsigned long)lpi_prop_pa, (unsigned long)lpi_pend_pa);
    INFO("ITS: cmdq=0x%lx dt=0x%lx ct=0x%lx (enabled)",
         (unsigned long)cmdq_pa, (unsigned long)dt_pa, (unsigned long)ct_pa);
}
