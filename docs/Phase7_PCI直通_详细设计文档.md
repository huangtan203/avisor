# aVisor Hypervisor Phase 7 详细设计文档 (PCI 直通)

> 版本：v0.7.0 (Phase 7)
> 平台：QEMU virt (ARMv8-A, GICv3, 虚拟化扩展)
> 目标：基于 QEMU `edu` 教育 PCI 设备实现完整的 PCI 直通（ECAM 配置空间 + BAR0 MMIO + INTx 中断转发），验证 GICv3 虚拟化下物理中断与虚拟中断的优先级交互机制

---

## 1. 概述

### 1.1 目标达成情况

| 目标 | 状态 | 实现方式 |
|------|------|---------|
| ECAM 配置空间枚举 | ✅ | `pci_find_edu()` 通过 ECAM 扫描 bus 0，匹配 vendor 0x1234 / device 0x11e8 |
| BAR0 编程与使能 | ✅ | 写 BAR0 = 0x10000000，置 command 寄存器 MemorySpace + BusMaster |
| ECAM + BAR0 Stage 2 映射 | ✅ | `pci_passthrough_map()` 用 `MMU_STAGE2_DEVICE_PAGE_FLAGS`（AP=11 + device memattr）等值映射 |
| INTx 中断号运行时计算 | ✅ | 读配置空间 interrupt pin，经 PCI swizzle（`host_pin = (pin-1 + slot) & 3`）算出主桥引脚 + GIC SPI |
| IROUTER 亲和路由 | ✅ | `gic_set_spi_target()` 写 `GICD_IROUTER<n>`（n 为完整 INTID），路由到 CPU0 |
| INTx 物理中断 → 虚拟中断转发 | ✅ | `irq.c` 中检测到 edu INTx → 调用 `vgic_inject_virq_hw_eoi()` 写入 LR2（HW 映射模式） |
| split EOI + HW 联动去活 | ✅ | 写 EOIR 只降优先级，Guest 写虚拟 EOIR 时经 LR HW 联动自动 deactivate 物理 SPI |
| Guest 端 edu 驱动 | ✅ | identification / liveness / factorial / INTx 四项测试 |
| 优先级抢占问题修复 | ✅ | 直通 SPI 优先级设为 0x60（高于虚拟中断默认的 0x80），确保物理中断可抢占 EL1 |

### 1.2 验证结果

**PCI 直通完整验证（VM1）：**
```
[vm1] --- Test 3: PCI passthrough (edu) ---
[vm1] edu: probing device at BAR0=0x10000000...
[vm1] edu: identification=0x010000ed
[vm1] edu: liveness PASSED (0x21524110)
[vm1] edu: factorial(10)=3628800 (expect 3628800)
[vm1] edu: factorial PASSED
[vm1] edu: starting INTx interrupt test...
[vm1] edu: INTx interrupt PASSED (irq_status=0x12345678)
```
- MMIO 直通：identification / liveness / factorial 三项全部通过，证明 BAR0 读写完全直通
- INTx 直通：写 `IRQ_RAISE` 寄存器拉高电平 → 物理 SPI 37 触发 → EL2 注入虚拟中断 → Guest ISR 清 `IRQ_ACK` → HW 联动去活，完整闭环

**回归验证（Phase 4-6 功能不受影响）：**
```
[vm1] virtio-net: ALL loopback tests PASSED!
[vm2] virtio-net: ALL loopback tests PASSED!
```
- virtio-net 环回、虚拟定时器 tick、多 VM 调度均正常
- SGI 核间通信（次核启动）正常

### 1.3 修改的关键文件

