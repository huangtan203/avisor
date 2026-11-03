// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - PCI/PCIe 直通实现 (Phase 7)
 *
 * 目标设备：QEMU edu 教育设备（vendor 0x1234 / device 0x11e8）
 *
 * 直通三步：
 *   1. 枚举（ECAM）：扫描 PCI 拓扑并分配 BAR；未接入 IOMMU 时仅
 *      使能所需的 I/O/Memory decode，保持 bus master 关闭。
 *   2. Stage 2 映射：把 ECAM（16MB）+ BAR0（1MB）用 device 属性
 *      映射进 VM 的 IPA 空间（等值映射，IPA == PA）。
 *   3. 中断：按 pin+slot swizzle 计算 edu INTx 落在主桥哪根 INTx 线、
 *      对应哪个 GIC SPI，路由到 CPU0 并使能；真正的"转发到虚拟中断"
 *      在 irq.c 的 handle_irq 里完成。
 */

#include "boards/virt/pci.h"
#include "boards/virt/base.h"
#include "boards/virt/gicv3.h"
#include "boards/virt/vgic.h"
#include "boards/virt/irq_domain.h"
#include "boards/virt/its.h"
#include "boards/virt/vits.h"
#include "common/task.h"
#include "arch/aarch64/mmu.h"
#include "arch/aarch64/sysregs.h"
#include "common/mm.h"
#include "common/delays.h"
#include "common/debug.h"
#include "common/utils.h"

/* PCI 配置空间寄存器偏移 */
#define PCI_CFG_VENDOR_ID  0x00
#define PCI_CFG_DEVICE_ID  0x02
#define PCI_CFG_STATUS     0x06
#define PCI_CFG_COMMAND    0x04
#define PCI_CFG_BAR0       0x10
#define PCI_CFG_INTR       0x3c  /* interrupt line[7:0] / pin[15:8] */
#define PCI_CFG_CAP_PTR    0x34  /* Capabilities Pointer */

/* Command 寄存器位 */
#define PCI_CMD_IO         (1 << 0)
#define PCI_CMD_MEMORY     (1 << 1)
#define PCI_CMD_BUSMASTER  (1 << 2)
#define PCI_CMD_INTX_DIS   (1 << 10) /* INTx Disable */

/* Status 寄存器位 */
#define PCI_STAT_CAP_LIST  (1 << 4)  /* Capabilities List */

/* Capability IDs */
#define PCI_CAP_ID_MSI     0x05
#define PCI_CAP_ID_MSIX    0x11
#define PCI_CAP_ID_EXP     0x10
#define PCI_EXP_DEVCAP     0x04
#define PCI_EXP_DEVCTL     0x08
#define PCI_EXP_DEVCAP_FLR (1U << 28)
#define PCI_EXP_DEVCTL_FLR (1U << 15)

/* MSI Capability 寄存器偏移（32 位消息地址模式） */
#define PCI_MSI_CTRL       0x02  /* Message Control（offset 相对于 cap 头部） */
#define PCI_MSI_ADDR_LO    0x04  /* Message Address（32位） */
#define PCI_MSI_DATA       0x08  /* Message Data（32位） */
#define PCI_MSI_MASK       0x0c  /* Mask Bits（可选，取决于 MSC） */
#define PCI_MSI_PEND       0x10
#define EDU_REG_LIVENESS   0x04
#define EDU_REG_FACTORIAL  0x08
#define EDU_REG_STATUS     0x20
#define EDU_REG_IRQ_STATUS 0x24
#define EDU_REG_DMA_SRC    0x80
#define EDU_REG_DMA_DST    0x88
#define EDU_REG_DMA_COUNT  0x90
#define EDU_REG_DMA_CMD    0x98
#define EDU_REG_IRQ_ACK    0x64

/* MSI Message Control 位 */
#define PCI_MSI_CTRL_EN          (1 << 0)   /* MSI Enable */
#define PCI_MSI_CTRL_MMC_SHIFT   1          /* Multiple Message Capable（位宽） */
#define PCI_MSI_CTRL_MME_SHIFT   4          /* Multiple Message Enable */
#define PCI_MSI_CTRL_64BIT       (1 << 7)   /* 64-bit Address */
#define PCI_MSI_CTRL_MASK        (1 << 8)   /* Per-vector Masking */
#define PCI_MSI_CTRL_MMC_MASK    (0x7 << PCI_MSI_CTRL_MMC_SHIFT)
#define PCI_MSI_CTRL_MME_MASK    (0x7 << PCI_MSI_CTRL_MME_SHIFT)
#define PCI_MSIX_CTRL            0x02
#define PCI_MSIX_CTRL_TABLE_SIZE 0x07ff
#define PCI_MSIX_CTRL_FUNC_MASK  (1U << 14)
#define PCI_MSIX_CTRL_ENABLE     (1U << 15)
#define PCI_MSIX_TABLE           0x04
#define PCI_MSIX_PBA             0x08

#define PCI_MAX_BUSES 16
#define PCI_MAX_DEVICES 128
#define PCI_MAX_BARS 6
#define PCI_HEADER_TYPE 0x0e
#define PCI_HEADER_MULTI 0x80
#define PCI_HEADER_BRIDGE 1
#define PCI_BAR0 0x10
#define PCI_BRIDGE_BUS_NUMBERS 0x18
#define PCI_BRIDGE_IO_BASE_LIMIT 0x1c
#define PCI_BRIDGE_MEMORY_BASE_LIMIT 0x20
#define PCI_BRIDGE_IO_BASE_UPPER 0x30
#define PCI_BRIDGE_IO_LIMIT_UPPER 0x34
#define PCI_BRIDGE_PREFETCH_BASE_LIMIT 0x24
#define PCI_BRIDGE_PREFETCH_BASE_UPPER 0x28
#define PCI_BRIDGE_PREFETCH_LIMIT_UPPER 0x2c
#define PCI_FREE_LIST_MAX 64
#define PCI_IO_ALLOC_END (VIRT_PCIE_IO_SIZE)
#define PCI_MMIO_ALLOC_END (VIRT_PCIE_MMIO_BASE + VIRT_PCIE_MMIO_SIZE)
#define PCI_BRIDGE_MEMORY_GRANULARITY 0x100000
#define PCI_BRIDGE_IO_GRANULARITY 0x1000
#define PCI_BRIDGE_HOTPLUG_BUS_RESERVE 4
#define PCI_ALLOC_FIRST_FIT 0
#define PCI_ALLOC_BEST_FIT 1
#define PCI_ALLOC_WORST_FIT 2
#define PCI_ALLOC_HIGH_BEST 3

static const char *pool_policy_name(unsigned int policy)
{
    switch (policy) {
    case PCI_ALLOC_FIRST_FIT: return "first-fit";
    case PCI_ALLOC_BEST_FIT: return "best-fit";
    case PCI_ALLOC_WORST_FIT: return "worst-fit";
    case PCI_ALLOC_HIGH_BEST: return "high-best";
    default: return "invalid";
    }
}

struct pci_bar_resource {
    uint64_t address;
    uint64_t size;
    uint32_t flags;
    uint8_t is_64bit;
    uint8_t is_io;
    uint8_t valid;
};

struct pci_free_range {
    uint64_t base;
    uint64_t size;
};

struct pci_resource_pool {
    struct pci_free_range ranges[PCI_FREE_LIST_MAX];
    unsigned int count;
    uint64_t base;
    uint64_t size;
    unsigned int policy;
};

struct bar_probe_entry {
    uint64_t size;
    uint32_t original;
    uint32_t original_hi;
    uint32_t mask;
    uint32_t mask_hi;
    uint8_t index;
    uint8_t is_64bit;
    uint8_t is_io;
};

struct pci_device_resource {
    uint16_t vendor;
    uint16_t device;
    uint8_t bus;
    uint8_t dev;
    uint8_t func;
    uint8_t header_type;
    uint8_t owner_vmid;
    uint8_t dma_owner_vmid;
    uint8_t dma_authorized;
    uint8_t primary_bus;
    uint8_t secondary_bus;
    uint8_t subordinate_bus;
    uint8_t msi_cap_off;
    uint8_t msi_is_64bit;
    uint8_t msix_cap_off;
    uint8_t msix_bir;
    uint8_t msix_pba_bir;
    uint8_t msi_mask_capable;
    uint16_t msi_ctrl;
    uint16_t msix_ctrl;
    uint16_t msix_max_vectors;
    uint64_t msi_addr;
    uint16_t msi_data;
    uint32_t msi_mask;
    uint32_t msi_pend;
    uint32_t msix_table_offset;
    uint32_t msix_pba_offset;
    uint32_t dma_generation;
    struct pci_bar_resource bars[PCI_MAX_BARS];
};

static struct pci_device_resource pci_devices[PCI_MAX_DEVICES];
static unsigned int pci_device_count;
static volatile unsigned int pci_dma_lock;

static unsigned long pci_dma_lock_acquire(void)
{
    unsigned long flags;
    unsigned int value, failed;

    asm volatile("mrs %0, daif\nmsr daifset, #2"
                 : "=r"(flags) :: "memory");
    for (;;) {
        asm volatile("1: ldaxr %w0, [%2]\n"
                     "cbnz %w0, 1b\n"
                     "mov %w0, #1\n"
                     "stxr %w1, %w0, [%2]\n"
                     "cbnz %w1, 1b\n"
                     : "=&r"(value), "=&r"(failed)
                     : "r"(&pci_dma_lock)
                     : "memory");
        if (!failed)
            return flags;
    }
}

static void pci_dma_lock_release(unsigned long flags)
{
    asm volatile("stlr wzr, [%0]\nmsr daif, %1"
                 :: "r"(&pci_dma_lock), "r"(flags) : "memory");
}

static struct pci_device_resource *pci_find_resource(unsigned int bus,
                                                      unsigned int dev,
                                                      unsigned int func);
static struct pci_resource_pool mmio_pool;
static struct pci_resource_pool io_pool;
static unsigned int pci_next_bus = 1;
static unsigned int edu_bus;
static unsigned int edu_dev;
static unsigned int edu_func;
static uint64_t edu_bar0;
static uint64_t edu_bar0_size;
static int edu_found = 0;

static unsigned int pci_edu_requester_id(void)
{
    return (edu_bus << 8) | (edu_dev << 3) | edu_func;
}

int pci_edu_owns_requester(unsigned int requester_id)
{
    return edu_found && requester_id == pci_edu_requester_id();
}

int pci_get_owned_config(struct task_struct *vm, unsigned int selector,
                         uint64_t *ecam, uint64_t *bar0,
                         uint64_t *bar0_size, unsigned int *requester_id)
{
    if (!vm || vm->vmid != 1 || selector != 0 || !edu_found ||
        !edu_bar0 || !edu_bar0_size)
        return -1;
    if (ecam)
        *ecam = VIRT_PCIE_ECAM_BASE + ((uint64_t)edu_bus << 20) +
                ((uint64_t)edu_dev << 15) + ((uint64_t)edu_func << 12);
    if (bar0)
        *bar0 = edu_bar0;
    if (bar0_size)
        *bar0_size = edu_bar0_size;
    if (requester_id)
        *requester_id = pci_edu_requester_id();
    return 0;
}

/*
 * edu INTx 的 GIC 全局中断号（运行时计算，pci_init 填充）。
 *
 * QEMU virt 的 gpex 主桥用标准 PCI swizzle 把设备 INTx 旋到主桥引脚：
 *   host_pin = (device_pin + slot) & 3     （device_pin: INTA=0...INTD=3）
 * 主桥 INTA/B/C/D 分别接 GIC SPI 3/4/5/6。edu 通常枚举到 slot 2，
 * 其 INTA（device_pin=0）旋到主桥 INTC（host_pin=2）→ SPI 5 → 全局 37。
 * 0 表示 edu 未找到（不转发中断）。
 */
unsigned int pci_edu_intx_irq = 0;

/*
 * edu MSI 相关变量（Phase 8）
 */
static int edu_msi_cap_off = 0;   /* MSI Capability 在配置空间中的偏移（0=不存在） */
static int edu_pcie_cap_off = 0;   /* PCIe Capability 偏移（0=不存在） */
static int edu_flr_supported = 0;
static unsigned int edu_msi_max_vectors = 1;
unsigned int pci_edu_lpi_base = 0;  /* edu MSI 对应的 LPI 基址（0=未配置） */
unsigned int pci_edu_msi_enabled = 0; /* MSI 是否已启用 */

/*
 * ECAM 配置空间地址计算
 *   addr = ECAM_BASE + (bus << 20 | dev << 15 | func << 12 | reg)
 */
static inline uint64_t ecam_addr(unsigned int bus, unsigned int dev,
                                 unsigned int func, unsigned int reg)
{
    return VIRT_PCIE_ECAM_BASE +
           (((uint64_t)bus << 20) | ((uint64_t)dev << 15) |
            ((uint64_t)func << 12) | (uint64_t)reg);
}

static uint32_t pci_config_read(unsigned int bus, unsigned int dev,
                                unsigned int func, unsigned int reg)
{
    return get32(ecam_addr(bus, dev, func, reg));
}

static void pci_config_write(unsigned int bus, unsigned int dev,
                             unsigned int func, unsigned int reg, uint32_t val)
{
    put32(ecam_addr(bus, dev, func, reg), val);
}

static uint32_t pci_command_dma_safe(uint32_t command)
{
    return command & ~PCI_CMD_BUSMASTER;
}

static uint32_t pci_dma_get_generation(struct pci_device_resource *resource)
{
    unsigned long flags = pci_dma_lock_acquire();
    uint32_t generation = resource->dma_generation;
    pci_dma_lock_release(flags);
    return generation;
}

