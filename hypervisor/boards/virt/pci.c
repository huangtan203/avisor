// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - PCI/PCIe 直通实现 (Phase 7)
 *
 * 目标设备：QEMU edu 教育设备（vendor 0x1234 / device 0x11e8）
 *
 * 直通三步：
 *   1. 枚举（ECAM）：扫描 bus 0 找到 edu，编程 BAR0=0x10000000，
 *      使能 memory/bus master。
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
#define PCI_IO_ALLOC_END (VIRT_PCIE_IO_SIZE)
#define PCI_MMIO_ALLOC_END (VIRT_PCIE_MMIO_BASE + VIRT_PCIE_MMIO_SIZE)
#define PCI_BRIDGE_MEMORY_GRANULARITY 0x100000
#define PCI_BRIDGE_IO_GRANULARITY 0x1000

struct pci_bar_resource {
    uint64_t address;
    uint64_t size;
    uint32_t flags;
    uint8_t is_64bit;
    uint8_t is_io;
    uint8_t valid;
};

struct pci_device_resource {
    uint16_t vendor;
    uint16_t device;
    uint8_t bus;
    uint8_t dev;
    uint8_t func;
    uint8_t header_type;
    uint8_t owner_vmid;
    uint8_t primary_bus;
    uint8_t secondary_bus;
    uint8_t subordinate_bus;
    struct pci_bar_resource bars[PCI_MAX_BARS];
};

static struct pci_device_resource pci_devices[PCI_MAX_DEVICES];
static unsigned int pci_device_count;
static uint64_t pci_mmio_next = VIRT_PCIE_MMIO_BASE;
static uint32_t pci_io_next;
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

static uint64_t pci_allocate_mmio(uint64_t size)
{
    uint64_t aligned = (pci_mmio_next + size - 1) & ~(size - 1);

    if (!size || (size & (size - 1)) || aligned < pci_mmio_next ||
        aligned > PCI_MMIO_ALLOC_END || size > PCI_MMIO_ALLOC_END - aligned)
        return 0;
    pci_mmio_next = aligned + size;
    return aligned;
}

static uint32_t pci_allocate_io(uint32_t size)
{
    if (!size || (size & (size - 1)))
        return 0xffffffff;
    uint32_t aligned = (pci_io_next + size - 1) & ~(size - 1);

    if (aligned < pci_io_next || aligned >= PCI_IO_ALLOC_END ||
        size > PCI_IO_ALLOC_END - aligned)
        return 0xffffffff;
    pci_io_next = aligned + size;
    return aligned;
}