```
avisor/
├── include/
│   ├── common/
│   │   └── hvc.h               # ★ 新增 HVC_PCI_DEBUG（诊断 hypercall）
│   ├── arch/aarch64/
│   │   └── sysregs.h           # ★ 新增 ICH_LR2_EL2、ICH_LR_HW、pINTID 字段定义
│   └── boards/virt/
│       ├── base.h              # ★ 新增 PCI ECAM/MMIO 窗口、edu 设备 ID/BAR0、INTX_SPI_BASE
│       ├── gicv3.h             # ★ 新增 GICD_IROUTER、gic_set_spi_target、ICC_RPR/HPPIR/AP1R 等调试寄存器
│       ├── vgic.h              # ★ 新增 VIRQ_PCI_EDU(50)、vgic_inject_virq_hw_eoi()、NR_VGIC_LRS=3
│       └── pci.h               # ★ 新增：pci_init、pci_passthrough_map、pci_debug_dump、pci_edu_intx_irq
├── hypervisor/
│   ├── arch/aarch64/
│   │   └── sync_exc.c          # ★ HVC 处理新增 HVC_PCI_DEBUG case
│   └── boards/virt/
│       ├── boot_virt.S         # ★ Stage-1 MMIO PMD 表新增 ECAM + BAR0 窗口映射
│       ├── gicv3.c             # ★ 新增 gic_set_spi_target()（IROUTER 亲和路由）
│       ├── vgic.c              # ★ 扩展到 3 LR；新增 vgic_inject_virq_hw_eoi(LR2)
│       ├── pci.c               # ★ 新增：ECAM 枚举、BAR 编程、swizzle、stage-2 映射、诊断 dump
│       ├── irq.c               # ★ 新增 PCI INTx 物理中断处理（HW 映射注入 + split EOI）
│       ├── virt_board.c        # ★ VM1 初始化时调用 pci_passthrough_map；离开 VM 时清理 LR2
│       └── main_virt.c         # ★ 新增 pci_init() 调用
├── guests/el1_test/
│   ├── guest.h                 # ★ 新增 edu 寄存器定义、PCI_EDU_IRQ、HVC_PCI_DEBUG
│   └── main.c                  # ★ 新增 edu 驱动 + 4 项测试
└── Makefile.virt               # ★ run/debug target 新增 -device edu、-smp 2、highmem=off
```

---

## 2. 总体架构

### 2.1 PCI 直通数据流

```
┌─────────────────────────────────────────────────────────────────────┐
│                          Guest VM1 (EL1)                            │
│  ┌───────────┐    ┌────────────────────┐    ┌───────────────────┐  │
│  │ edu 驱动  │───►│  0x10000000 (BAR0) │    │  0x3f000000 ECAM │  │
│  └───────────┘    └─────────┬──────────┘    └─────────┬─────────┘  │
│      ▲                       │ Stage-2 等值映射          │            │
│      │ 虚拟 SPI 50            ▼                          ▼            │
│  ┌───┴────────┐        ┌───────────────────────────────────┐         │
│  │ vGIC LR2   │        │   物理设备 (QEMU edu)             │         │
│  │ HW 映射    │◄───────┤   BAR0: 0x10000000 (1MB MMIO)    │         │
│  │ pINTID=37  │        │   INTA  ---->  GIC SPI 5 (全局37) │         │
│  └────────────┘        └───────────────┬───────────────────┘         │
└─────────────────────────────────────────┼─────────────────────────────┘
                                          │
                        ┌─────────────────┴──────────────────┐
                        │  Hypervisor (EL2)                  │
                        │  ┌──────────────────────────────┐  │
                        │  │  irq.c handle_irq()          │  │
                        │  │  ack 物理 37 → 注入 LR2      │  │
                        │  │  EOIR (priority drop only)   │  │
                        │  └───────────────┬──────────────┘  │
                        └──────────────────┼─────────────────┘
                                           │
                                ┌──────────▼──────────┐
                                │  GICv3 (split EOI)  │
                                │  物理 37 保持 active │
                                │  Guest 虚拟 EOI     │
                                │  → HW 联动 deactivate│
                                └─────────────────────┘
```

### 2.2 核心设计原则

1. **完整直通，最小陷获**：ECAM + BAR0 全部通过 Stage-2 等值映射为 device 内存，Guest 直接读写物理设备，零陷获开销。
2. **INTx 走 HW 映射 vGIC 通道**：与 vtimer（PPI 27）类似，使用 List Register 的 HW 模式，Guest 写虚拟 EOI 自动联动去活物理中断，避免电平风暴。
3. **中断号运行时计算**：不硬编码 INTx 引脚，而是通过 PCI 标准 swizzle 函数从设备 slot + pin 动态算出，符合真实 PCI 架构。
4. **优先级差异化设计**：直通物理中断优先级（0x60）必须高于虚拟中断默认优先级（0x80），确保物理中断能抢占 EL1 guest 上下文。
5. **per-VM 直通隔离**：仅 VM1 映射 edu 的 ECAM 和 BAR0，其他 VM 的 Stage-2 页表中无该设备区域，天然隔离。