static int pci_dma_set_authorized(struct pci_device_resource *resource,
                                  unsigned int owner_vmid, int authorized,
                                  uint32_t expected_generation)
{
    if (!resource || !resource->owner_vmid ||
        owner_vmid != resource->owner_vmid)
        return -1;
    unsigned long flags = pci_dma_lock_acquire();
    if (resource->dma_generation != expected_generation ||
        !!resource->dma_authorized == !!authorized) {
        uint32_t current_generation = resource->dma_generation;
        unsigned int state = resource->dma_authorized;
        pci_dma_lock_release(flags);
        WARN("PCI: rejected stale DMA transition %02x:%02x.%u expected=%u current=%u state=%u target=%u",
             resource->bus, resource->dev, resource->func,
             expected_generation, current_generation, state, !!authorized);
        return -1;
    }
    if (resource->dma_generation == UINT32_MAX) {
        pci_dma_lock_release(flags);
        WARN("PCI: DMA generation exhausted for %02x:%02x.%u",
             resource->bus, resource->dev, resource->func);
        return -1;
    }
    uint32_t command = pci_config_read(resource->bus, resource->dev,
                                       resource->func, PCI_CFG_COMMAND);
    command = pci_command_dma_safe(command);
    if (authorized)
        command |= PCI_CMD_BUSMASTER;
    pci_config_write(resource->bus, resource->dev, resource->func,
                     PCI_CFG_COMMAND, command);
    uint32_t verify = pci_config_read(resource->bus, resource->dev,
                                      resource->func, PCI_CFG_COMMAND);
    if (!!(verify & PCI_CMD_BUSMASTER) != !!authorized) {
        pci_dma_lock_release(flags);
        return -1;
    }
    resource->dma_owner_vmid = authorized ? owner_vmid : 0;
    resource->dma_authorized = !!authorized;
    resource->dma_generation++;
    uint32_t generation = resource->dma_generation;
    pci_dma_lock_release(flags);
    INFO("PCI: DMA %s %02x:%02x.%u owner=VM%u generation=%u",
         authorized ? "authorized" : "revoked", resource->bus,
         resource->dev, resource->func, owner_vmid, generation);
    return 0;
}

static void pool_init(struct pci_resource_pool *pool, uint64_t base,
                      uint64_t size, unsigned int policy)
{
    memzero(pool, sizeof(*pool));
    pool->base = base;
    pool->size = size;
    pool->policy = policy;
    pool->ranges[0] = (struct pci_free_range) { base, size };
    pool->count = size ? 1 : 0;
}

static int pool_range_valid(const struct pci_resource_pool *pool,
                            uint64_t base, uint64_t size)
{
    if (!pool || !size || base < pool->base ||
        base > UINT64_MAX - size || size > pool->size ||
        base - pool->base > pool->size - size)
        return 0;
    return 1;
}

static int pool_split(struct pci_resource_pool *pool, unsigned int index,
                      uint64_t base, uint64_t size)
{
    struct pci_free_range range = pool->ranges[index];
    if (!pool_range_valid(pool, range.base, range.size) ||
        base < range.base || base > UINT64_MAX - size ||
        size > range.base + range.size - base)
        return -1;
    uint64_t end = range.base + range.size;
    uint64_t alloc_end = base + size;
    struct pci_free_range left = { range.base, base - range.base };
    struct pci_free_range right = { alloc_end, end - alloc_end };
    unsigned int pieces = !!left.size + !!right.size;
    if (pool->count - 1 + pieces > PCI_FREE_LIST_MAX)
        return -1;
    for (unsigned int i = index; i + 1 < pool->count; i++)
        pool->ranges[i] = pool->ranges[i + 1];
    pool->count--;
    struct pci_free_range split[2] = { left, right };
    for (unsigned int p = 0; p < 2; p++) {
        if (!split[p].size)
            continue;
        unsigned int pos = 0;
        while (pos < pool->count && pool->ranges[pos].base < split[p].base)
            pos++;
        for (unsigned int i = pool->count; i > pos; i--)
            pool->ranges[i] = pool->ranges[i - 1];
        pool->ranges[pos] = split[p];
        pool->count++;
    }
    return 0;
}

static int pool_alloc(struct pci_resource_pool *pool, uint64_t size,
                      uint64_t *address)
{
    if (!pool || !address || !size || (size & (size - 1)) ||
        pool->count > PCI_FREE_LIST_MAX ||
        pool->policy > PCI_ALLOC_HIGH_BEST ||
        !pool_range_valid(pool, pool->base, pool->size))
        return -1;
    int best = -1;
    uint64_t best_waste = pool->policy == PCI_ALLOC_WORST_FIT ? 0 : UINT64_MAX;
    uint64_t best_base = 0;
    for (unsigned int i = 0; i < pool->count; i++) {
        struct pci_free_range *range = &pool->ranges[i];
        if (!pool_range_valid(pool, range->base, range->size))
            return -1;
        uint64_t end = range->base + range->size;
        if (range->base > UINT64_MAX - (size - 1))
            continue;
        uint64_t candidate = pool->policy == PCI_ALLOC_HIGH_BEST ?
            (end >= size ? (end - size) & ~(size - 1) : 0) :
            (range->base + size - 1) & ~(size - 1);
        if (candidate < range->base || candidate > UINT64_MAX - size ||
            size > end - candidate)
            continue;
        uint64_t waste = range->size - size;
        if (pool->policy == PCI_ALLOC_FIRST_FIT) {
            best = i;
            best_base = candidate;
            break;
        }
        if ((pool->policy == PCI_ALLOC_WORST_FIT && waste > best_waste) ||
            (pool->policy != PCI_ALLOC_WORST_FIT && waste < best_waste)) {
            best = i;
            best_waste = waste;
            best_base = candidate;
        }
    }
    if (best < 0 || pool_split(pool, best, best_base, size) < 0)
        return -1;
    *address = best_base;
    return 0;
}

static int pool_free(struct pci_resource_pool *pool, uint64_t base,
                     uint64_t size)
{
    if (!pool_range_valid(pool, base, size) ||
        pool->count > PCI_FREE_LIST_MAX)
        return -1;
    for (unsigned int i = 0; i < pool->count; i++)
        if (!pool_range_valid(pool, pool->ranges[i].base,
                              pool->ranges[i].size) ||
            (i && pool->ranges[i - 1].base + pool->ranges[i - 1].size >=
                  pool->ranges[i].base))
            return -1;
    uint64_t end = base + size;
    unsigned int pos = 0;
    while (pos < pool->count && pool->ranges[pos].base < base) {
        uint64_t range_end = pool->ranges[pos].base + pool->ranges[pos].size;
        if (range_end > base)
            return -1;
        pos++;
    }
    if (pos < pool->count && end > pool->ranges[pos].base)
        return -1;
    int merge_prev = pos &&
        pool->ranges[pos - 1].base + pool->ranges[pos - 1].size == base;
    int merge_next = pos < pool->count && end == pool->ranges[pos].base;
    if (merge_prev && merge_next) {
        pool->ranges[pos - 1].size += size + pool->ranges[pos].size;
        for (unsigned int i = pos; i + 1 < pool->count; i++)
            pool->ranges[i] = pool->ranges[i + 1];
        pool->count--;
    } else if (merge_prev) {
        pool->ranges[pos - 1].size += size;
    } else if (merge_next) {
        pool->ranges[pos].base = base;
        pool->ranges[pos].size += size;
    } else {
        if (pool->count == PCI_FREE_LIST_MAX)
            return -1;
        for (unsigned int i = pool->count; i > pos; i--)
            pool->ranges[i] = pool->ranges[i - 1];
        pool->ranges[pos] = (struct pci_free_range) { base, size };
        pool->count++;
    }
    return 0;
}

static int pool_reserve_range(struct pci_resource_pool *pool, uint64_t base,
                              uint64_t size)
{
    if (!pool_range_valid(pool, base, size) ||
        pool->count > PCI_FREE_LIST_MAX)
        return -1;
    struct pci_resource_pool updated = *pool;
    struct pci_resource_pool *original = pool;
    pool = &updated;
    uint64_t end = base + size;
    for (unsigned int i = 0; i < pool->count;) {
        struct pci_free_range *range = &pool->ranges[i];
        uint64_t range_end;
        if (!range->size || range->base > UINT64_MAX - range->size)
            return -1;
        range_end = range->base + range->size;
        if (range_end <= base) {
            i++;
            continue;
        }
        if (range->base >= end)
            break;
        if (range->base > pool->base + pool->size ||
            range_end > pool->base + pool->size)
            return -1;
        if (base <= range->base && end >= range_end) {
            for (unsigned int j = i; j + 1 < pool->count; j++)
                pool->ranges[j] = pool->ranges[j + 1];
            pool->count--;
            continue;
        }
        if (base <= range->base) {
            range->base = end;
            range->size = range_end - end;
            i++;
            continue;
        }
        if (end >= range_end) {
            range->size = base - range->base;
            i++;
            continue;
        }
        if (pool->count == PCI_FREE_LIST_MAX)
            return -1;
        for (unsigned int j = pool->count; j > i + 1; j--)
            pool->ranges[j] = pool->ranges[j - 1];
        pool->ranges[i + 1] = (struct pci_free_range) {
            end, range_end - end
        };
        range->size = base - range->base;
        pool->count++;
        i += 2;
    }
    *original = updated;
    return 0;
}

static uint64_t bridge_window_limit(struct pci_resource_pool *pool,
                                    uint64_t base, uint64_t limit,
                                    uint64_t granularity,
                                    uint64_t minimum_reserve)
{
    uint64_t span = limit - base + granularity;
    uint64_t reserve = span / 4;
    if (reserve < minimum_reserve)
        reserve = minimum_reserve;
    uint64_t end = limit + granularity;
    if (reserve > UINT64_MAX - end)
        return limit;
    end += reserve;
    if (end > UINT64_MAX - (granularity - 1))
        return limit;
    end = (end + granularity - 1) & ~(granularity - 1);
    uint64_t available_end = limit + granularity;
    for (unsigned int i = 0; i < pool->count; i++)
        if (pool->ranges[i].base == available_end) {
            uint64_t range_end = pool->ranges[i].base + pool->ranges[i].size;
            if (end > range_end)
                end = range_end & ~(granularity - 1);
            break;
        }
    return end >= base + granularity ? end - granularity : limit;
}

static uint64_t pool_used(const struct pci_resource_pool *pool)
{
    uint64_t free_size = 0;
    if (!pool || pool->count > PCI_FREE_LIST_MAX)
        return 0;
    for (unsigned int i = 0; i < pool->count; i++) {
        if (!pool_range_valid(pool, pool->ranges[i].base,
                              pool->ranges[i].size))
            return 0;
        free_size += pool->ranges[i].size;
    }
    if (free_size > pool->size)
        return 0;
    return pool->size - free_size;
}

static int pool_selftest(void)
{
    struct pci_resource_pool pool;
    uint64_t first, second;
    pool_init(&pool, 0x1000, 0x10000, PCI_ALLOC_BEST_FIT);
    if (pool_alloc(&pool, 0x1000, &first) < 0 || first != 0x1000 ||
        pool_alloc(&pool, 0x2000, &second) < 0 || second != 0x2000 ||
        pool_free(&pool, first, 0x1000) < 0 ||
        pool_free(&pool, second, 0x2000) < 0 || pool.count != 1 ||
        pool.ranges[0].base != 0x1000 || pool.ranges[0].size != 0x10000)
        return -1;
    if (pool_reserve_range(&pool, 0x3000, 0x2000) < 0 || pool.count != 2 ||
        pool.ranges[0].base != 0x1000 || pool.ranges[0].size != 0x2000 ||
        pool.ranges[1].base != 0x5000 || pool.ranges[1].size != 0xc000)
        return -1;
    if (pool_reserve_range(&pool, 0x2000, 0x6000) < 0 || pool.count != 2 ||
        pool.ranges[0].base != 0x1000 || pool.ranges[0].size != 0x1000 ||
        pool.ranges[1].base != 0x8000 || pool.ranges[1].size != 0x9000)
        return -1;

    pool_init(&pool, 0x1000, 0x10000, PCI_ALLOC_HIGH_BEST);
    if (pool_alloc(&pool, 0x1000, &first) < 0 ||
        first != 0x10000 || pool.count != 1 ||
        pool.ranges[0].base != 0x1000 || pool.ranges[0].size != 0xf000 ||
        pool_free(&pool, first, 0x1000) < 0 || pool.count != 1 ||
        pool.ranges[0].base != 0x1000 || pool.ranges[0].size != 0x10000)
        return -1;

    pool_init(&pool, 0x1000, 0x10000, PCI_ALLOC_FIRST_FIT);
    if (pool_reserve_range(&pool, 0x3000, 0x2000) < 0 ||
        pool_alloc(&pool, 0x1000, &first) < 0 || first != 0x1000)
        return -1;
    pool_init(&pool, 0x1000, 0x10000, PCI_ALLOC_BEST_FIT);
    if (pool_reserve_range(&pool, 0x3000, 0x2000) < 0 ||
        pool_alloc(&pool, 0x1000, &first) < 0 || first != 0x1000)
        return -1;
    pool_init(&pool, 0x1000, 0x10000, PCI_ALLOC_WORST_FIT);
    if (pool_reserve_range(&pool, 0x3000, 0x2000) < 0 ||
        pool_alloc(&pool, 0x1000, &first) < 0 || first != 0x5000)
        return -1;
    return 0;
}

static uint64_t pool_largest_free(const struct pci_resource_pool *pool)
{
    uint64_t largest = 0;
    if (!pool || pool->count > PCI_FREE_LIST_MAX)
        return 0;
    for (unsigned int i = 0; i < pool->count; i++)
        if (pool->ranges[i].size > largest)
            largest = pool->ranges[i].size;
    return largest;
}

static void pool_dump_stats(const char *name,
                            const struct pci_resource_pool *pool)
{
    INFO("PCI: %s policy=%s total=0x%lx used=0x%lx free=0x%lx fragments=%u largest=0x%lx base=0x%lx",
         name, pool_policy_name(pool->policy), (unsigned long)pool->size,
         (unsigned long)pool_used(pool),
         (unsigned long)(pool->size - pool_used(pool)), pool->count,
         (unsigned long)pool_largest_free(pool),
         (unsigned long)pool->base);
}

static int pci_allocate_mmio(uint64_t size, int is_64bit,
                             uint64_t *address)
{
    unsigned int policy = mmio_pool.policy;
    if (is_64bit)
        mmio_pool.policy = PCI_ALLOC_HIGH_BEST;
    int ret = pool_alloc(&mmio_pool, size, address);
    mmio_pool.policy = policy;
    return ret;
}