static int pci_probe_bars(struct pci_device_resource *resource,
                          unsigned int bar_count)
{
    uint32_t command = pci_config_read(resource->bus, resource->dev,
                                       resource->func, PCI_CFG_COMMAND);
    uint32_t original_bars[PCI_MAX_BARS];
    uint64_t saved_mmio_next = pci_mmio_next;
    uint32_t saved_io_next = pci_io_next;
    struct pci_bar_resource saved_resources[PCI_MAX_BARS];
    for (unsigned int i = 0; i < bar_count; i++) {
        original_bars[i] = pci_config_read(resource->bus, resource->dev,
                                           resource->func, PCI_BAR0 + i * 4);
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

        struct pci_bar_resource *bar = &resource->bars[i];
        bar->flags = original & 0xf;
        unsigned int bar_index = i;
        uint64_t size;
        if (original & 1) {
            size = (uint32_t)(~(mask & ~3U) + 1U);
            if (!size || (size & (size - 1)))
                goto rollback;
            uint32_t port = pci_allocate_io(size);
            if (port == 0xffffffff) {
                WARN("PCI: I/O BAR allocation failed at %02x:%02x.%u BAR%u size=0x%lx",
                     resource->bus, resource->dev, resource->func, i,
                     (unsigned long)size);
                goto rollback;
            }
            bar->address = VIRT_PCIE_IO_BASE + port;
            bar->size = size;
            bar->is_io = 1;
            bar->valid = 1;
            pci_config_write(resource->bus, resource->dev, resource->func,
                             reg, port | (original & 3));
            INFO("PCI: %02x:%02x.%u BAR%u I/O port=0x%x CPU=0x%lx/0x%lx",
                 resource->bus, resource->dev, resource->func, bar_index, port,
                 (unsigned long)bar->address, (unsigned long)size);
            continue;
        }

        if (is_64bit) {
            size = ~(((uint64_t)mask_hi << 32) | (mask & ~0xfULL)) + 1;
            bar->is_64bit = 1;
        } else {
            size = (uint32_t)(~(mask & ~0xfU) + 1U);
        }
        if (!size || (size & (size - 1)))
            goto rollback;
        uint64_t assigned = pci_allocate_mmio(size);
        if (!assigned) {
            WARN("PCI: MMIO BAR allocation failed at %02x:%02x.%u BAR%u size=0x%lx",
                 resource->bus, resource->dev, resource->func, i,
                 (unsigned long)size);
            goto rollback;
        }
        bar->address = assigned;
        bar->size = size;
        bar->valid = 1;
        pci_config_write(resource->bus, resource->dev, resource->func, reg,
                         (uint32_t)assigned | bar->flags);
        if (bar->is_64bit)
            pci_config_write(resource->bus, resource->dev, resource->func,
                             reg + 4, (uint32_t)(assigned >> 32));
        INFO("PCI: %02x:%02x.%u BAR%u MMIO 0x%lx/0x%lx",
             resource->bus, resource->dev, resource->func, bar_index,
             (unsigned long)assigned, (unsigned long)size);
        if (bar->is_64bit)
            i++;
    }
    pci_config_write(resource->bus, resource->dev, resource->func,
                     PCI_CFG_COMMAND, command);
    return 0;

rollback:
    for (unsigned int i = 0; i < bar_count; i++) {
        pci_config_write(resource->bus, resource->dev, resource->func,
                         PCI_BAR0 + i * 4, original_bars[i]);
        resource->bars[i] = saved_resources[i];
    }
    pci_mmio_next = saved_mmio_next;
    pci_io_next = saved_io_next;
    pci_config_write(resource->bus, resource->dev, resource->func,
                     PCI_CFG_COMMAND, command);
    WARN("PCI: BAR transaction rolled back for %02x:%02x.%u",
         resource->bus, resource->dev, resource->func);
    return -1;
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
                pci_config_write(bus, dev, func, PCI_CFG_COMMAND,
                                 bridge_command | PCI_CMD_IO | PCI_CMD_MEMORY);
                resource->primary_bus = bus;
                resource->secondary_bus = secondary;
                uint32_t bus_numbers = (bus & 0xff) | (secondary << 8) |
                                       (0xff << 16);
                pci_config_write(bus, dev, func, PCI_BRIDGE_BUS_NUMBERS,
                                 bus_numbers);
                uint64_t mmio_start = pci_mmio_next;
                uint32_t io_start = pci_io_next;
                if (pci_scan_bus(secondary) < 0)
                    return -1;
                unsigned int subordinate = pci_next_bus - 1;
                resource->subordinate_bus = subordinate;
                bus_numbers = (bus & 0xff) | (secondary << 8) |
                              (subordinate << 16);
                pci_config_write(bus, dev, func, PCI_BRIDGE_BUS_NUMBERS,
                                 bus_numbers);

                if (pci_mmio_next > mmio_start) {
                    uint64_t base = mmio_start &
                        ~(uint64_t)(PCI_BRIDGE_MEMORY_GRANULARITY - 1);
                    uint64_t limit = (pci_mmio_next - 1) &
                        ~(uint64_t)(PCI_BRIDGE_MEMORY_GRANULARITY - 1);
                    uint32_t window = (uint32_t)((base >> 16) & 0xfff0) |
                        (uint32_t)((limit >> 16) << 16 & 0xfff00000);
                    pci_config_write(bus, dev, func,
                                     PCI_BRIDGE_MEMORY_BASE_LIMIT, window);
                    INFO("PCI: bridge %02x:%02x.%u memory window 0x%lx-0x%lx",
                         bus, dev, func, (unsigned long)base,
                         (unsigned long)(limit + PCI_BRIDGE_MEMORY_GRANULARITY - 1));
                } else {
                    pci_config_write(bus, dev, func,
                                     PCI_BRIDGE_MEMORY_BASE_LIMIT, 0x0000fff0);
                }
                if (pci_io_next > io_start) {
                    uint32_t base = io_start &
                        ~(PCI_BRIDGE_IO_GRANULARITY - 1);
                    uint32_t limit = (pci_io_next - 1) &
                        ~(PCI_BRIDGE_IO_GRANULARITY - 1);
                    uint32_t io_window = ((base >> 8) & 0xf0) |
                        (limit & 0xf000);
                    pci_config_write(bus, dev, func,
                                     PCI_BRIDGE_IO_BASE_LIMIT, io_window);
                    INFO("PCI: bridge %02x:%02x.%u I/O window 0x%x-0x%x",
                         bus, dev, func, base,
                         limit + PCI_BRIDGE_IO_GRANULARITY - 1);
                } else {
                    pci_config_write(bus, dev, func,
                                     PCI_BRIDGE_IO_BASE_LIMIT, 0x000000f0);
                }
            }
        }
    }
    return 0;
}