---

## 3. PCI 与 QEMU virt 平台基础

### 3.1 QEMU virt 平台的 PCIe 布局（highmem=off）

```
0x1000_0000 ────────────────────────────────────────── 0x3eff_ffff
  │              PCI MMIO 窗口 (752MB)                  │
  │  设备 BAR 映射的目标区域，由软件编程 BAR 寄存器       │
  └─────────────────────────────────────────────────────┘

0x3f00_0000 ────────────────────────────────────────── 0x3fff_ffff
  │              ECAM 配置空间 (16MB)                   │
  │  addr = base + (bus<<20 | dev<<15 | func<<12 | reg) │
  └─────────────────────────────────────────────────────┘
```

- `highmem=off` 把所有 PCI 相关地址压在 4GB 以下，方便 Stage-2 的 38-bit IPA 空间直接等值映射
- ECAM：Enhanced Configuration Access Mechanism，每设备 4KB 配置空间，bus 0 有 32 dev × 8 func = 256 个设备

### 3.2 edu 教育设备

QEMU 内置的简易 PCI 教学设备：

| 字段 | 值 |
|------|-----|
| Vendor ID | 0x1234 |
| Device ID | 0x11e8 |
| BAR0 | 1MB MMIO |
| 中断 | INTA（传统 INTx 线中断） |

**BAR0 寄存器布局：**

| 偏移 | 名称 | 类型 | 说明 |
|------|------|------|------|
| 0x00 | identification | RO | 固定值 0x010000ed（设备身份验证） |
| 0x04 | liveness | RW | 写入 v，读出 ~v（连通性测试） |
| 0x08 | factorial | RW | 写入 n，计算 n!，状态位在 status[0] |
| 0x20 | status | RO | bit0 = 计算中，bit7 = IRQFACT |
| 0x24 | irq_status | RO | 当前中断状态位图 |
| 0x60 | irq_raise | WO | 写入 val → irq_status |= val，非零则拉高 INTx |
| 0x64 | irq_ack | WO | 写入 val → irq_status &= ~val，归零则拉低 INTx |

### 3.3 PCI INTx 与 GIC 的连接

QEMU virt 的 GPEX 根桥使用标准 PCI swizzle：

```
host_pin = (slot + device_pin) & 3
```

其中：
- `device_pin`：0=INTA, 1=INTB, 2=INTC, 3=INTD（从配置空间 offset 0x3d 读取，值为 1-4，需减 1）
- `slot`：设备号（dev）
- `host_pin`：根桥的 INTx 线编号（0=INTA, 1=INTB, 2=INTC, 3=INTD）

根桥的四根 INTx 线分别连接到 GIC 的 SPI 3/4/5/6（全局中断号 35/36/37/38）：

```
主桥 INTA → SPI 3 → 全局 35
主桥 INTB → SPI 4 → 全局 36
主桥 INTC → SPI 5 → 全局 37
主桥 INTD → SPI 6 → 全局 38
```

**示例：edu 设备在 slot 2，使用 INTA（pin=1 → device_pin=0）**
- host_pin = (2 + 0) & 3 = 2 → 主桥 INTC
- GIC SPI = 3 + 2 = 5 → 全局中断号 = 32 + 5 = **37**

---

## 4. PCI 枚举与 BAR 编程

### 4.1 ECAM 配置空间访问

```c
static inline uint64_t ecam_addr(unsigned int bus, unsigned int dev,
                                 unsigned int func, unsigned int reg)
{
    return VIRT_PCIE_ECAM_BASE +
           (((uint64_t)bus << 20) | ((uint64_t)dev << 15) |
            ((uint64_t)func << 12) | (uint64_t)reg);
}
```

ECAM 地址计算完全符合 PCIe 规范：
- bus: 8 位（<< 20）
- dev: 5 位（<< 15）
- func: 3 位（<< 12）
- reg: 12 位（offset 0-4095）

### 4.2 设备枚举