static int pci_allocate_io(uint32_t size, uint32_t *port)
{
    uint64_t address;
    if (pool_alloc(&io_pool, size, &address) < 0)
        return -1;
    *port = (uint32_t)address;
    return 0;
}

static int bar_size_compare_desc(const struct bar_probe_entry *a,
                                 const struct bar_probe_entry *b)
{
    if (a->size != b->size)
        return a->size > b->size;
    return a->is_64bit > b->is_64bit;
}

static void bar_probes_sort(struct bar_probe_entry *entries,
                            unsigned int count)
{
    for (unsigned int i = 1; i < count; i++) {
        struct bar_probe_entry entry = entries[i];
        unsigned int j = i;
        while (j && bar_size_compare_desc(&entry, &entries[j - 1])) {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = entry;
    }
}

static int pci_probe_bars(struct pci_device_resource *resource,
                          unsigned int bar_count)
{
    uint32_t command = pci_config_read(resource->bus, resource->dev,
                                       resource->func, PCI_CFG_COMMAND);
    uint32_t original_bars[PCI_MAX_BARS];
    struct pci_resource_pool saved_mmio_pool = mmio_pool;
    struct pci_resource_pool saved_io_pool = io_pool;
    uint32_t original_bars_hi[PCI_MAX_BARS] = { 0 };
    struct pci_bar_resource saved_resources[PCI_MAX_BARS];
    struct bar_probe_entry entries[PCI_MAX_BARS];
    unsigned int entry_count = 0;
    memzero(entries, sizeof(entries));
    for (unsigned int i = 0; i < bar_count; i++) {
        original_bars[i] = pci_config_read(resource->bus, resource->dev,
                                           resource->func, PCI_BAR0 + i * 4);
        if (!(original_bars[i] & 1) &&
            (((original_bars[i] >> 1) & 3) == 2) && i + 1 < bar_count)
            original_bars_hi[i] = pci_config_read(
                resource->bus, resource->dev, resource->func,
                PCI_BAR0 + (i + 1) * 4);
        saved_resources[i] = resource->bars[i];
    }

    pci_config_write(resource->bus, resource->dev, resource->func,
                     PCI_CFG_COMMAND, command & ~(PCI_CMD_IO | PCI_CMD_MEMORY));
    for (unsigned int i = 0; i < bar_count; i++) {
        unsigned int reg = PCI_BAR0 + i * 4;
        uint32_t original = pci_config_read(resource->bus, resource->dev,
                                            resource->func, reg);
        uint32_t original_hi = 0;
        int is_64bit = !(original & 1) && (((original >> 1) & 3) == 2) &&
                      i + 1 < bar_count;
        if (is_64bit)
            original_hi = pci_config_read(resource->bus, resource->dev,
                                          resource->func, reg + 4);
        pci_config_write(resource->bus, resource->dev, resource->func, reg,
                         0xffffffff);
        if (is_64bit)
            pci_config_write(resource->bus, resource->dev, resource->func,
                             reg + 4, 0xffffffff);
        uint32_t mask = pci_config_read(resource->bus, resource->dev,
                                        resource->func, reg);
        uint32_t mask_hi = is_64bit ? pci_config_read(
            resource->bus, resource->dev, resource->func, reg + 4) : 0;
        pci_config_write(resource->bus, resource->dev, resource->func, reg,
                         original);
        if (is_64bit)
            pci_config_write(resource->bus, resource->dev, resource->func,
                             reg + 4, original_hi);
        if (!mask || mask == 0xffffffff) {
            if (original || (is_64bit && original_hi))
                goto rollback;
            continue;
        }

        uint64_t size;
        int is_io = original & 1;
        if (is_io) {
            size = (uint32_t)(~(mask & ~3U) + 1U);
        } else if (is_64bit) {
            uint64_t mask64 = ((uint64_t)mask_hi << 32) | (mask & ~0xfULL);
            size = ~mask64 + 1;
            if (size > UINT32_MAX)
                goto rollback;
        } else {
            size = (uint32_t)(~(mask & ~0xfU) + 1U);
        }
        if (!size || (size & (size - 1)))
            goto rollback;
        entries[entry_count++] = (struct bar_probe_entry) {
            .size = size,
            .original = original,
            .original_hi = original_hi,
            .mask = mask,
            .mask_hi = mask_hi,
            .index = i,
            .is_64bit = is_64bit,
            .is_io = is_io,
        };
        if (is_64bit)
            i++;
    }

    bar_probes_sort(entries, entry_count);
    for (unsigned int n = 0; n < entry_count; n++) {
        struct bar_probe_entry *probe = &entries[n];
        unsigned int i = probe->index;
        unsigned int reg = PCI_BAR0 + i * 4;
        struct pci_bar_resource *bar = &resource->bars[i];
        bar->flags = probe->original & 0xf;
        bar->is_64bit = probe->is_64bit;
        bar->is_io = probe->is_io;
        bar->size = probe->size;
        if (probe->is_io) {
            uint32_t port;
            if (pci_allocate_io(probe->size, &port) < 0) {
                WARN("PCI: I/O BAR allocation failed at %02x:%02x.%u BAR%u size=0x%lx",
                     resource->bus, resource->dev, resource->func, i,
                     (unsigned long)probe->size);
                goto rollback;
            }
            bar->address = VIRT_PCIE_IO_BASE + port;
            bar->valid = 1;
            pci_config_write(resource->bus, resource->dev, resource->func,
                             reg, port | (probe->original & 3));
            INFO("PCI: %02x:%02x.%u BAR%u I/O port=0x%x CPU=0x%lx/0x%lx",
                 resource->bus, resource->dev, resource->func, i, port,
                 (unsigned long)bar->address, (unsigned long)bar->size);
            continue;
        }
        uint64_t assigned;
        if (pci_allocate_mmio(probe->size, probe->is_64bit, &assigned) < 0) {
            WARN("PCI: MMIO BAR allocation failed at %02x:%02x.%u BAR%u size=0x%lx",
                 resource->bus, resource->dev, resource->func, i,
                 (unsigned long)probe->size);
            goto rollback;
        }
        bar->address = assigned;
        bar->valid = 1;
        pci_config_write(resource->bus, resource->dev, resource->func, reg,
                         (uint32_t)assigned | bar->flags);
        if (probe->is_64bit)
            pci_config_write(resource->bus, resource->dev, resource->func,
                             reg + 4, (uint32_t)(assigned >> 32));
        INFO("PCI: %02x:%02x.%u BAR%u MMIO 0x%lx/0x%lx",
             resource->bus, resource->dev, resource->func, i,
             (unsigned long)assigned, (unsigned long)probe->size);
    }
    pci_config_write(resource->bus, resource->dev, resource->func,
                     PCI_CFG_COMMAND, command);
    return 0;

rollback:
    for (unsigned int i = 0; i < bar_count; i++) {
        pci_config_write(resource->bus, resource->dev, resource->func,
                         PCI_BAR0 + i * 4, original_bars[i]);
        if (!(original_bars[i] & 1) &&
            (((original_bars[i] >> 1) & 3) == 2) && i + 1 < bar_count)
            pci_config_write(resource->bus, resource->dev, resource->func,
                             PCI_BAR0 + (i + 1) * 4, original_bars_hi[i]);
        struct pci_bar_resource *bar = &resource->bars[i];
        if (bar->valid && bar->address >= VIRT_PCIE_IO_BASE &&
            bar->is_io) {
            if (pool_free(&io_pool, bar->address - VIRT_PCIE_IO_BASE,
                          bar->size) < 0)
                WARN("PCI: failed to free I/O BAR during rollback");
        } else if (bar->valid && !bar->is_io) {
            if (pool_free(&mmio_pool, bar->address, bar->size) < 0)
                WARN("PCI: failed to free MMIO BAR during rollback");
        }
        resource->bars[i] = saved_resources[i];
    }
    mmio_pool = saved_mmio_pool;
    io_pool = saved_io_pool;
    pci_config_write(resource->bus, resource->dev, resource->func,
                     PCI_CFG_COMMAND, command);
    WARN("PCI: BAR transaction rolled back for %02x:%02x.%u",
         resource->bus, resource->dev, resource->func);
    return -1;
}

struct pci_bridge_config {
    uint32_t bus_numbers;
    uint32_t io_base_limit;
    uint32_t io_upper;
    uint32_t memory_base_limit;
    uint32_t prefetch_base_limit;
    uint32_t prefetch_base_upper;
    uint32_t prefetch_limit_upper;
};

static struct pci_bridge_config pci_bridge_config_read(unsigned int bus,
                                                        unsigned int dev,
                                                        unsigned int func)
{
    struct pci_bridge_config config = {
        .bus_numbers = pci_config_read(bus, dev, func,
                                       PCI_BRIDGE_BUS_NUMBERS),
        .io_base_limit = pci_config_read(bus, dev, func,
                                         PCI_BRIDGE_IO_BASE_LIMIT),
        .io_upper = pci_config_read(bus, dev, func,
                                    PCI_BRIDGE_IO_BASE_UPPER),
        .memory_base_limit = pci_config_read(bus, dev, func,
                                             PCI_BRIDGE_MEMORY_BASE_LIMIT),
        .prefetch_base_limit = pci_config_read(
            bus, dev, func, PCI_BRIDGE_PREFETCH_BASE_LIMIT),
        .prefetch_base_upper = pci_config_read(
            bus, dev, func, PCI_BRIDGE_PREFETCH_BASE_UPPER),
        .prefetch_limit_upper = pci_config_read(
            bus, dev, func, PCI_BRIDGE_PREFETCH_LIMIT_UPPER),
    };
    return config;
}

static void pci_bridge_config_write(unsigned int bus, unsigned int dev,
                                    unsigned int func,
                                    const struct pci_bridge_config *config)
{
    pci_config_write(bus, dev, func, PCI_BRIDGE_BUS_NUMBERS,
                     config->bus_numbers);
    pci_config_write(bus, dev, func, PCI_BRIDGE_IO_BASE_LIMIT,
                     config->io_base_limit);
    pci_config_write(bus, dev, func, PCI_BRIDGE_IO_BASE_UPPER,
                     config->io_upper);
    pci_config_write(bus, dev, func, PCI_BRIDGE_MEMORY_BASE_LIMIT,
                     config->memory_base_limit);
    pci_config_write(bus, dev, func, PCI_BRIDGE_PREFETCH_BASE_LIMIT,
                     config->prefetch_base_limit);
    pci_config_write(bus, dev, func, PCI_BRIDGE_PREFETCH_BASE_UPPER,
                     config->prefetch_base_upper);
    pci_config_write(bus, dev, func, PCI_BRIDGE_PREFETCH_LIMIT_UPPER,
                     config->prefetch_limit_upper);
}

static int pci_bridge_config_matches(unsigned int bus, unsigned int dev,
                                     unsigned int func,
                                     const struct pci_bridge_config *expected)
{
    struct pci_bridge_config actual = pci_bridge_config_read(bus, dev, func);
    int matches = (actual.bus_numbers & 0x00ffffff) ==
                      (expected->bus_numbers & 0x00ffffff) &&
                  (actual.io_base_limit & 0xf0f0) ==
                      (expected->io_base_limit & 0xf0f0) &&
                  actual.io_upper == expected->io_upper &&
                  (actual.memory_base_limit & 0xfff0fff0) ==
                      (expected->memory_base_limit & 0xfff0fff0) &&
                  (actual.prefetch_base_limit & 0xfff1fff1) ==
                      (expected->prefetch_base_limit & 0xfff1fff1) &&
                  actual.prefetch_base_upper ==
                      expected->prefetch_base_upper &&
                  actual.prefetch_limit_upper ==
                      expected->prefetch_limit_upper;
    if (!matches)
        WARN("PCI: bridge %02x:%02x.%u config expected=%x/%x/%x/%x/%x/%x/%x actual=%x/%x/%x/%x/%x/%x/%x",
             bus, dev, func, expected->bus_numbers, expected->io_base_limit,
             expected->io_upper, expected->memory_base_limit,
             expected->prefetch_base_limit, expected->prefetch_base_upper,
             expected->prefetch_limit_upper, actual.bus_numbers,
             actual.io_base_limit, actual.io_upper, actual.memory_base_limit,
             actual.prefetch_base_limit, actual.prefetch_base_upper,
             actual.prefetch_limit_upper);
    return matches;
}

struct pci_bridge_window {
    uint64_t base;
    uint64_t limit;
    uint8_t enabled;
};

static struct pci_bridge_window pci_bridge_io_window(
    const struct pci_bridge_config *config)
{
    uint64_t base = ((uint64_t)(config->io_base_limit & 0xf0) << 8) |
                    ((uint64_t)(config->io_upper & 0xffff) << 16);
    uint64_t limit = (config->io_base_limit & 0xf000) |
                     (config->io_upper & 0xffff0000ULL);
    struct pci_bridge_window window = {
        .base = base,
        .limit = limit | (PCI_BRIDGE_IO_GRANULARITY - 1),
        .enabled = base <= limit,
    };
    return window;
}

static struct pci_bridge_window pci_bridge_memory_window(uint32_t value)
{
    uint64_t base = (uint64_t)(value & 0xfff0) << 16;
    uint64_t limit = (uint64_t)(value & 0xfff00000);
    struct pci_bridge_window window = {
        .base = base,
        .limit = limit | (PCI_BRIDGE_MEMORY_GRANULARITY - 1),
        .enabled = base <= limit,
    };
    return window;
}

static struct pci_bridge_window pci_bridge_prefetch_window(
    const struct pci_bridge_config *config)
{
    uint64_t base = ((uint64_t)config->prefetch_base_upper << 32) |
                    ((uint64_t)(config->prefetch_base_limit & 0xfff0) << 16);
    uint64_t limit = ((uint64_t)config->prefetch_limit_upper << 32) |
                     (config->prefetch_base_limit & 0xfff00000ULL);
    struct pci_bridge_window window = {
        .base = base,
        .limit = limit | (PCI_BRIDGE_MEMORY_GRANULARITY - 1),
        .enabled = base <= limit,
    };
    return window;
}

static int pci_bar_in_window(const struct pci_bar_resource *bar,
                             const struct pci_bridge_window *window)
{
    uint64_t base = bar->is_io ?
        bar->address - VIRT_PCIE_IO_BASE : bar->address;
    if (!window->enabled || !bar->size || base > UINT64_MAX - bar->size)
        return 0;
    return base >= window->base && base + bar->size - 1 <= window->limit;
}

static int pci_dma_guard_init(void)
{
    unsigned long flags = pci_dma_lock_acquire();
    unsigned int guarded = 0;
    for (unsigned int i = 0; i < pci_device_count; i++) {
        struct pci_device_resource *resource = &pci_devices[i];
        uint32_t command = pci_config_read(resource->bus, resource->dev,
                                           resource->func,
                                           PCI_CFG_COMMAND);
        command = pci_command_dma_safe(command);
        if (resource->owner_vmid)
            command |= PCI_CMD_MEMORY;
        pci_config_write(resource->bus, resource->dev, resource->func,
                         PCI_CFG_COMMAND, command);
        resource->dma_owner_vmid = 0;
        resource->dma_authorized = 0;
        resource->dma_generation = 1;
        uint32_t verify = pci_config_read(resource->bus, resource->dev,
                                          resource->func,
                                          PCI_CFG_COMMAND);
        if (verify & PCI_CMD_BUSMASTER) {
            pci_dma_lock_release(flags);
            WARN("PCI: failed to disable bus mastering for %02x:%02x.%u",
                 resource->bus, resource->dev, resource->func);
            return -1;
        }
        guarded++;
    }
    pci_dma_lock_release(flags);
    INFO("PCI: DMA guard active, bus mastering disabled for %u functions",
         guarded);
    return 0;
}

static int pci_dma_state_audit_locked(void)
{
    unsigned int authorized = 0;
    for (unsigned int i = 0; i < pci_device_count; i++) {
        struct pci_device_resource *resource = &pci_devices[i];
        uint32_t command = pci_config_read(resource->bus, resource->dev,
                                           resource->func,
                                           PCI_CFG_COMMAND);
        int physical = !!(command & PCI_CMD_BUSMASTER);
        if (!resource->dma_generation ||
            physical != resource->dma_authorized ||
            (resource->dma_authorized &&
             resource->dma_owner_vmid != resource->owner_vmid) ||
            (!resource->dma_authorized && resource->dma_owner_vmid)) {
            WARN("PCI: DMA state mismatch %02x:%02x.%u physical=%u authorized=%u owner=%u/%u generation=%u",
                 resource->bus, resource->dev, resource->func, physical,
                 resource->dma_authorized, resource->dma_owner_vmid,
                 resource->owner_vmid, resource->dma_generation);
            return -1;
        }
        authorized += resource->dma_authorized;
    }
    INFO("PCI: DMA state audit PASSED functions=%u authorized=%u",
         pci_device_count, authorized);
    return 0;
}

static int pci_dma_state_audit(void)
{
    unsigned long flags = pci_dma_lock_acquire();
    int ret = pci_dma_state_audit_locked();
    pci_dma_lock_release(flags);
    return ret;
}

static int pci_bar_resource_audit(void)
{
    unsigned int bar_count = 0;
    for (unsigned int i = 0; i < pci_device_count; i++) {
        struct pci_device_resource *resource = &pci_devices[i];
        for (unsigned int b = 0; b < PCI_MAX_BARS; b++) {
            struct pci_bar_resource *bar = &resource->bars[b];
            if (!bar->valid)
                continue;
            unsigned int reg = PCI_BAR0 + b * 4;
            uint32_t low = pci_config_read(resource->bus, resource->dev,
                                           resource->func, reg);
            int is_io = !!(low & 1);
            int is_64bit = !is_io && (((low >> 1) & 3) == 2);
            uint64_t address = is_io ?
                VIRT_PCIE_IO_BASE + (low & ~3U) : (low & ~0xfU);
            if (is_64bit) {
                if (b + 1 >= PCI_MAX_BARS) {
                    WARN("PCI: %02x:%02x.%u BAR%u invalid 64-bit pair",
                         resource->bus, resource->dev, resource->func, b);
                    return -1;
                }
                address |= (uint64_t)pci_config_read(
                    resource->bus, resource->dev, resource->func, reg + 4)
                    << 32;
            }
            uint32_t flag_mask = is_io ? 3U : 0xfU;
            if (is_io != bar->is_io || is_64bit != bar->is_64bit ||
                (low & flag_mask) != (bar->flags & flag_mask) ||
                address != bar->address || !bar->size ||
                (bar->size & (bar->size - 1)) ||
                (bar->address & (bar->size - 1)) ||
                bar->address > UINT64_MAX - bar->size) {
                WARN("PCI: %02x:%02x.%u BAR%u resource mismatch config=0x%lx metadata=0x%lx/0x%lx",
                     resource->bus, resource->dev, resource->func, b,
                     (unsigned long)address, (unsigned long)bar->address,
                     (unsigned long)bar->size);
                return -1;
            }
            uint64_t aperture_base = bar->is_io ?
                VIRT_PCIE_IO_BASE : VIRT_PCIE_MMIO_BASE;
            uint64_t aperture_size = bar->is_io ?
                VIRT_PCIE_IO_SIZE : VIRT_PCIE_MMIO_SIZE;
            if (bar->address < aperture_base ||
                bar->size > aperture_size ||
                bar->address - aperture_base > aperture_size - bar->size) {
                WARN("PCI: %02x:%02x.%u BAR%u outside %s aperture",
                     resource->bus, resource->dev, resource->func, b,
                     bar->is_io ? "I/O" : "MMIO");
                return -1;
            }
            bar_count++;
        }
    }
    for (unsigned int i = 0; i < pci_device_count; i++) {
        for (unsigned int a = 0; a < PCI_MAX_BARS; a++) {
            struct pci_bar_resource *left = &pci_devices[i].bars[a];
            if (!left->valid)
                continue;
            uint64_t left_end = left->address + left->size;
            for (unsigned int j = i; j < pci_device_count; j++) {
                unsigned int first = j == i ? a + 1 : 0;
                for (unsigned int b = first; b < PCI_MAX_BARS; b++) {
                    struct pci_bar_resource *right = &pci_devices[j].bars[b];
                    if (!right->valid || left->is_io != right->is_io)
                        continue;
                    uint64_t right_end = right->address + right->size;
                    if (left->address < right_end &&
                        right->address < left_end) {
                        WARN("PCI: BAR overlap %02x:%02x.%u BAR%u and %02x:%02x.%u BAR%u",
                             pci_devices[i].bus, pci_devices[i].dev,
                             pci_devices[i].func, a, pci_devices[j].bus,
                             pci_devices[j].dev, pci_devices[j].func, b);
                        return -1;
                    }
                }
            }
        }
    }
    INFO("PCI: BAR resource audit PASSED bars=%u", bar_count);
    return 0;
}

static int pci_topology_audit(void)
{
    unsigned int bridge_count = 0;
    unsigned int bar_count = 0;
    for (unsigned int i = 0; i < pci_device_count; i++) {
        struct pci_device_resource *bridge = &pci_devices[i];
        if (bridge->header_type != PCI_HEADER_BRIDGE)
            continue;
        bridge_count++;
        struct pci_bridge_config config = pci_bridge_config_read(
            bridge->bus, bridge->dev, bridge->func);
        unsigned int primary = config.bus_numbers & 0xff;
        unsigned int secondary = (config.bus_numbers >> 8) & 0xff;
        unsigned int subordinate = (config.bus_numbers >> 16) & 0xff;
        if (primary != bridge->bus || secondary != bridge->secondary_bus ||
            subordinate != bridge->subordinate_bus || secondary <= primary ||
            subordinate < secondary || subordinate >= PCI_MAX_BUSES) {
            WARN("PCI: bridge %02x:%02x.%u invalid bus topology %u/%u/%u",
                 bridge->bus, bridge->dev, bridge->func,
                 primary, secondary, subordinate);
            return -1;
        }
        struct pci_bridge_window io = pci_bridge_io_window(&config);
        struct pci_bridge_window memory = pci_bridge_memory_window(
            config.memory_base_limit);
        struct pci_bridge_window prefetch = pci_bridge_prefetch_window(&config);
        for (unsigned int j = 0; j < pci_device_count; j++) {
            struct pci_device_resource *child = &pci_devices[j];
            if (child->bus < secondary || child->bus > subordinate)
                continue;
            for (unsigned int b = 0; b < PCI_MAX_BARS; b++) {
                struct pci_bar_resource *bar = &child->bars[b];
                if (!bar->valid)
                    continue;
                const struct pci_bridge_window *window = bar->is_io ? &io :
                    (bar->flags & PCI_BAR_PREFETCH ? &prefetch : &memory);
                if (!pci_bar_in_window(bar, window)) {
                    WARN("PCI: bridge %02x:%02x.%u does not forward %02x:%02x.%u BAR%u 0x%lx/0x%lx",
                         bridge->bus, bridge->dev, bridge->func,
                         child->bus, child->dev, child->func, b,
                         (unsigned long)bar->address,
                         (unsigned long)bar->size);
                    return -1;
                }
                bar_count++;
            }
        }
    }
    INFO("PCI: topology audit PASSED bridges=%u forwarded-bars=%u",
         bridge_count, bar_count);
    return 0;
}

static void pci_init_vmsi(struct pci_device_resource *resource)
{
    uint32_t status = pci_config_read(resource->bus, resource->dev,
                                      resource->func, PCI_CFG_STATUS);
    if (!(status & PCI_STAT_CAP_LIST))
        return;

    uint8_t cap = pci_config_read(resource->bus, resource->dev,
                                  resource->func, PCI_CFG_CAP_PTR) & 0xff;
    unsigned int guard = 0;
    while (cap && cap != 0xff && guard++ < 48) {
        if (cap < 0x40 || cap > 0xfc || (cap & 3)) {
            WARN("PCI: %02x:%02x.%u invalid capability pointer 0x%x",
                 resource->bus, resource->dev, resource->func, cap);
            break;
        }
        uint32_t header = pci_config_read(resource->bus, resource->dev,
                                          resource->func, cap);
        uint8_t id = header & 0xff;
        if (id == PCI_CAP_ID_MSI) {
            resource->msi_cap_off = cap;
            resource->msi_ctrl = (header >> 16) & 0xffff;
            uint16_t physical_ctrl = resource->msi_ctrl &
                ~(PCI_MSI_CTRL_EN | PCI_MSI_CTRL_MME_MASK);
            resource->msi_ctrl &= ~PCI_MSI_CTRL_EN;
            pci_config_write(resource->bus, resource->dev, resource->func,
                             cap, (header & 0x0000ffffU) |
                             ((uint32_t)physical_ctrl << 16));
            resource->msi_is_64bit = !!(resource->msi_ctrl &
                                        PCI_MSI_CTRL_64BIT);
            resource->msi_mask_capable = !!(resource->msi_ctrl &
                                            PCI_MSI_CTRL_MASK);
            unsigned int data_off = cap +
                (resource->msi_is_64bit ? 12 : 8);
            uint32_t address = pci_config_read(resource->bus, resource->dev,
                                               resource->func, cap + 4);
            resource->msi_addr = address;
            if (resource->msi_is_64bit) {
                uint32_t address_high = pci_config_read(
                    resource->bus, resource->dev, resource->func, cap + 8);
                resource->msi_addr |= (uint64_t)address_high << 32;
            }
            uint32_t data = pci_config_read(resource->bus, resource->dev,
                                            resource->func, data_off);
            resource->msi_data = data & 0xffff;
            resource->msi_mask = 0;
            resource->msi_pend = 0;
            INFO("PCI: %02x:%02x.%u vMSI cap=0x%x %s-bit mask=%u",
                 resource->bus, resource->dev, resource->func, cap,
                 resource->msi_is_64bit ? "64" : "32",
                 resource->msi_mask_capable);
        } else if (id == PCI_CAP_ID_MSIX) {
            uint32_t table = pci_config_read(resource->bus, resource->dev,
                                             resource->func,
                                             cap + PCI_MSIX_TABLE);
            uint32_t pba = pci_config_read(resource->bus, resource->dev,
                                           resource->func,
                                           cap + PCI_MSIX_PBA);
            resource->msix_cap_off = cap;
            resource->msix_ctrl = (header >> 16) & 0xffff;
            resource->msix_ctrl &= ~PCI_MSIX_CTRL_ENABLE;
            resource->msix_max_vectors =
                (resource->msix_ctrl & PCI_MSIX_CTRL_TABLE_SIZE) + 1;
            resource->msix_bir = table & 7;
            resource->msix_table_offset = table & ~7U;
            resource->msix_pba_bir = pba & 7;
            resource->msix_pba_offset = pba & ~7U;
            uint32_t physical = header &
                ~((uint32_t)PCI_MSIX_CTRL_ENABLE << 16);
            pci_config_write(resource->bus, resource->dev, resource->func,
                             cap, physical);
            uint32_t verify = pci_config_read(resource->bus, resource->dev,
                                              resource->func, cap);
            if (verify & ((uint32_t)PCI_MSIX_CTRL_ENABLE << 16))
                WARN("PCI: %02x:%02x.%u failed to disable physical MSI-X",
                     resource->bus, resource->dev, resource->func);
            INFO("PCI: %02x:%02x.%u vMSI-X cap=0x%x vectors=%u table=BAR%u+0x%x physical=off",
                 resource->bus, resource->dev, resource->func, cap,
                 resource->msix_max_vectors, resource->msix_bir,
                 resource->msix_table_offset);
        }
        cap = (header >> 8) & 0xff;
    }
}

static int pci_scan_bus(unsigned int bus)
{
    for (unsigned int dev = 0; dev < 32; dev++) {
        uint32_t id0 = pci_config_read(bus, dev, 0, PCI_CFG_VENDOR_ID);
        if ((id0 & 0xffff) == 0xffff)
            continue;
        uint8_t header0 = (uint8_t)pci_config_read(bus, dev, 0,
                                                   PCI_HEADER_TYPE);
        unsigned int functions = (header0 & PCI_HEADER_MULTI) ? 8 : 1;
        for (unsigned int func = 0; func < functions; func++) {
            uint32_t id = pci_config_read(bus, dev, func, PCI_CFG_VENDOR_ID);
            uint16_t vendor = id & 0xffff;
            uint16_t device = id >> 16;
            if (vendor == 0xffff)
                continue;
            if (pci_device_count == PCI_MAX_DEVICES)
                return -1;
            struct pci_device_resource *resource =
                &pci_devices[pci_device_count++];
            memzero(resource, sizeof(*resource));
            resource->vendor = vendor;
            resource->device = device;
            resource->bus = bus;
            resource->dev = dev;
            resource->func = func;
            resource->header_type = (uint8_t)pci_config_read(
                bus, dev, func, PCI_HEADER_TYPE) & 0x7f;
            if (vendor == PCI_EDU_VENDOR && device == PCI_EDU_DEVICE) {
                resource->owner_vmid = 1;
                edu_bus = bus;
                edu_dev = dev;
                edu_func = func;
                edu_found = 1;
            }
            INFO("PCI: enumerated %02x:%02x.%u id=%04x:%04x header=%u",
                 bus, dev, func, vendor, device, resource->header_type);
            if (resource->header_type == 0) {
                if (pci_probe_bars(resource, PCI_MAX_BARS) < 0)
                    return -1;
            } else if (resource->header_type == PCI_HEADER_BRIDGE) {
                if (pci_next_bus >= PCI_MAX_BUSES) {
                    WARN("PCI: no secondary bus available for %02x:%02x.%u",
                         bus, dev, func);
                    continue;
                }
                unsigned int secondary = pci_next_bus++;
                uint32_t bridge_command = pci_config_read(
                    bus, dev, func, PCI_CFG_COMMAND);
                struct pci_bridge_config saved_config =
                    pci_bridge_config_read(bus, dev, func);
                struct pci_resource_pool saved_mmio = mmio_pool;
                struct pci_resource_pool saved_io = io_pool;
                unsigned int saved_next_bus = pci_next_bus - 1;
                unsigned int saved_count = pci_device_count;
                pci_config_write(bus, dev, func, PCI_CFG_COMMAND,
                                 bridge_command | PCI_CMD_IO | PCI_CMD_MEMORY);
                resource->primary_bus = bus;
                resource->secondary_bus = secondary;
                uint32_t bus_numbers = (bus & 0xff) | (secondary << 8) |
                                       (0xff << 16);
                pci_config_write(bus, dev, func, PCI_BRIDGE_BUS_NUMBERS,
                                 bus_numbers);
                unsigned int child_start = pci_device_count;
                if (pci_scan_bus(secondary) < 0) {
                    mmio_pool = saved_mmio;
                    io_pool = saved_io;
                    pci_next_bus = saved_next_bus;
                    pci_device_count = saved_count;
                    pci_bridge_config_write(bus, dev, func, &saved_config);
                    pci_config_write(bus, dev, func, PCI_CFG_COMMAND,
                                     bridge_command);
                    resource->primary_bus = 0;
                    resource->secondary_bus = 0;
                    resource->subordinate_bus = 0;
                    WARN("PCI: bridge scan rolled back for %02x:%02x.%u",
                         bus, dev, func);
                    return -1;
                }
                unsigned int actual_subordinate = pci_next_bus - 1;
                unsigned int reserve_buses = PCI_BRIDGE_HOTPLUG_BUS_RESERVE;
                if (reserve_buses > PCI_MAX_BUSES - 1 - actual_subordinate)
                    reserve_buses = PCI_MAX_BUSES - 1 - actual_subordinate;
                unsigned int subordinate = actual_subordinate + reserve_buses;
                pci_next_bus = subordinate + 1;
                resource->subordinate_bus = subordinate;
                bus_numbers = (bus & 0xff) | (secondary << 8) |
                              (subordinate << 16);
                struct pci_bridge_config planned_config = {
                    .bus_numbers = bus_numbers,
                    .io_base_limit = 0x000000f0,
                    .io_upper = 0,
                    .memory_base_limit = 0x0000fff0,
                    .prefetch_base_limit = 0x0001fff1,
                    .prefetch_base_upper = 0,
                    .prefetch_limit_upper = 0,
                };

                uint64_t mmio_base = UINT64_MAX, mmio_limit = 0;
                uint64_t prefetch_base = UINT64_MAX, prefetch_limit = 0;
                uint32_t io_base = UINT32_MAX, io_limit = 0;
                for (unsigned int i = child_start; i < pci_device_count; i++) {
                    struct pci_device_resource *child = &pci_devices[i];
                    for (unsigned int b = 0; b < PCI_MAX_BARS; b++) {
                        struct pci_bar_resource *bar = &child->bars[b];
                        if (!bar->valid)
                            continue;
                        uint64_t start = bar->is_io ?
                            bar->address - VIRT_PCIE_IO_BASE : bar->address;
                        uint64_t end = start + bar->size - 1;
                        if (bar->is_io) {
                            if (start < io_base) io_base = start;
                            if (end > io_limit) io_limit = end;
                        } else if (bar->flags & PCI_BAR_PREFETCH) {
                            if (start < prefetch_base) prefetch_base = start;
                            if (end > prefetch_limit) prefetch_limit = end;
                        } else {
                            if (start < mmio_base) mmio_base = start;
                            if (end > mmio_limit) mmio_limit = end;
                        }
                    }
                }
                if (mmio_base != UINT64_MAX) {
                    uint64_t base = mmio_base &
                        ~(uint64_t)(PCI_BRIDGE_MEMORY_GRANULARITY - 1);
                    uint64_t limit = mmio_limit &
                        ~(uint64_t)(PCI_BRIDGE_MEMORY_GRANULARITY - 1);
                    limit = bridge_window_limit(&mmio_pool, base, limit,
                        PCI_BRIDGE_MEMORY_GRANULARITY, 0x100000);
                    uint32_t window = (uint32_t)((base >> 16) & 0xfff0) |
                        (uint32_t)((limit >> 16) << 16 & 0xfff00000);
                    planned_config.memory_base_limit = window;
                    INFO("PCI: bridge %02x:%02x.%u memory window 0x%lx-0x%lx",
                         bus, dev, func, (unsigned long)base,
                         (unsigned long)(limit + PCI_BRIDGE_MEMORY_GRANULARITY - 1));
                }
                if (prefetch_base != UINT64_MAX) {
                    uint64_t base = prefetch_base &
                        ~(uint64_t)(PCI_BRIDGE_MEMORY_GRANULARITY - 1);
                    uint64_t limit = prefetch_limit &
                        ~(uint64_t)(PCI_BRIDGE_MEMORY_GRANULARITY - 1);
                    limit = bridge_window_limit(&mmio_pool, base, limit,
                        PCI_BRIDGE_MEMORY_GRANULARITY, 0x100000);
                    uint32_t window = ((base >> 16) & 0xfff0) |
                        (((limit >> 16) & 0xfff0) << 16) |
                        (1U << 0) | (1U << 16);
                    planned_config.prefetch_base_limit = window;
                    planned_config.prefetch_base_upper = base >> 32;
                    planned_config.prefetch_limit_upper = limit >> 32;
                    INFO("PCI: bridge %02x:%02x.%u prefetch window 0x%lx-0x%lx",
                         bus, dev, func, (unsigned long)base,
                         (unsigned long)(limit + PCI_BRIDGE_MEMORY_GRANULARITY - 1));
                }
                int reserve_failed = 0;
                if (mmio_base != UINT64_MAX) {
                    uint64_t base = mmio_base &
                        ~(uint64_t)(PCI_BRIDGE_MEMORY_GRANULARITY - 1);
                    uint64_t limit = mmio_limit &
                        ~(uint64_t)(PCI_BRIDGE_MEMORY_GRANULARITY - 1);
                    limit = bridge_window_limit(&mmio_pool, base, limit,
                        PCI_BRIDGE_MEMORY_GRANULARITY, 0x100000);
                    uint64_t reserve_size = limit - base +
                        PCI_BRIDGE_MEMORY_GRANULARITY;
                    if (pool_reserve_range(&mmio_pool, base, reserve_size) < 0)
                        reserve_failed = 1;
                }
                if (prefetch_base != UINT64_MAX) {
                    uint64_t base = prefetch_base &
                        ~(uint64_t)(PCI_BRIDGE_MEMORY_GRANULARITY - 1);
                    uint64_t limit = prefetch_limit &
                        ~(uint64_t)(PCI_BRIDGE_MEMORY_GRANULARITY - 1);
                    limit = bridge_window_limit(&mmio_pool, base, limit,
                        PCI_BRIDGE_MEMORY_GRANULARITY, 0x100000);
                    uint64_t reserve_size = limit - base +
                        PCI_BRIDGE_MEMORY_GRANULARITY;
                    if (pool_reserve_range(&mmio_pool, base, reserve_size) < 0)
                        reserve_failed = 1;
                }
                if (io_base != UINT32_MAX) {
                    uint32_t base = io_base &
                        ~(PCI_BRIDGE_IO_GRANULARITY - 1);
                    uint32_t limit = io_limit &
                        ~(PCI_BRIDGE_IO_GRANULARITY - 1);
                    limit = bridge_window_limit(&io_pool, base, limit,
                        PCI_BRIDGE_IO_GRANULARITY, 0x400);
                    uint32_t reserve_size = limit - base +
                        PCI_BRIDGE_IO_GRANULARITY;
                    if (pool_reserve_range(&io_pool, base, reserve_size) < 0)
                        reserve_failed = 1;
                    uint32_t io_type = 1U;
                    planned_config.io_base_limit =
                        ((base >> 8) & 0xf0) | (limit & 0xf000) |
                        io_type | (io_type << 8);
                    planned_config.io_upper = ((base >> 16) & 0xffff) |
                                              ((limit >> 16) << 16);
                    INFO("PCI: bridge %02x:%02x.%u I/O window 0x%x-0x%x",
                         bus, dev, func, base,
                         limit + PCI_BRIDGE_IO_GRANULARITY - 1);
                }
                if (reserve_failed) {
                    mmio_pool = saved_mmio;
                    io_pool = saved_io;
                    pci_next_bus = saved_next_bus;
                    pci_device_count = saved_count;
                    pci_bridge_config_write(bus, dev, func, &saved_config);
                    pci_config_write(bus, dev, func, PCI_CFG_COMMAND,
                                     bridge_command);
                    WARN("PCI: bridge window reservation failed for %02x:%02x.%u",
                         bus, dev, func);
                    return -1;
                }
                pci_bridge_config_write(bus, dev, func, &planned_config);
                if (!pci_bridge_config_matches(bus, dev, func,
                                               &planned_config)) {
                    mmio_pool = saved_mmio;
                    io_pool = saved_io;
                    pci_next_bus = saved_next_bus;
                    pci_device_count = saved_count;
                    pci_bridge_config_write(bus, dev, func, &saved_config);
                    pci_config_write(bus, dev, func, PCI_CFG_COMMAND,
                                     bridge_command);
                    WARN("PCI: bridge config readback failed for %02x:%02x.%u",
                         bus, dev, func);
                    return -1;
                }
                INFO("PCI: bridge %02x:%02x.%u config committed and verified",
                     bus, dev, func);
            }
            pci_init_vmsi(resource);
            if (resource->msi_cap_off && resource->owner_vmid == 1 &&
                resource->bus == edu_bus && resource->dev == edu_dev &&
                resource->func == edu_func) {
                edu_msi_cap_off = resource->msi_cap_off;
                edu_msi_max_vectors = 1U <<
                    ((resource->msi_ctrl & PCI_MSI_CTRL_MMC_MASK) >>
                     PCI_MSI_CTRL_MMC_SHIFT);
            }
        }
    }
    return 0;
}

static int pci_enumerate(void)
{
    if (pool_selftest() < 0) {
        WARN("PCI: resource pool selftest failed");
        return -1;
    }
    INFO("PCI: resource pool selftest PASSED");
    pool_init(&mmio_pool, VIRT_PCIE_MMIO_BASE, VIRT_PCIE_MMIO_SIZE,
              PCI_ALLOC_BEST_FIT);
    pool_init(&io_pool, 0, PCI_IO_ALLOC_END, PCI_ALLOC_FIRST_FIT);
    if (pci_scan_bus(0) < 0 || !edu_found)
        return -1;
    if (pci_dma_guard_init() < 0 || pci_dma_state_audit() < 0)
        return -1;
    if (pci_bar_resource_audit() < 0) {
        WARN("PCI: BAR resource audit failed");
        return -1;
    }
    if (pci_topology_audit() < 0) {
        WARN("PCI: topology audit failed");
        return -1;
    }
    pool_dump_stats("MMIO", &mmio_pool);
    pool_dump_stats("I/O", &io_pool);
    for (unsigned int i = 0; i < pci_device_count; i++) {
        struct pci_device_resource *resource = &pci_devices[i];
        if (resource->msix_cap_off) {
            uint32_t ctrl = pci_config_read(resource->bus, resource->dev,
                                            resource->func,
                                            resource->msix_cap_off);
            if (ctrl & ((uint32_t)PCI_MSIX_CTRL_ENABLE << 16))
                WARN("PCI: %02x:%02x.%u physical MSI-X unexpectedly enabled",
                     resource->bus, resource->dev, resource->func);
        }
    }
    for (unsigned int i = 0; i < pci_device_count; i++) {
        struct pci_device_resource *resource = &pci_devices[i];
        if (resource->owner_vmid == 1) {
            for (unsigned int bar = 0; bar < PCI_MAX_BARS; bar++)
                if (resource->bars[bar].valid && !resource->bars[bar].is_io) {
                    edu_bar0 = resource->bars[bar].address;
                    edu_bar0_size = resource->bars[bar].size;
                    break;
                }
            break;
        }
    }
    return edu_bar0 != 0;
}

void pci_init(void)
{
    if (pci_enumerate() < 0) {
        WARN("PCI: edu device enumeration failed (vendor=0x%lx device=0x%lx)",
             (unsigned long)PCI_EDU_VENDOR, (unsigned long)PCI_EDU_DEVICE);
        return;
    }
    INFO("PCI: edu found at %02x:%02x.%x BAR0=0x%lx size=0x%lx",
         edu_bus, edu_dev, edu_func, (unsigned long)edu_bar0,
         (unsigned long)edu_bar0_size);

    /* 1. BAR 已由启动期 resource allocator 编程。 */

    /* 使能 BAR memory decode；ITS/MSI 路由完成前保持 bus master 关闭。 */
    uint32_t cmd = pci_config_read(edu_bus, edu_dev, edu_func, PCI_CFG_COMMAND);
    cmd = (cmd | PCI_CMD_MEMORY) & ~PCI_CMD_BUSMASTER;
    pci_config_write(edu_bus, edu_dev, edu_func, PCI_CFG_COMMAND, cmd);

    /*
     * 3. 计算 INTx 物理中断号。
     *
     * 读配置空间 offset 0x3c：低 8 位 interrupt line，高 8 位
     * interrupt pin（1=INTA ... 4=INTD，edu 固定使用 INTA）。
     * 经 swizzle 旋到主桥引脚后，主桥 INTx 线固定接 GIC SPI 3/4/5/6。
     */
    {
        uint32_t intr = pci_config_read(edu_bus, edu_dev, edu_func,
                                        PCI_CFG_INTR);
        uint8_t pin = (uint8_t)(intr >> 8);

        if (pin < 1 || pin > 4) {
            WARN("PCI: edu reports no INTx pin (0x%x), skip IRQ setup", pin);
            return;
        }

        unsigned int host_pin = ((unsigned int)pin - 1 + edu_dev) & 3;
        pci_edu_intx_irq = 32 + VIRT_PCIE_INTX_SPI_BASE + host_pin;

        INFO("PCI: edu INT%c (slot %u) swizzled to host INT%c"
             " -> GIC SPI %u (global %u)",
             'A' + pin - 1, edu_dev, 'A' + host_pin,
             pci_edu_intx_irq - 32, pci_edu_intx_irq);
        if (irq_domain_register(pci_edu_intx_irq, 1, 0,
                                VIRQ_PCI_EDU) < 0) {
            WARN("PCI: edu INTx ownership registration failed");
            return;
        }
    }

    /* 4. 把 edu INTx 物理中断路由到 CPU0 并使能 */
    gic_set_spi_target(pci_edu_intx_irq, 0);
    /*
     * 把直通 SPI 优先级设为 0x60（高于虚拟中断默认的 0x80）。
     *
     * 为什么必须这样做：
     *   GICv3 虚拟化中，LR 里 pending 的虚拟中断会把物理 CPU
     *   接口的运行优先级（RPR）降到该虚拟中断的优先级。如果
     *   物理 SPI 的优先级和它相同（都是 0x80），物理中断就
     *   无法抢占 EL1 guest 上下文——中断永远 pending 但不会
     *   触发，只会在 guest 主动陷入 EL2（如 WFI/HVC）后才
     *   能被处理。
     *
     *   把物理直通中断优先级设得更高（数值更小），保证即使
     *   guest 正在处理同优先级的虚拟中断，物理 INTx 也能
     *   及时抢占并注入到 vGIC。
     */
    gic_set_priority(pci_edu_intx_irq, 0x60);
    gic_enable_irq(pci_edu_intx_irq);
    INFO("PCI: edu INTx (GIC %u) routed to CPU0 and enabled",
         pci_edu_intx_irq);

    /*
     * 5. MSI 配置（Phase 8）
     *
     * 优先使用 MSI 代替 INTx：
     *   - 探测 MSI Capability
     *   - ITS 建立 device/collection/interrupt 映射
     *   - 写 MSI 消息地址 + 数据
     *   - 使能 MSI，禁用 INTx
     */
    {
        /* 5.1 检查 Status 寄存器的 Capabilities List 位 */
        uint32_t status = pci_config_read(edu_bus, edu_dev, edu_func,
                                          PCI_CFG_STATUS);
        if (!(status & PCI_STAT_CAP_LIST)) {
            INFO("PCI: edu has no capability list, use INTx only");
            return;
        }

        /* 5.2 读取 Cap Pointer，遍历 capability 列表找 MSI */
        uint8_t cap_off = (uint8_t)pci_config_read(edu_bus, edu_dev, edu_func,
                                                    PCI_CFG_CAP_PTR);
        while (cap_off != 0 && cap_off != 0xFF) {
            uint32_t cap_hdr = pci_config_read(edu_bus, edu_dev, edu_func,
                                               cap_off);
            uint8_t cap_id = (uint8_t)(cap_hdr & 0xFF);
            uint8_t next_ptr = (uint8_t)((cap_hdr >> 8) & 0xFF);

            if (cap_id == PCI_CAP_ID_MSI)
                edu_msi_cap_off = cap_off;
            else if (cap_id == PCI_CAP_ID_EXP)
                edu_pcie_cap_off = cap_off;
            cap_off = next_ptr;
        }

        if (edu_pcie_cap_off) {
            uint32_t devcap = pci_config_read(edu_bus, edu_dev, edu_func,
                                               edu_pcie_cap_off + PCI_EXP_DEVCAP);
            edu_flr_supported = !!(devcap & PCI_EXP_DEVCAP_FLR);
        }
        INFO("PCI: edu PCIe FLR %s",
             edu_flr_supported ? "supported" : "not supported");
        if (edu_msi_cap_off == 0) {
            INFO("PCI: edu has no MSI capability, use INTx only");
            return;
        }

        /* 5.3 读 MSI Message Control 寄存器 */
        uint32_t msi_ctrl = pci_config_read(edu_bus, edu_dev, edu_func,
                                            edu_msi_cap_off + PCI_MSI_CTRL);
        int is_64bit = (msi_ctrl & PCI_MSI_CTRL_64BIT) ? 1 : 0;
        int mme_shift = (msi_ctrl >> PCI_MSI_CTRL_MMC_SHIFT) & 0x7;
        unsigned int max_vectors = 1 << mme_shift;
        edu_msi_max_vectors = max_vectors;

        INFO("PCI: edu MSI cap at 0x%x (%s-bit, max %u vectors)",
             edu_msi_cap_off, is_64bit ? "64" : "32", max_vectors);

        /* 5.4 ITS 建立映射，分配 LPI */
        unsigned int lpi_base = its_setup_pci_msi(
            edu_bus, edu_dev, edu_func, max_vectors, 0);

        if (lpi_base == 0) {
            WARN("PCI: ITS MSI setup failed, fall back to INTx");
            return;
        }
        pci_edu_lpi_base = lpi_base;

        /* 5.5 写 MSI 消息地址（32 位 PA，高 32 位为 0）和消息数据 */
        uint64_t msi_addr = its_get_msi_address();
        uint32_t msi_data = its_get_msi_data(0);  /* eventID = 0 */
        /* Message Data 是 16 位字段：读-改-写避免越界覆盖相邻寄存器 */
        unsigned int data_off = is_64bit ? (edu_msi_cap_off + 0x0c)
                                         : (edu_msi_cap_off + PCI_MSI_DATA);
        uint32_t old = pci_config_read(edu_bus, edu_dev, edu_func, data_off);
        pci_config_write(edu_bus, edu_dev, edu_func, data_off,
                         (old & 0xFFFF0000) | (msi_data & 0xFFFF));

        struct pci_device_resource *edu_resource =
            pci_find_resource(edu_bus, edu_dev, edu_func);
        if (edu_resource) {
            edu_resource->msi_addr = msi_addr;
            edu_resource->msi_data = msi_data & 0xffff;
        }
        if (is_64bit) {
            /* 64 位地址：ADDR_LO 在 0x04，ADDR_HI 在 0x08 */
            pci_config_write(edu_bus, edu_dev, edu_func,
                             edu_msi_cap_off + PCI_MSI_ADDR_LO,
                             (uint32_t)(msi_addr & 0xFFFFFFFF));
            pci_config_write(edu_bus, edu_dev, edu_func,
                             edu_msi_cap_off + PCI_MSI_ADDR_LO + 4,
                             (uint32_t)(msi_addr >> 32));
        } else {
            /* 32 位地址：ADDR 在 0x04 */
            pci_config_write(edu_bus, edu_dev, edu_func,
                             edu_msi_cap_off + PCI_MSI_ADDR_LO,
                             (uint32_t)(msi_addr & 0xFFFFFFFF));
        }

        /* 5.6 使能 MSI，同时禁用 INTx（置位 INTx Disable 位） */
        msi_ctrl = pci_config_read(edu_bus, edu_dev, edu_func,
                                   edu_msi_cap_off + PCI_MSI_CTRL);
        /* Physical MME remains zero; the guest-visible value is virtual. */
        msi_ctrl &= ~(PCI_MSI_CTRL_MME_MASK);
        msi_ctrl |= PCI_MSI_CTRL_EN;
        edu_resource->msi_ctrl = (edu_resource->msi_ctrl &
                                  ~PCI_MSI_CTRL_EN) | PCI_MSI_CTRL_EN;
        unsigned int owned_vectors = max_vectors > 16 ? 16 : max_vectors;
        for (unsigned int i = 0; i < owned_vectors; i++) {
            if (irq_domain_register(lpi_base + i, 1, 0,
                                    VIRQ_PCI_EDU) < 0) {
                WARN("PCI: edu MSI LPI ownership registration failed");
                return;
            }
        }
        uint32_t dma_generation =
            pci_dma_get_generation(edu_resource);
        if (pci_dma_set_authorized(edu_resource, 1, 1,
                                   dma_generation) < 0) {
            WARN("PCI: edu DMA authorization failed after MSI routing");
            return;
        }
        pci_config_write(edu_bus, edu_dev, edu_func,
                         edu_msi_cap_off + PCI_MSI_CTRL, msi_ctrl);

        /* 禁用 INTx（Command 寄存器 bit 10） */
        uint32_t cmd = pci_config_read(edu_bus, edu_dev, edu_func,
                                       PCI_CFG_COMMAND);
        cmd |= PCI_CMD_INTX_DIS;
        pci_config_write(edu_bus, edu_dev, edu_func, PCI_CFG_COMMAND, cmd);

        /* INTx 模式已不再使用：同时把物理 INTx SPI 关掉，
         * 避免 stray pending（设备侧 INTx Disable 后线不会再拉高） */
        gic_disable_irq(pci_edu_intx_irq);

        pci_edu_msi_enabled = 1;
        INFO("PCI: edu MSI enabled (LPI base=%u, msg_addr=0x%lx msg_data=0x%x)",
             lpi_base, msi_addr, msi_data);
    }
}

static int pci_edu_quiesce(uint32_t *revoked_generation)
{
    uint32_t cmd;
    unsigned int timeout = 100000;

    cmd = pci_config_read(edu_bus, edu_dev, edu_func, PCI_CFG_COMMAND);
    struct pci_device_resource *resource =
        pci_find_resource(edu_bus, edu_dev, edu_func);
    if (!resource)
        return -1;
    uint32_t generation = pci_dma_get_generation(resource);
    if (pci_dma_set_authorized(resource, 1, 0, generation) < 0)
        return -1;
    if (revoked_generation)
        *revoked_generation = generation + 1;
    pci_config_write(edu_bus, edu_dev, edu_func, PCI_CFG_COMMAND,
                     pci_command_dma_safe(cmd) | PCI_CMD_INTX_DIS);
    if (edu_msi_cap_off) {
        uint32_t msi = pci_config_read(edu_bus, edu_dev, edu_func,
                                       edu_msi_cap_off + PCI_MSI_CTRL);
        pci_config_write(edu_bus, edu_dev, edu_func,
                         edu_msi_cap_off + PCI_MSI_CTRL,
                         msi & ~PCI_MSI_CTRL_EN);
        resource->msi_ctrl &= ~PCI_MSI_CTRL_EN;
    }
    while (timeout-- &&
           ((get32(edu_bar0 + EDU_REG_STATUS) & 1) ||
            (get32(edu_bar0 + EDU_REG_DMA_CMD) & 1)))
        wait_msec(1);
    return timeout ? 0 : -1;
}

static void pci_edu_internal_reset(void)
{
    put32(edu_bar0 + EDU_REG_IRQ_ACK, 0xffffffff);
    put32(edu_bar0 + EDU_REG_DMA_CMD, 0);
    put32(edu_bar0 + EDU_REG_DMA_SRC, 0);
    put32(edu_bar0 + EDU_REG_DMA_DST, 0);
    put32(edu_bar0 + EDU_REG_DMA_COUNT, 0);
    put32(edu_bar0 + EDU_REG_LIVENESS, 0);
    put32(edu_bar0 + EDU_REG_FACTORIAL, 0);
    asm volatile("dsb sy" ::: "memory");
}

static int pci_edu_reinit_msi(uint32_t expected_generation)
{
    unsigned int lpi_base = 0;
    unsigned int registered_vectors = 0;
    unsigned int intx_registered = 0;
    unsigned int vectors = edu_msi_max_vectors > 16 ? 16 : edu_msi_max_vectors;
    struct pci_device_resource *edu_resource =
        pci_find_resource(edu_bus, edu_dev, edu_func);

    if (!edu_msi_cap_off)
        return 0;
    if (!edu_resource ||
        pci_dma_get_generation(edu_resource) != expected_generation)
        return -1;

    gic_set_spi_target(pci_edu_intx_irq, 0);
    lpi_base = its_setup_pci_msi(edu_bus, edu_dev, edu_func,
                                 edu_msi_max_vectors, 0);
    if (!lpi_base)
        return -1;
    for (unsigned int i = 0; i < vectors; i++) {
        if (irq_domain_register(lpi_base + i, 1, 0,
                                VIRQ_PCI_EDU) < 0)
            goto rollback;
        registered_vectors++;
    }
    if (pci_edu_intx_irq >= 32 && pci_edu_intx_irq < 1020) {
        if (irq_domain_register(pci_edu_intx_irq, 1, 0,
                                VIRQ_PCI_EDU) < 0)
            goto rollback;
        intx_registered = 1;
        gic_set_spi_target(pci_edu_intx_irq, 0);
    }

    uint64_t msi_addr = its_get_msi_address();
    uint32_t data_off =
        (pci_config_read(edu_bus, edu_dev, edu_func,
                         edu_msi_cap_off + PCI_MSI_CTRL) &
         PCI_MSI_CTRL_64BIT) ? 0x0c : PCI_MSI_DATA;
    uint32_t old_data = pci_config_read(edu_bus, edu_dev, edu_func,
                                        edu_msi_cap_off + data_off);
    pci_config_write(edu_bus, edu_dev, edu_func,
                     edu_msi_cap_off + data_off,
                     (old_data & 0xffff0000) | (its_get_msi_data(0) & 0xffff));
    pci_config_write(edu_bus, edu_dev, edu_func,
                     edu_msi_cap_off + PCI_MSI_ADDR_LO, (uint32_t)msi_addr);
    if (pci_config_read(edu_bus, edu_dev, edu_func,
                        edu_msi_cap_off + PCI_MSI_CTRL) & PCI_MSI_CTRL_64BIT)
        pci_config_write(edu_bus, edu_dev, edu_func,
                         edu_msi_cap_off + PCI_MSI_ADDR_LO + 4,
                         (uint32_t)(msi_addr >> 32));
    pci_config_write(edu_bus, edu_dev, edu_func,
                     edu_msi_cap_off + data_off,
                     its_get_msi_data(0) & 0xffff);

    if (pci_dma_set_authorized(edu_resource, 1, 1,
                               expected_generation) < 0)
        goto rollback;

    edu_resource->msi_addr = msi_addr;
    edu_resource->msi_data = its_get_msi_data(0) & 0xffff;
    edu_resource->msi_ctrl = (edu_resource->msi_ctrl &
                              ~PCI_MSI_CTRL_EN) | PCI_MSI_CTRL_EN;
    edu_resource->msi_mask = 0;
    edu_resource->msi_pend = 0;
    uint32_t msi_ctrl = pci_config_read(edu_bus, edu_dev, edu_func,
                                        edu_msi_cap_off + PCI_MSI_CTRL);
    msi_ctrl &= ~(0x7 << PCI_MSI_CTRL_MME_SHIFT);
    pci_config_write(edu_bus, edu_dev, edu_func,
                     edu_msi_cap_off + PCI_MSI_CTRL,
                     msi_ctrl | PCI_MSI_CTRL_EN);
    uint32_t cmd = pci_config_read(edu_bus, edu_dev, edu_func,
                                   PCI_CFG_COMMAND);
    pci_config_write(edu_bus, edu_dev, edu_func, PCI_CFG_COMMAND,
                     cmd | PCI_CMD_MEMORY | PCI_CMD_INTX_DIS);
    gic_disable_irq(pci_edu_intx_irq);
    pci_edu_lpi_base = lpi_base;
    pci_edu_msi_enabled = 1;
    INFO("PCI: edu MSI reinitialized (LPI base=%u)", lpi_base);
    return 0;

rollback:
    if (intx_registered) {
        gic_disable_irq(pci_edu_intx_irq);
        put32(GICD_ICPENDR(pci_edu_intx_irq / 32),
              1U << (pci_edu_intx_irq % 32));
        irq_domain_unregister(pci_edu_intx_irq);
    }
    if (its_teardown_pci_msi(pci_edu_requester_id(), lpi_base,
                             vectors) < 0)
        WARN("PCI: failed to release rolled-back ITS MSI resources");
    if (vits_lpi_revoke_range_owned(lpi_base, registered_vectors, 1) < 0)
        WARN("PCI: failed to revoke rolled-back vITS routes");
    pci_edu_lpi_base = 0;
    pci_edu_msi_enabled = 0;
    WARN("PCI: edu MSI reinitialization transaction rolled back");
    return -1;
}

int pci_edu_vm_reset(struct task_struct *vm)
{
    if (!vm || vm->vmid != 1 || !edu_found || pci_edu_quiesce(NULL) < 0)
        return -1;
    if (pci_edu_intx_irq >= 32 && pci_edu_intx_irq < 1020) {
        gic_disable_irq(pci_edu_intx_irq);
        put32(GICD_ICPENDR(pci_edu_intx_irq / 32),
              1U << (pci_edu_intx_irq % 32));
        irq_domain_unregister(pci_edu_intx_irq);
    }
    if (pci_edu_lpi_base) {
        unsigned int vectors = edu_msi_max_vectors > 16 ? 16 :
                               edu_msi_max_vectors;
        if (its_teardown_pci_msi(pci_edu_requester_id(),
                                 pci_edu_lpi_base, vectors) < 0)
            return -1;
        if (vits_lpi_revoke_range_owned(pci_edu_lpi_base, vectors, 1) < 0)
            return -1;
    }
    pci_edu_lpi_base = 0;
    pci_edu_msi_enabled = 0;
    INFO("PCI: edu IRQ resources released for VM%lu", vm->vmid);
    return 0;
}

int pci_edu_function_reset(struct task_struct *vm)
{
    uint32_t revoked_generation;
    if (!vm || vm->vmid != 1 || !edu_found)
        return -1;
    if (pci_edu_quiesce(&revoked_generation) < 0) {
        WARN("PCI: edu reset aborted, device did not become idle");
        return -1;
    }
    if (pci_edu_intx_irq >= 32 && pci_edu_intx_irq < 1020) {
        gic_disable_irq(pci_edu_intx_irq);
        put32(GICD_ICPENDR(pci_edu_intx_irq / 32),
              1U << (pci_edu_intx_irq % 32));
        irq_domain_unregister(pci_edu_intx_irq);
    }
    if (pci_edu_lpi_base) {
        unsigned int vectors = edu_msi_max_vectors > 16 ? 16 :
                               edu_msi_max_vectors;
        if (its_teardown_pci_msi(pci_edu_requester_id(),
                                 pci_edu_lpi_base, vectors) < 0) {
            WARN("PCI: edu reset aborted, ITS resources remain mapped");
            return -1;
        }
        if (vits_lpi_revoke_range_owned(pci_edu_lpi_base, vectors,
                                        vm->vmid) < 0) {
            WARN("PCI: edu reset aborted, vITS route ownership mismatch");
            return -1;
        }
    }
    if (edu_flr_supported && edu_pcie_cap_off) {
        uint32_t devctl = pci_config_read(edu_bus, edu_dev, edu_func,
                                          edu_pcie_cap_off + PCI_EXP_DEVCTL);
        pci_config_write(edu_bus, edu_dev, edu_func,
                         edu_pcie_cap_off + PCI_EXP_DEVCTL,
                         devctl | PCI_EXP_DEVCTL_FLR);
        wait_msec(100);
    } else {
        pci_edu_internal_reset();
    }
    struct pci_device_resource *edu_resource = NULL;
    for (unsigned int i = 0; i < pci_device_count; i++)
        if (pci_devices[i].bus == edu_bus && pci_devices[i].dev == edu_dev &&
            pci_devices[i].func == edu_func) {
            edu_resource = &pci_devices[i];
            break;
        }
    if (!edu_resource || !edu_resource->bars[0].valid)
        return -1;
    uint32_t cmd = pci_config_read(edu_bus, edu_dev, edu_func, PCI_CFG_COMMAND);
    pci_config_write(edu_bus, edu_dev, edu_func, PCI_CFG_COMMAND,
                     cmd & ~(PCI_CMD_IO | PCI_CMD_MEMORY));
    for (unsigned int i = 0; i < PCI_MAX_BARS; i++) {
        struct pci_bar_resource *bar = &edu_resource->bars[i];
        if (!bar->valid)
            continue;
        unsigned int reg = PCI_BAR0 + i * 4;
        pci_config_write(edu_bus, edu_dev, edu_func, reg,
                         (uint32_t)bar->address | bar->flags);
        if (bar->is_64bit) {
            pci_config_write(edu_bus, edu_dev, edu_func, reg + 4,
                             (uint32_t)(bar->address >> 32));
            i++;
        }
    }
    edu_bar0 = edu_resource->bars[0].address;
    edu_bar0_size = edu_resource->bars[0].size;
    pci_config_write(edu_bus, edu_dev, edu_func, PCI_CFG_COMMAND,
                     (cmd | PCI_CMD_MEMORY | PCI_CMD_INTX_DIS) &
                     ~PCI_CMD_BUSMASTER);
    pci_edu_lpi_base = 0;
    pci_edu_msi_enabled = 0;
    if (pci_edu_reinit_msi(revoked_generation) < 0) {
        WARN("PCI: edu MSI reinitialization failed after reset");
        return -1;
    }
    INFO("PCI: edu function reset complete for VM%lu (%s)", vm->vmid,
         edu_flr_supported ? "PCIe FLR" : "edu internal fallback");
    return 0;
}

static int pci_resource_owner(struct pci_device_resource *resource,
                              unsigned int bus, unsigned int dev,
                              unsigned int func)
{
    return resource->bus == bus && resource->dev == dev &&
           resource->func == func;
}

static struct pci_device_resource *pci_find_resource(unsigned int bus,
                                                      unsigned int dev,
                                                      unsigned int func)
{
    for (unsigned int i = 0; i < pci_device_count; i++) {
        if (pci_resource_owner(&pci_devices[i], bus, dev, func))
            return &pci_devices[i];
    }
    return 0;
}

static int pci_io_page_conflict(unsigned int page, unsigned int owner_vmid)
{
    uint64_t page_start = VIRT_PCIE_IO_BASE + (uint64_t)page * PAGE_SIZE;
    uint64_t page_end = page_start + PAGE_SIZE;

    for (unsigned int i = 0; i < pci_device_count; i++) {
        struct pci_device_resource *resource = &pci_devices[i];
        for (unsigned int bar = 0; bar < PCI_MAX_BARS; bar++) {
            struct pci_bar_resource *entry = &resource->bars[bar];
            uint64_t start, end;
            if (!entry->valid || !entry->is_io)
                continue;
            start = entry->address;
            end = start + entry->size;
            if (start < page_end && end > page_start &&
                resource->owner_vmid != owner_vmid)
                return 1;
        }
    }
    return 0;
}

static int pci_map_owned_io(struct task_struct *vm)
{
    uint8_t mapped[PCI_IO_ALLOC_END / PAGE_SIZE];
    unsigned int mapped_count = 0;

    memzero(mapped, sizeof(mapped));
    for (unsigned int i = 0; i < pci_device_count; i++) {
        struct pci_device_resource *resource = &pci_devices[i];
        if (resource->owner_vmid != vm->vmid)
            continue;
        for (unsigned int bar = 0; bar < PCI_MAX_BARS; bar++) {
            struct pci_bar_resource *entry = &resource->bars[bar];
            uint64_t start, end;
            if (!entry->valid || !entry->is_io)
                continue;
            if (entry->address < VIRT_PCIE_IO_BASE ||
                entry->address + entry->size > VIRT_PCIE_IO_BASE +
                VIRT_PCIE_IO_SIZE)
                goto rollback;
            start = entry->address & PAGE_MASK;
            end = (entry->address + entry->size + PAGE_SIZE - 1) & PAGE_MASK;
            for (uint64_t ipa = start; ipa < end; ipa += PAGE_SIZE) {
                unsigned int page = (ipa - VIRT_PCIE_IO_BASE) / PAGE_SIZE;
                if (pci_io_page_conflict(page, vm->vmid))
                    goto rollback;
                if (mapped[page])
                    continue;
                if (stage2_page_is_mapped(vm, ipa))
                    goto rollback;
                map_stage2_page(vm, ipa, ipa,
                                MMU_STAGE2_DEVICE_PAGE_FLAGS);
                mapped[page] = 1;
                mapped_count++;
            }
        }
    }
    INFO("PCI: mapped %u owned I/O aperture pages into VM%lu",
         mapped_count, (unsigned long)vm->vmid);
    return 0;

rollback:
    for (unsigned int page = 0; page < PCI_IO_ALLOC_END / PAGE_SIZE; page++)
        if (mapped[page])
            unmap_stage2_page(vm, VIRT_PCIE_IO_BASE +
                              (uint64_t)page * PAGE_SIZE, 0);
    return -1;
}

int pci_passthrough_map(struct task_struct *vm)
{
    if (!vm || vm->vmid != 1 || !edu_found || !edu_bar0 ||
        !edu_bar0_size || (edu_bar0 & (PAGE_SIZE - 1)) ||
        (edu_bar0_size & (PAGE_SIZE - 1)) ||
        edu_bar0 < VIRT_PCIE_MMIO_BASE || edu_bar0 > PCI_MMIO_ALLOC_END ||
        edu_bar0_size > PCI_MMIO_ALLOC_END - edu_bar0)
        return -1;

    uint64_t mapped_bar_size = 0;
    for (uint64_t off = 0; off < edu_bar0_size; off += PAGE_SIZE) {
        uint64_t ipa = edu_bar0 + off;
        if (stage2_page_is_mapped(vm, ipa))
            goto rollback;
        map_stage2_page(vm, ipa, ipa, MMU_STAGE2_DEVICE_PAGE_FLAGS);
        mapped_bar_size += PAGE_SIZE;
    }
    if (pci_map_owned_io(vm) < 0)
        goto rollback;
    INFO("PCI: mapped %u owned devices into VM%lu (ECAM emulated)",
         edu_found ? 1 : 0, (unsigned long)vm->vmid);
    return 0;

rollback:
    while (mapped_bar_size) {
        mapped_bar_size -= PAGE_SIZE;
        unmap_stage2_page(vm, edu_bar0 + mapped_bar_size, 0);
    }
    WARN("PCI: passthrough mapping rolled back for VM%lu",
         (unsigned long)vm->vmid);
    return -1;
}

static uint8_t pci_vmsi_read_byte(struct pci_device_resource *resource,
                                  unsigned int reg)
{
    unsigned int off = reg - resource->msi_cap_off;
    unsigned int addr_size = resource->msi_is_64bit ? 8 : 4;
    unsigned int data_off = 4 + addr_size;
    unsigned int mask_off = data_off + 4;
    unsigned int pend_off = mask_off + 4;

    if (off == 0)
        return (uint8_t)(pci_config_read(resource->bus, resource->dev,
                                         resource->func, resource->msi_cap_off) & 0xff);
    if (off == 1)
        return (uint8_t)(pci_config_read(resource->bus, resource->dev,
                                         resource->func, resource->msi_cap_off) >> 8);
    if (off == 2)
        return (uint8_t)resource->msi_ctrl;
    if (off == 3)
        return (uint8_t)(resource->msi_ctrl >> 8);
    if (off >= 4 && off < 4 + addr_size) {
        uint64_t addr = (resource->owner_vmid == 1 &&
                         resource->bus == edu_bus && resource->dev == edu_dev &&
                         resource->func == edu_func) ?
                        its_get_msi_address() : resource->msi_addr;
        return (uint8_t)(addr >> ((off - 4) * 8));
    }
    if (off >= data_off && off < data_off + 2)
        return (uint8_t)(resource->msi_data >> ((off - data_off) * 8));
    if (resource->msi_mask_capable && off >= mask_off &&
        off < mask_off + 4)
        return (uint8_t)(resource->msi_mask >> ((off - mask_off) * 8));
    if (resource->msi_mask_capable && off >= pend_off &&
        off < pend_off + 4)
        return (uint8_t)(resource->msi_pend >> ((off - pend_off) * 8));
    return pci_config_read(resource->bus, resource->dev, resource->func,
                           reg & ~3U) >> ((reg & 3U) * 8);
}

static void pci_vmsi_write_byte(struct pci_device_resource *resource,
                                unsigned int reg, uint8_t value)
{
    unsigned int off = reg - resource->msi_cap_off;
    unsigned int addr_size = resource->msi_is_64bit ? 8 : 4;
    unsigned int data_off = 4 + addr_size;
    unsigned int mask_off = data_off + 4;

    if (off == 2) {
        resource->msi_ctrl = (resource->msi_ctrl & ~PCI_MSI_CTRL_EN) |
                             (value & PCI_MSI_CTRL_EN);
        uint32_t physical = pci_config_read(resource->bus, resource->dev,
                                             resource->func,
                                             resource->msi_cap_off + PCI_MSI_CTRL);
        if (value & PCI_MSI_CTRL_EN)
            physical |= PCI_MSI_CTRL_EN;
        else
            physical &= ~PCI_MSI_CTRL_EN;
        pci_config_write(resource->bus, resource->dev, resource->func,
                         resource->msi_cap_off + PCI_MSI_CTRL, physical);
        return;
    }
    if (off == 3) {
        unsigned int mme = (value >> 4) & 0x7;
        unsigned int mmc = (resource->msi_ctrl & PCI_MSI_CTRL_MMC_MASK) >>
                           PCI_MSI_CTRL_MMC_SHIFT;
        if (mme > mmc)
            mme = mmc;
        resource->msi_ctrl = (resource->msi_ctrl & ~PCI_MSI_CTRL_MME_MASK) |
                             (mme << PCI_MSI_CTRL_MME_SHIFT);
        return;
    }
    if (off >= 4 && off < 4 + addr_size) {
        unsigned int shift = (off - 4) * 8;
        uint64_t mask = 0xffULL << shift;
        resource->msi_addr = (resource->msi_addr & ~mask) |
                             ((uint64_t)value << shift);
        return;
    }
    if (off >= data_off && off < data_off + 2) {
        unsigned int shift = (off - data_off) * 8;
        resource->msi_data = (resource->msi_data & ~(0xffU << shift)) |
                             ((uint16_t)value << shift);
        pci_config_write(resource->bus, resource->dev, resource->func,
                         resource->msi_cap_off + data_off,
                         resource->msi_data);
        return;
    }
    if (resource->msi_mask_capable && off >= mask_off &&
        off < mask_off + 4) {
        unsigned int shift = (off - mask_off) * 8;
        resource->msi_mask = (resource->msi_mask & ~(0xffU << shift)) |
                             ((uint32_t)value << shift);
        uint32_t physical = pci_config_read(resource->bus, resource->dev,
                                            resource->func,
                                            resource->msi_cap_off + mask_off);
        physical = (physical & ~(0xffU << shift)) |
                   ((uint32_t)value << shift);
        pci_config_write(resource->bus, resource->dev, resource->func,
                         resource->msi_cap_off + mask_off, physical);
    }
}

static int pci_vmsi_access(struct pci_device_resource *resource,
                           unsigned int reg, unsigned int size, int write,
                           uint64_t *value)
{
    unsigned int data_off = 4 + (resource->msi_is_64bit ? 8 : 4);
    unsigned int length = resource->msi_mask_capable ?
                          data_off + 12 : data_off + 4;
    if (!resource->msi_cap_off || reg < resource->msi_cap_off ||
        reg + size > resource->msi_cap_off + length)
        return 0;
    if (write) {
        for (unsigned int i = 0; i < size; i++)
            pci_vmsi_write_byte(resource, reg + i,
                                (*value >> (i * 8)) & 0xff);
    } else {
        *value = 0;
        for (unsigned int i = 0; i < size; i++)
            *value |= (uint64_t)pci_vmsi_read_byte(resource, reg + i) <<
                      (i * 8);
    }
    return 1;
}

static uint8_t pci_msix_read_byte(struct pci_device_resource *resource,
                                  unsigned int reg)
{
    unsigned int off = reg - resource->msix_cap_off;
    if (off < 2)
        return (pci_config_read(resource->bus, resource->dev, resource->func,
                                reg & ~3U) >> ((reg & 3U) * 8)) & 0xff;
    if (off == 2)
        return resource->msix_ctrl & 0xff;
    if (off == 3)
        return resource->msix_ctrl >> 8;
    if (off >= 4 && off < 12)
        return pci_config_read(resource->bus, resource->dev, resource->func,
                               reg & ~3U) >> ((reg & 3U) * 8);
    return 0xff;
}

static void pci_msix_write_byte(struct pci_device_resource *resource,
                                unsigned int reg, uint8_t value)
{
    unsigned int off = reg - resource->msix_cap_off;
    if (off == 2)
        return;
    if (off == 3) {
        uint16_t writable = ((uint16_t)value << 8) &
            (PCI_MSIX_CTRL_ENABLE | PCI_MSIX_CTRL_FUNC_MASK);
        resource->msix_ctrl =
            (resource->msix_ctrl & ~(PCI_MSIX_CTRL_ENABLE |
                                     PCI_MSIX_CTRL_FUNC_MASK)) | writable;
        uint32_t physical = pci_config_read(resource->bus, resource->dev,
                                             resource->func,
                                             resource->msix_cap_off);
        physical &= ~((uint32_t)PCI_MSIX_CTRL_ENABLE << 16);
        pci_config_write(resource->bus, resource->dev, resource->func,
                         resource->msix_cap_off, physical);
    }
}

static int pci_msix_access(struct pci_device_resource *resource,
                           unsigned int reg, unsigned int size, int write,
                           uint64_t *value)
{
    if (!resource->msix_cap_off || reg < resource->msix_cap_off ||
        reg + size > resource->msix_cap_off + 12)
        return 0;
    if (write) {
        for (unsigned int i = 0; i < size; i++)
            pci_msix_write_byte(resource, reg + i,
                                (*value >> (i * 8)) & 0xff);
    } else {
        *value = 0;
        for (unsigned int i = 0; i < size; i++)
            *value |= (uint64_t)pci_msix_read_byte(resource, reg + i) <<
                      (i * 8);
    }
    return 1;
}

static uint32_t pci_config_read_width(unsigned int bus, unsigned int dev,
                                      unsigned int func, unsigned int reg,
                                      unsigned int size)
{
    uint32_t value = 0;
    for (unsigned int i = 0; i < size; i++) {
        uint32_t word = pci_config_read(bus, dev, func,
                                        (reg + i) & ~3U);
        value |= ((word >> (((reg + i) & 3U) * 8)) & 0xffU) << (i * 8);
    }
    return value;
}

int pci_is_mmio_addr(struct task_struct *vm, unsigned long ipa)
{
    (void)vm;
    return (ipa >= VIRT_PCIE_ECAM_BASE &&
            ipa < VIRT_PCIE_ECAM_BASE + VIRT_PCIE_ECAM_SIZE) ||
           (ipa >= VIRT_PCIE_IO_BASE &&
            ipa < VIRT_PCIE_IO_BASE + VIRT_PCIE_IO_SIZE);
}

int pci_mmio_access(struct task_struct *vm, unsigned long ipa,
                    unsigned int size, int write, uint64_t *value)
{
    uint64_t ecam_end = VIRT_PCIE_ECAM_BASE + VIRT_PCIE_ECAM_SIZE;
    uint64_t io_end = VIRT_PCIE_IO_BASE + VIRT_PCIE_IO_SIZE;
    unsigned int bus, dev, func, reg;
    struct pci_device_resource *resource;

    if (!value || (size != 1 && size != 2 && size != 4))
        return 0;
    if (ipa >= VIRT_PCIE_IO_BASE && ipa < io_end) {
        unsigned int page = (ipa - VIRT_PCIE_IO_BASE) / PAGE_SIZE;
        if (!vm || pci_io_page_conflict(page, vm->vmid)) {
            *value = write ? 0 : (size == 4 ? 0xffffffffU :
                                  (size == 2 ? 0xffffU : 0xffU));
            return 1;
        }
        return 0;
    }
    if (ipa < VIRT_PCIE_ECAM_BASE || ipa >= ecam_end)
        return 0;
    uint64_t offset = ipa - VIRT_PCIE_ECAM_BASE;
    bus = (offset >> 20) & 0xff;
    dev = (offset >> 15) & 0x1f;
    func = (offset >> 12) & 7;
    reg = offset & 0xfff;
    resource = pci_find_resource(bus, dev, func);
    if (!resource || resource->owner_vmid != (vm ? vm->vmid : 0)) {
        *value = write ? 0 : (size == 4 ? 0xffffffffU :
                              (size == 2 ? 0xffffU : 0xffU));
        return 1;
    }
    if (!write) {
        if (pci_msix_access(resource, reg, size, 0, value))
            return 1;
        if (resource->msix_cap_off && reg < resource->msix_cap_off + 12 &&
            reg + size > resource->msix_cap_off)
            return 1;
        if (resource->msi_cap_off &&
            pci_vmsi_access(resource, reg, size, 0, value))
            return 1;
        *value = pci_config_read_width(bus, dev, func, reg, size);
        return 1;
    }
    if ((uint64_t)reg + size > 0x1000)
        return 1;
    if (reg < PCI_BAR0 + PCI_MAX_BARS * 4 &&
        reg + size > PCI_BAR0)
        return 1;
    if (pci_msix_access(resource, reg, size, 1, value))
        return 1;
    if (resource->msix_cap_off && reg < resource->msix_cap_off + 12 &&
        reg + size > resource->msix_cap_off)
        return 1;
    if (resource->msi_cap_off &&
        pci_vmsi_access(resource, reg, size, 1, value))
        return 1;
    if (reg < PCI_CFG_COMMAND + 2 && reg + size > PCI_CFG_COMMAND) {
        unsigned long dma_flags = pci_dma_lock_acquire();
        uint32_t command = pci_config_read(bus, dev, func, PCI_CFG_COMMAND);
        uint32_t dma_state = resource->dma_authorized ?
                             PCI_CMD_BUSMASTER : 0;
        unsigned int first = reg > PCI_CFG_COMMAND ? reg : PCI_CFG_COMMAND;
        unsigned int last = reg + size < PCI_CFG_COMMAND + 2 ?
                            reg + size : PCI_CFG_COMMAND + 2;
        for (unsigned int byte = first; byte < last; byte++) {
            unsigned int src_shift = (byte - reg) * 8;
            uint8_t incoming = (*value >> src_shift) & 0xff;
            if (byte == PCI_CFG_COMMAND)
                command = (command & ~0xffU) | incoming;
            else
                command = (command & ~PCI_CMD_INTX_DIS) |
                          ((uint32_t)(incoming & (PCI_CMD_INTX_DIS >> 8)) << 8);
        }
        command = pci_command_dma_safe(command) | dma_state;
        pci_config_write(bus, dev, func, PCI_CFG_COMMAND, command);
        int audit_ret = pci_dma_state_audit_locked();
        *value = pci_config_read_width(bus, dev, func, reg, size);
        pci_dma_lock_release(dma_flags);
        if (audit_ret < 0)
            WARN("PCI: DMA state audit failed after guest Command write");
        return 1;
    }
    for (unsigned int i = 0; i < size; i++) {
        unsigned int byte_reg = reg + i;
        unsigned int shift = (byte_reg & 3U) * 8;
        uint32_t word = pci_config_read(bus, dev, func, byte_reg & ~3U);
        uint32_t byte = (*value >> (i * 8)) & 0xffU;
        word = (word & ~(0xffU << shift)) | (byte << shift);
        pci_config_write(bus, dev, func, byte_reg & ~3U, word);
    }
    return 1;
}

/*
 * 调试：dump PCI 直通中断链路的完整状态
 * （Guest 在 INTx 测试超时后通过 HVC_PCI_DEBUG 触发）
 * 注意：不读 ICH_VTR_EL2 / ICH_LR2_EL2 —— 这套 QEMU 上它们未实现，
 * 读取会触发 UNDEFINED 异常（曾导致 PANIC）。
 */
void pci_debug_dump(void)
{
    INFO("PCI dbg: intx_irq=%u", pci_edu_intx_irq);
    INFO("PCI dbg: GICD en1=0x%x pend1=0x%x act1=0x%x",
         get32(GICD_ISENABLER(1)), get32(GICD_ISPENDR(1)),
         get32(GICD_ISACTIVER(1)));
    if (pci_edu_intx_irq) {
        uint64_t router = GICD_IROUTER(pci_edu_intx_irq);
        INFO("PCI dbg: IROUTER[%u]=0x%x_%08x",
             pci_edu_intx_irq, get32(router + 4), get32(router));
    }
    if (edu_found)
        INFO("PCI dbg: edu irq_status=0x%x",
             get32(edu_bar0 + EDU_REG_IRQ_STATUS));
    INFO("PCI dbg: LR0=0x%lx LR1=0x%lx",
         READ_SYSREG64(ICH_LR0_EL2), READ_SYSREG64(ICH_LR1_EL2));
    INFO("PCI dbg: RPR=0x%02x PMR=0x%02x HPPIR=%u",
         READ_SYSREG(ICC_RPR_EL1) & 0xFF,
         READ_SYSREG(ICC_PMR_EL1) & 0xFF,
         READ_SYSREG(ICC_HPPIR1_EL1) & 0xFFFFFF);
}
