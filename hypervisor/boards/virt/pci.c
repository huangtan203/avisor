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
#include "boards/virt/its.h"
#include "arch/aarch64/mmu.h"
#include "arch/aarch64/sysregs.h"
#include "common/mm.h"
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

/* MSI Capability 寄存器偏移（32 位消息地址模式） */
#define PCI_MSI_CTRL       0x02  /* Message Control（offset 相对于 cap 头部） */
#define PCI_MSI_ADDR_LO    0x04  /* Message Address（32位） */
#define PCI_MSI_DATA       0x08  /* Message Data（32位） */
#define PCI_MSI_MASK       0x0c  /* Mask Bits（可选，取决于 MSC） */
#define PCI_MSI_PEND       0x10  /* Pending Bits（可选） */

/* MSI Message Control 位 */
#define PCI_MSI_CTRL_EN          (1 << 0)   /* MSI Enable */
#define PCI_MSI_CTRL_MMC_SHIFT   1          /* Multiple Message Capable（位宽） */
#define PCI_MSI_CTRL_MME_SHIFT   4          /* Multiple Message Enable */
#define PCI_MSI_CTRL_64BIT       (1 << 7)   /* 64-bit Address */
#define PCI_MSI_CTRL_MASK        (1 << 8)   /* Per-vector Masking */

/* edu 设备位置（枚举时填充） */
static unsigned int edu_bus;
static unsigned int edu_dev;
static unsigned int edu_func;
static int edu_found = 0;

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

/*
 * 枚举 bus 0，找到 edu 设备（vendor 0x1234 / device 0x11e8）
 */
static int pci_find_edu(void)
{
    for (unsigned int dev = 0; dev < 32; dev++) {
        /* 先看 function 0：若不存在（vendor == 0xffff）则跳过该 dev */
        uint32_t id0 = pci_config_read(0, dev, 0, PCI_CFG_VENDOR_ID);
        if ((id0 & 0xffff) == 0xffff)
            continue;

        for (unsigned int func = 0; func < 8; func++) {
            uint32_t id = pci_config_read(0, dev, func, PCI_CFG_VENDOR_ID);
            uint16_t vendor = (uint16_t)(id & 0xffff);
            uint16_t device = (uint16_t)((id >> 16) & 0xffff);

            if (vendor == PCI_EDU_VENDOR && device == PCI_EDU_DEVICE) {
                edu_bus = 0;
                edu_dev = dev;
                edu_func = func;
                return 1;
            }
        }
    }
    return 0;
}

void pci_init(void)
{
    if (!pci_find_edu()) {
        WARN("PCI: edu device not found (vendor=0x%lx device=0x%lx)",
             (unsigned long)PCI_EDU_VENDOR, (unsigned long)PCI_EDU_DEVICE);
        return;
    }
    edu_found = 1;
    INFO("PCI: edu found at %02x:%02x.%x", edu_bus, edu_dev, edu_func);

    /* 1. 编程 BAR0 = 0x10000000（PCI MMIO 窗口起始） */
    pci_config_write(edu_bus, edu_dev, edu_func, PCI_CFG_BAR0,
                     (uint32_t)PCI_EDU_BAR0_IPA);
    uint32_t bar0 = pci_config_read(edu_bus, edu_dev, edu_func, PCI_CFG_BAR0);
    INFO("PCI: edu BAR0 = 0x%x", bar0);

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
    }

    /* 4. 把 INTx 物理中断路由到 CPU0 并使能 */
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

            if (cap_id == PCI_CAP_ID_MSI) {
                edu_msi_cap_off = cap_off;
                break;
            }
            cap_off = next_ptr;
        }

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

void pci_passthrough_map(struct task_struct *vm)
{
    if (!edu_found)
        return;

    /*
     * 映射 ECAM（16MB）为 device 内存，Guest 可自行枚举配置空间。
     * 等值映射：IPA == PA。
     */
    for (uint64_t off = 0; off < VIRT_PCIE_ECAM_SIZE; off += PAGE_SIZE) {
        map_stage2_page(vm, VIRT_PCIE_ECAM_BASE + off,
                        VIRT_PCIE_ECAM_BASE + off,
                        MMU_STAGE2_DEVICE_PAGE_FLAGS);
    }

    /* 映射 BAR0（1MB）为 device 内存 */
    for (uint64_t off = 0; off < PCI_EDU_BAR0_SIZE; off += PAGE_SIZE) {
        map_stage2_page(vm, PCI_EDU_BAR0_IPA + off,
                        PCI_EDU_BAR0_IPA + off,
                        MMU_STAGE2_DEVICE_PAGE_FLAGS);
    }

    INFO("PCI: passthrough mapped into VM%lu (ECAM 16MB + BAR0 1MB)",
         (unsigned long)vm->vmid);
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
             get32(PCI_EDU_BAR0_IPA + 0x24));
    INFO("PCI dbg: LR0=0x%lx LR1=0x%lx",
         READ_SYSREG64(ICH_LR0_EL2), READ_SYSREG64(ICH_LR1_EL2));
    INFO("PCI dbg: RPR=0x%02x PMR=0x%02x HPPIR=%u",
         READ_SYSREG(ICC_RPR_EL1) & 0xFF,
         READ_SYSREG(ICC_PMR_EL1) & 0xFF,
         READ_SYSREG(ICC_HPPIR1_EL1) & 0xFFFFFF);
}