```c
static int pci_find_edu(void)
{
    for (unsigned int dev = 0; dev < 32; dev++) {
        uint32_t id0 = pci_config_read(0, dev, 0, PCI_CFG_VENDOR_ID);
        if ((id0 & 0xffff) == 0xffff)
            continue;  // 该 slot 无设备

        for (unsigned int func = 0; func < 8; func++) {
            uint32_t id = pci_config_read(0, dev, func, PCI_CFG_VENDOR_ID);
            uint16_t vendor = (uint16_t)(id & 0xffff);
            uint16_t device = (uint16_t)((id >> 16) & 0xffff);

            if (vendor == PCI_EDU_VENDOR && device == PCI_EDU_DEVICE) {
                edu_bus = 0; edu_dev = dev; edu_func = func;
                return 1;
            }
        }
    }
    return 0;
}
```

**枚举策略：**
1. 只扫描 bus 0（单总线，QEMU virt 根桥上的设备都在 bus 0）
2. 先读 function 0 的 vendor ID，若为 0xFFFF 则整个 dev 都不存在，跳过
3. 每个 dev 扫描 8 个 func（兼容多功能设备）
4. 匹配 vendor=0x1234 且 device=0x11e8 即为 edu

### 4.3 BAR 编程与命令使能

```c
/* 编程 BAR0 = 0x10000000 */
pci_config_write(edu_bus, edu_dev, edu_func, PCI_CFG_BAR0,
                 (uint32_t)PCI_EDU_BAR0_IPA);

/* 使能 memory space + bus master */
uint32_t cmd = pci_config_read(edu_bus, edu_dev, edu_func, PCI_CFG_COMMAND);
cmd |= PCI_CMD_MEMORY | PCI_CMD_BUSMASTER;
pci_config_write(edu_bus, edu_dev, edu_func, PCI_CFG_COMMAND, cmd);
```

- **BAR0 编程**：把设备的 MMIO 基地址设为 `0x10000000`（PCI MMIO 窗口起始），Guest 的 IPA 也使用同一地址（等值映射）
- **Memory Space Enable**：置位后设备才响应 BAR 范围内的 MMIO 访问
- **Bus Master Enable**：置位后设备可发起 DMA（当前 edu 用不到，但标准流程应该置位）

---

## 5. Stage-2 直通映射

### 5.1 映射策略

```c
void pci_passthrough_map(struct task_struct *vm)
{
    /* 映射 ECAM（16MB）为 device 内存 */
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
}
```

### 5.2 Stage-2 Device Page Flags

```c
#define MMU_STAGE2_DEVICE_PAGE_FLAGS \
    (MMU_STAGE2_AP_RW_EL1 |            /* AP=11: EL1 读写 */ \
     (0b001ULL << 2) |                 /* MemAttr: Device-nGnRnE */ \
     (0b01ULL << 0))                   /* 有效页 */
```

**与 MMIO 陷获页的区别：**

| 标志 | 用途 | AP | 结果 |
|------|------|----|------|
| `MMU_STAGE2_MMIO_PAGE_FLAGS` | trap-and-emulate（virtio-net 等） | AP=00（无访问） | Guest 访问触发 Stage-2 fault → 陷获到 EL2 模拟 |
| `MMU_STAGE2_DEVICE_PAGE_FLAGS` | 直通（PCI 设备） | AP=11（完全访问） | Guest 直接读写物理设备，零陷获开销 |

> **历史教训**：最初误用了 `MMU_STAGE2_MMIO_PAGE_FLAGS` 映射 PCI 直通区域，导致 Guest 读 BAR0 全部返回 0（陷获后 MMIO 处理函数不识别该地址，返回 0），identification 测试失败。改为 DEVICE 页标志后立即恢复正常。

---

## 6. INTx 中断直通

### 6.1 中断号运行时计算

```c
uint32_t intr = pci_config_read(edu_bus, edu_dev, edu_func, PCI_CFG_INTR);
uint8_t pin = (uint8_t)(intr >> 8);  // interrupt pin: 1=INTA...4=INTD

unsigned int host_pin = ((unsigned int)pin - 1 + edu_dev) & 3;
pci_edu_intx_irq = 32 + VIRT_PCIE_INTX_SPI_BASE + host_pin;
```

- 从配置空间 `0x3c` 寄存器的高 8 位读取 interrupt pin（1-4）
- 减 1 转成 0-based device_pin
- 经标准 PCI swizzle 算出 host_pin（根桥的 INTx 线编号）
- 加上 `VIRT_PCIE_INTX_SPI_BASE`（主桥 INTA 对应的 SPI 号 = 3）和 32 得到全局中断号