static int pci_enumerate(void)
{
    if (pci_scan_bus(0) < 0 || !edu_found)
        return -1;
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

    /* 2. 使能 memory space + bus master */
    uint32_t cmd = pci_config_read(edu_bus, edu_dev, edu_func, PCI_CFG_COMMAND);
    cmd |= PCI_CMD_MEMORY | PCI_CMD_BUSMASTER;
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
        /* MME = 0（1 个向量），MSI Enable = 1 */
        msi_ctrl &= ~(0x7 << PCI_MSI_CTRL_MME_SHIFT);
        msi_ctrl |= PCI_MSI_CTRL_EN;
        pci_config_write(edu_bus, edu_dev, edu_func,
                         edu_msi_cap_off + PCI_MSI_CTRL, msi_ctrl);
        unsigned int owned_vectors = max_vectors > 16 ? 16 : max_vectors;
        for (unsigned int i = 0; i < owned_vectors; i++) {
            if (irq_domain_register(lpi_base + i, 1, 0,
                                    VIRQ_PCI_EDU) < 0) {
                WARN("PCI: edu MSI LPI ownership registration failed");
                return;
            }
        }

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

static int pci_edu_quiesce(void)
{
    uint32_t cmd;
    unsigned int timeout = 100000;

    cmd = pci_config_read(edu_bus, edu_dev, edu_func, PCI_CFG_COMMAND);
    pci_config_write(edu_bus, edu_dev, edu_func, PCI_CFG_COMMAND,
                     (cmd | PCI_CMD_INTX_DIS) & ~PCI_CMD_BUSMASTER);
    if (edu_msi_cap_off) {
        uint32_t msi = pci_config_read(edu_bus, edu_dev, edu_func,
                                       edu_msi_cap_off + PCI_MSI_CTRL);
        pci_config_write(edu_bus, edu_dev, edu_func,
                         edu_msi_cap_off + PCI_MSI_CTRL,
                         msi & ~PCI_MSI_CTRL_EN);
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

static int pci_edu_reinit_msi(void)
{
    unsigned int lpi_base;
    unsigned int vectors = edu_msi_max_vectors > 16 ? 16 : edu_msi_max_vectors;

    if (!edu_msi_cap_off)
        return 0;
    gic_set_spi_target(pci_edu_intx_irq, 0);
    lpi_base = its_setup_pci_msi(edu_bus, edu_dev, edu_func,
                                 edu_msi_max_vectors, 0);
    if (!lpi_base)
        return -1;
    for (unsigned int i = 0; i < vectors; i++)
        if (irq_domain_register(lpi_base + i, 1, 0, VIRQ_PCI_EDU) < 0)
            return -1;
    if (pci_edu_intx_irq >= 32 && pci_edu_intx_irq < 1020) {
        if (irq_domain_register(pci_edu_intx_irq, 1, 0,
                                VIRQ_PCI_EDU) < 0)
            return -1;
        gic_set_spi_target(pci_edu_intx_irq, 0);
        gic_enable_irq(pci_edu_intx_irq);
    }
    uint64_t msi_addr = its_get_msi_address();
    uint32_t data_off = edu_msi_cap_off +
        ((pci_config_read(edu_bus, edu_dev, edu_func,
                          edu_msi_cap_off + PCI_MSI_CTRL) &
          PCI_MSI_CTRL_64BIT) ? 0x0c : PCI_MSI_DATA);
    uint32_t old_data = pci_config_read(edu_bus, edu_dev, edu_func, data_off);
    pci_config_write(edu_bus, edu_dev, edu_func, data_off,
                     (old_data & 0xffff0000) | (its_get_msi_data(0) & 0xffff));
    pci_config_write(edu_bus, edu_dev, edu_func,
                     edu_msi_cap_off + PCI_MSI_ADDR_LO, (uint32_t)msi_addr);
    if (pci_config_read(edu_bus, edu_dev, edu_func,
                        edu_msi_cap_off + PCI_MSI_CTRL) & PCI_MSI_CTRL_64BIT)
        pci_config_write(edu_bus, edu_dev, edu_func,
                         edu_msi_cap_off + PCI_MSI_ADDR_LO + 4,
                         (uint32_t)(msi_addr >> 32));
    uint32_t msi_ctrl = pci_config_read(edu_bus, edu_dev, edu_func,
                                        edu_msi_cap_off + PCI_MSI_CTRL);
    msi_ctrl &= ~(0x7 << PCI_MSI_CTRL_MME_SHIFT);
    pci_config_write(edu_bus, edu_dev, edu_func,
                     edu_msi_cap_off + PCI_MSI_CTRL,
                     msi_ctrl | PCI_MSI_CTRL_EN);
    uint32_t cmd = pci_config_read(edu_bus, edu_dev, edu_func,
                                   PCI_CFG_COMMAND);
    pci_config_write(edu_bus, edu_dev, edu_func, PCI_CFG_COMMAND,
                     cmd | PCI_CMD_MEMORY | PCI_CMD_BUSMASTER |
                     PCI_CMD_INTX_DIS);
    pci_edu_lpi_base = lpi_base;
    pci_edu_msi_enabled = 1;
    INFO("PCI: edu MSI reinitialized (LPI base=%u)", lpi_base);
    return 0;
}

int pci_edu_vm_reset(struct task_struct *vm)
{
    if (!vm || vm->vmid != 1 || !edu_found || pci_edu_quiesce() < 0)
        return -1;
    if (pci_edu_intx_irq >= 32 && pci_edu_intx_irq < 1020) {
        gic_disable_irq(pci_edu_intx_irq);
        put32(GICD_ICPENDR(pci_edu_intx_irq / 32),
              1U << (pci_edu_intx_irq % 32));
        irq_domain_unregister(pci_edu_intx_irq);
    }
    if (pci_edu_lpi_base) {
        its_teardown_pci_msi(pci_edu_requester_id(), pci_edu_lpi_base, 16);
        for (unsigned int i = 0; i < 16; i++)
            vits_lpi_revoke(pci_edu_lpi_base + i);
    }
    pci_edu_lpi_base = 0;
    pci_edu_msi_enabled = 0;
    INFO("PCI: edu IRQ resources released for VM%lu", vm->vmid);
    return 0;
}

int pci_edu_function_reset(struct task_struct *vm)
{
    if (!vm || vm->vmid != 1 || !edu_found)
        return -1;
    if (pci_edu_quiesce() < 0) {
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
        its_teardown_pci_msi(pci_edu_requester_id(), pci_edu_lpi_base, 16);
        for (unsigned int i = 0; i < 16; i++)
            vits_lpi_revoke(pci_edu_lpi_base + i);
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
    pci_config_write(edu_bus, edu_dev, edu_func, PCI_CFG_COMMAND, cmd);
    pci_config_write(edu_bus, edu_dev, edu_func, PCI_CFG_COMMAND,
                     cmd | PCI_CMD_MEMORY | PCI_CMD_BUSMASTER |
                     PCI_CMD_INTX_DIS);
    pci_edu_lpi_base = 0;
    pci_edu_msi_enabled = 0;
    if (pci_edu_reinit_msi() < 0) {
        WARN("PCI: edu MSI reinitialization failed after reset");
        return -1;
    }
    INFO("PCI: edu function reset complete for VM%lu (%s)", vm->vmid,
         edu_flr_supported ? "PCIe FLR" : "edu internal fallback");
    return 0;
}

int pci_passthrough_map(struct task_struct *vm)
{
    if (!vm || vm->vmid != 1 || !edu_found || !edu_bar0 ||
        !edu_bar0_size || (edu_bar0 & (PAGE_SIZE - 1)) ||
        (edu_bar0_size & (PAGE_SIZE - 1)) ||
        edu_bar0 < VIRT_PCIE_MMIO_BASE || edu_bar0 > PCI_MMIO_ALLOC_END ||
        edu_bar0_size > PCI_MMIO_ALLOC_END - edu_bar0)
        return -1;

    uint64_t ecam = ecam_addr(edu_bus, edu_dev, edu_func, 0) &
                    ~(uint64_t)(PAGE_SIZE - 1);
    if (ecam < VIRT_PCIE_ECAM_BASE ||
        ecam >= VIRT_PCIE_ECAM_BASE + VIRT_PCIE_ECAM_SIZE)
        return -1;

    uint64_t mapped_bar_size = 0;
    if (stage2_page_is_mapped(vm, ecam))
        return -1;
    map_stage2_page(vm, ecam, ecam, MMU_STAGE2_DEVICE_PAGE_FLAGS);
    for (uint64_t off = 0; off < edu_bar0_size; off += PAGE_SIZE) {
        uint64_t ipa = edu_bar0 + off;
        if (stage2_page_is_mapped(vm, ipa))
            goto rollback;
        map_stage2_page(vm, ipa, ipa, MMU_STAGE2_DEVICE_PAGE_FLAGS);
        mapped_bar_size += PAGE_SIZE;
    }
    INFO("PCI: passthrough mapped into VM%lu (ECAM page=0x%lx BAR0=0x%lx/0x%lx)",
         (unsigned long)vm->vmid, (unsigned long)ecam,
         (unsigned long)edu_bar0, (unsigned long)edu_bar0_size);
    return 0;

rollback:
    while (mapped_bar_size) {
        mapped_bar_size -= PAGE_SIZE;
        unmap_stage2_page(vm, edu_bar0 + mapped_bar_size, 0);
    }
    unmap_stage2_page(vm, ecam, 0);
    WARN("PCI: passthrough mapping rolled back for VM%lu",
         (unsigned long)vm->vmid);
    return -1;
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