### 6.2 亲和路由（IROUTER）

```c
void gic_set_spi_target(unsigned int irq, unsigned int cpu_id)
{
    if (irq < 32) return;  // SGI/PPI 没有 IROUTER

    uint64_t router = GICD_IROUTER(irq);
    uint32_t aff = (cpu_id & 0xFF);  /* Aff0 */
    put32(router, aff);
    put32(router + 4, 0);  /* Aff3:Aff2:Aff1 高位清零 */
}
```

**关键注意点**：`GICD_IROUTER<n>` 按**完整 INTID** 编址：
- `offset = 0x6000 + 8 × INTID`
- INTID 0-31（SGI/PPI）的槽位 RAZ/WI（QEMU 直接忽略写入）
- 第一个 SPI（INTID 32）位于 `0x6000 + 8*32 = 0x6100`

> **历史教训**：最初实现错误地用 `irq - 32` 作索引，把 SPI 5（全局 37）写到了 IROUTER[5]（SGI 5 的槽位，RAZ/WI），导致中断始终路由到默认 CPU，表现为 IROUTER 写入后读出仍为 0。改为直接使用完整 INTID 后正常。

### 6.3 物理中断处理（irq.c）

```c
if (pci_edu_intx_irq != 0 && irq == pci_edu_intx_irq) {
    if (current && current->pid != 0 && current->vmid == 1) {
        /* 目标 VM 上下文：注入 HW 映射虚拟中断 */
        vgic_inject_virq_hw_eoi(VIRQ_PCI_EDU, pci_edu_intx_irq);
        gic_eoi_irq(irq);   /* split EOI: 只降优先级，物理中断保持 active */
        return;              /* 跳过末尾公共 EOI */
    }
    /* 非目标 VM 上下文：显式去活，避免电平中断卡在 active */
    WRITE_SYSREG64(pci_edu_intx_irq, ICC_DIR_EL1);
    asm volatile("isb");
    gic_eoi_irq(irq);
    return;
}
```

**两种上下文处理：**

| 上下文 | 处理方式 | 原因 |
|--------|---------|------|
| VM1（直通该设备） | 注入 LR2 + EOIR（仅 priority drop） | 由 Guest 虚拟 EOI 通过 HW 联动去活物理中断 |
| idle / 其他 VM | DIR deactivate + EOIR | 没有对应的虚拟中断，必须手动去活，否则电平中断无法重新 pending |

### 6.4 vGIC HW 映射注入（LR2）

```c
void vgic_inject_virq_hw_eoi(uint32_t vintid, uint32_t pintid)
{
    uint64_t lr = ((uint64_t)vintid & 0xffffffffULL)  /* vINTID [31:0] */
                | ((uint64_t)pintid << 32)             /* pINTID [41:32] */
                | ICH_LR_GROUP1                        /* Group 1 */
                | ICH_LR_HW                            /* HW 映射模式 */
                | ICH_LR_PENDING;                       /* state = pending */

    write_lr(2, lr);  /* 固定使用 LR2 */
    asm volatile("isb");
}
```

**ICH_LR_EL2 HW 模式字段：**

| 字段 | 位域 | 说明 |
|------|------|------|
| vINTID | [31:0] | 虚拟中断号（Guest 看到的） |
| pINTID | [41:32] | 物理中断号（HW 联动用） |
| HW | bit 48 | 1 = 硬件映射模式 |
| State | [62:61] | 00=invalid, 01=pending, 10=active, 11=pending+active |

> **历史教训**：最初错误地给 HW 映射的 LR 设置了 bit 41（误以为是 EOI 标志位）。实际上 bit 41 是 pINTID 字段的 MSB，设置后会导致 pINTID 变成一个很大的错误值，HW 联动失效。HW 映射模式下，Guest 写虚拟 EOI 时硬件自动 deactivate 物理中断，无需手动设置任何 EOI 位。

### 6.5 世界切换时的清理

```c
/* virt_leaving_vm() 中 */
if (pci_edu_intx_irq != 0 && (lr2 & ICH_LR_HW) &&
    (lr2 & ICH_LR_STATE_MASK)) {
    uint32_t pintid = (uint32_t)(lr2 >> 32) & 0x3ff;
    if (pintid == pci_edu_intx_irq) {
        WRITE_SYSREG64(pci_edu_intx_irq, ICC_DIR_EL1);
        asm volatile("isb");
    }
}
```

离开 VM 时，如果 LR2 中的 PCI INTx 硬件映射仍处于非 invalid 状态，必须手动 deactivate 对应的物理中断。否则物理 INTx 会一直保持 active，导致该中断号后续无法再次触发。

---

## 7. 优先级抢占问题（关键设计决策）

### 7.1 问题现象

PCI INTx 物理中断（SPI 37）在 GICD 中处于 pending + enabled 状态，但**在 EL1 guest 上下文中永远不会触发**。只有当 Guest 主动陷入 EL2（如 WFI、HVC、其他异常）后，中断才会被处理。

调试时观察到的关键状态：
```
GICD: en1=0x20  pend1=0x20  act1=0x0    ← SPI 37 pending，但不 active
CPU IF: RPR=0x80  PMR=0xF0  HPPIR=27    ← 运行优先级 0x80，最高优先级挂起是 27
LR1: 0x1000000000000030                   ← virtio-net 虚拟中断 pending（优先级 0x80）
```

### 7.2 根因分析

这是 GICv3 虚拟化架构的基本约束：

> **当 List Register 中有 pending 状态的虚拟中断时，物理 CPU 接口的运行优先级（RPR）会被降到该虚拟中断的优先级水平。**

机制详解：
1. Guest 运行时，LR1 中有一个 pending 的 virtio-net 虚拟中断（优先级 0x80）
2. 为了让 Guest 的虚拟 GIC 能"看到"这个虚拟中断，物理 RPR 被降到 0x80
3. 物理 SPI 37 的优先级也是 0x80，**同等优先级不能抢占**
4. 因此物理中断永远 pending，永远不会触发 IRQ 异常

这不是 bug，而是 GICv3 虚拟化的设计：虚拟中断和物理中断共享同一个物理 CPU 接口的优先级仲裁逻辑。

### 7.3 解决方案

将直通物理中断的优先级设置为**高于**虚拟中断的默认优先级：

```c
gic_set_priority(pci_edu_intx_irq, 0x60);  /* 物理: 0x60 (更高) */
```

- 虚拟中断默认优先级：0x80
- 物理直通中断优先级：**0x60**（数值越小优先级越高）
- 这样即使 LR 中有 pending 的虚拟中断（RPR=0x80），物理 SPI 37（优先级 0x60）也能抢占，触发 IRQ 进入 EL2

### 7.4 优先级设计原则

对于使用 HW 映射直通的物理中断，优先级配置应遵循：

| 中断类型 | 推荐优先级 | 原因 |
|---------|-----------|------|
| 直通物理中断 (HW 映射) | 0x40 - 0x60 | 必须高于虚拟中断，保证物理中断能抢占 EL1 |
| 虚拟定时器 (HW 映射) | 0x80（默认） | PPI 本身优先级就是 0x80，与物理一致 |
| 纯软件虚拟中断 | 0x80（默认） | 普通设备中断，不要求抢占物理中断 |

---

## 8. Guest 端 edu 驱动

### 8.1 中断处理流程

```c
void guest_irq_handler(void)
{
    u64 iar = icc_iar1_read();
    u32 intid = (u32)iar;
    ...
    } else if (intid == PCI_EDU_IRQ) {
        /* 先拉低物理 INTx 线（清 irq_status），再走末尾统一虚拟 EOI，
         * 避免 HW 联动去活物理中断后电平仍未清除导致重触发风暴 */
        edu_irq_status_val = mmio_read32(EDU_BAR0_BASE + EDU_IRQ_STATUS);
        mmio_write32(EDU_BAR0_BASE + EDU_IRQ_ACK, edu_irq_status_val);
        edu_irq_received = 1;
    }
    icc_eoir1_write(iar);  /* 虚拟 EOI → HW 联动去活物理 SPI 37 */
}
```

**关键顺序**：先写 `IRQ_ACK` 清除设备端的中断状态（拉低 INTx 电平），**再**写 EOIR 做虚拟中断结束。

如果顺序相反（先 EOI 再清设备），HW 联动去活物理中断后，设备电平仍为高，GIC 会立刻重新 pending 该中断 → 又一次虚拟注入 → 形成中断风暴。

### 8.2 测试项

| 测试项 | 验证内容 | 通过标准 |
|--------|---------|---------|
| identification | BAR0 可读，设备 ID 正确 | 值 == 0x010000ed |
| liveness | BAR0 读写正常（写 v 读 ~v） | 写入 0xDEADBEEF，读出取反后匹配 |
| factorial | 设备计算功能正常 | 写入 10，等待状态位清零，结果 == 3628800 |
| INTx interrupt | 中断直通完整闭环 | 写 IRQ_RAISE → ISR 被调用 → irq_status 值匹配 |

---

## 9. 构建与运行

### 9.1 编译

```bash
make -f Makefile.virt clean all
```

### 9.2 QEMU 运行参数

```makefile
run:
	qemu-system-aarch64 \
		-M virt,virtualization=on,gic-version=3,highmem=off \
		-cpu max \
		-smp 2 \
		-m 1G \
		-device edu \
		-kernel bin_virt/kernel8.img \
		-nographic \
		-serial mon:stdio
```

关键参数说明：
- `-device edu`：添加 edu 教育 PCI 设备
- `highmem=off`：PCI 区域全部位于 4GB 以下，方便 Stage-2 映射
- `-smp 2`：双核（PCI 直通运行在 CPU0，次核用于 SGI 验证）

### 9.3 调试诊断

Guest 中 INTx 测试超时后会自动调用 `HVC_PCI_DEBUG`，Hypervisor 端 dump 完整的中断链路状态：

```
PCI dbg: intx_irq=37
PCI dbg: GICD en1=0x20 pend1=0x20 act1=0x0
PCI dbg: IROUTER[37]=0x0_00000000
PCI dbg: edu irq_status=0x12345678
PCI dbg: LR0=0x0 LR1=0x1000000000000030
PCI dbg: RPR=0x80 PMR=0xf0 HPPIR=27
```

可快速定位是哪一环节出了问题（设备端 / GICD 配置 / 路由 / CPU 接口优先级 / vGIC LR）。

---

## 10. 已知限制与未来优化

### 10.1 当前限制

| 限制 | 说明 |
|------|------|
| 仅支持 INTx 线中断 | 未实现 MSI/MSI-X，需要 ITS（Interrupt Translation Service）支持 |
| 单设备直通 | 只直通了一个 edu 设备，未实现通用的 PCI 直通框架 |
| 等值映射 | Guest IPA == 物理 PA，未实现地址重映射（虽然 PCI BAR 本身可编程） |
| 仅 VM1 直通 | 硬编码只给 VM1 映射，未做灵活的设备分配机制 |
| 无 DMA 隔离 | edu 设备不做 DMA，未实现 SMMU/IOMMU 用于 DMA 隔离 |
| 无热插拔 | 设备在启动前静态添加，不支持运行时插拔 |

### 10.2 未来优化方向

1. **MSI/MSI-X 支持**：实现 GICv3 ITS 虚拟化，支持消息信号中断
2. **SMMU 虚拟化**：Stage-2 + SMMU 实现 DMA 隔离，直通 DMA 设备
3. **通用 PCI 直通框架**：不限于 edu，可直通任意 PCI 设备
4. **设备隔离与分配**：多 VM 场景下灵活分配 PCI 设备
5. **VFIO 兼容接口**：向 Linux VFIO 标准靠拢，支持标准 Guest 驱动

---

## 11. 各阶段对比

| 阶段 | 核心功能 | 关键技术 |
|------|---------|---------|
| Phase 1 | Hypervisor 基础框架 | EL2 初始化、异常向量、MMU Stage-1 |
| Phase 2 | Guest VM 启动 | Stage-2 地址转换、Guest 加载、eret 进入 EL1 |
| Phase 3 | 多 VM 调度 | 轮转调度、世界切换（vGIC + vtimer）、TLB 隔离 |
| Phase 4 | 设备虚拟化 | MMIO 陷获框架、virtio-net、ivshmem 共享内存 |
| Phase 5 | SMP 与多描述符链 | 虚拟 SMP（多核 VM）、virtio 多描述符链 |
| Phase 6 | SGI 核间通信 | GICv3 SGI、IPI 抽象层、核间就绪握手 |
| Phase 7 | PCI 直通 | ECAM 枚举、BAR0 MMIO 直通、INTx HW 映射注入、优先级设计 |
