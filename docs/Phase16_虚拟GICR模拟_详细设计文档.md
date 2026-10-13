# Phase 16：虚拟 GIC Redistributor（GICR）模拟 详细设计文档

## 1. 概述

### 1.1 背景

ARMv8-A 的 GICv3 中断控制器由三个物理模块组成：

| 模块 | 类型 | 访问方式 | 本项目现状 |
|------|------|----------|-----------|
| CPU Interface（`ICC_*_EL1`） | 系统寄存器 | 硬件自动虚拟化（HCR_EL2 控制重定向） | Phase 3 起直通，无需软件介入 |
| Distributor（GICD） | MMIO | 访问会 trap 到 EL2 | Phase 15 已模拟 |
| Redistributor（GICR） | MMIO | 访问会 trap 到 EL2 | 本 Phase 补全 |

GICD 管理 **SPI（共享外设中断，INTID 32..1019）**，而每个 PE 各自挂接一个 **GICR（Redistributor）**，负责该 PE 的 **SGI（0..15）/ PPI（16..31）** 私有中断。GICR 由两个连续的 64KB 帧组成：

- **RD_base 帧**（偏移 0x0000–0xFFFF）：`GICR_CTLR/IIDR/TYPER/WAKER/PROPBASER/PENDBASER` 等全局控制寄存器；
- **SGI_base 帧**（偏移 0x10000–0x1FFFF）：SGI/PPI 的 `IGROUPR0/ISENABLER0/ICENABLER0/IPRIORITYR(n)` 等。

Guest 访问这些 `GICR_*` MMIO 寄存器时必然触发 Stage-2 数据中止并陷入 EL2。Phase 15 之前 Hypervisor 未模拟 GICR，Guest 无法用标准 GICv3 流程初始化私有中断；且注入路径对 PPI **无条件放行**，无法体现 GICR 对 PPI 的使能门控语义。

### 1.2 目标

- ✅ 为每个 VM 维护一份虚拟 GICR 状态（`struct vgic_rdist`），核心是 **PPI 使能位图**
- ✅ Guest 访问 `GICR_*` MMIO 时 trap-and-emulate，支持读/写 `CTLR/IIDR/TYPER/WAKER/IGROUPR0/ISENABLER0/ICENABLER0/IPRIORITYR`
- ✅ 在软件 PPI 注入路径上增加 **enable 门控**：被 Guest 禁用的 PPI 直接丢弃（返回 -1）
- ✅ 门控边界清晰：仅软件 PPI 受门控；vtimer PPI 27 走 HW 直通（`vgic_inject_virq_hw`）**不受**软件位图门控
- ✅ 与 Phase 15 对称：GICD = SPI 门控，GICR = PPI 门控，二者统一收口于 `vgic_inject_virq`
- ✅ 新增 `HVC_GICR_INJECT(43)` 测试 hypercall + Guest 侧 Phase 16 自检，全量回归通过

### 1.3 平台与基础

- **硬件平台**：QEMU `virt` 机器，ARMv8-A，GICv3 + ITS + vGIC，2 物理核
- **Hypervisor**：aVisor（Type-1，EL2）
- **Guest**：EL1 测试程序（裸金属）
- **前序版本**：`v0.15.0-phase15-vgicd`

---

## 2. 整体架构设计

### 2.1 数据流

```
Guest 访问 GICR MMIO (0x080A0000 + offset)
        │  Stage-2 data abort
        ▼
handle_mem_abort (common/mm.c)
        │  is_mmio_addr() == true
        ▼
virt_mmio_read / virt_mmio_write (virt_board.c)
        │  按地址区间分派到 GICR
        ▼
vgicr_read / vgicr_write (vgic.c)
        │  读写 struct vgic_rdist（per-VM 状态）
        ▼
返回读值 / 更新 ctlr 与 ppi_enable
```

```
软件 PPI 注入（HVC_GICR_INJECT / 未来设备 PPI）
        │
        ▼
vgic_inject_virq(vintid)          ← 统一入口（带门控）
        │  vintid ∈ [16, 32) 且 !virt_vgicr_ppi_enabled(vm, vintid) → 丢弃，返回 -1
        │  vintid ∈ [32, 1020) 且 !virt_vgicd_spi_enabled(vm, vintid) → 丢弃，返回 -1
        ▼
vgic_inject_virq_raw(vintid)      ← 无门控内部实现（写 LR1）
```

### 2.2 per-VM GICR 状态

```c
/* include/boards/virt/vgic.h */
struct vgic_rdist {
    uint32_t ctlr;             /* GICR_CTLR */
    uint32_t ppi_enable;       /* GICR_ISENABLER0：INTID 0..31 使能位图 */
};
```

- `ppi_enable` 覆盖 INTID 0..31，`bit = vintid & 31`。其中 bit 0..15 对应 SGI、bit 16..31 对应 PPI。
- `struct vgic_rdist` 定义在公共头 `vgic.h`，嵌入到 `virt_board.c` 私有的 `struct virt_vm_devices` 中，与 Phase 15 的 `struct vgic_dist` 并列，共同构成 per-VM 的 vGIC 设备模型。
- 次级 vCPU 通过 `virt_attach_secondary` 共享主 vCPU 的 `board_data`，因此 **同一 VM 的所有 vCPU 共享同一份 GICR 状态**。真实硬件中 GICR 是 per-PE 的，本教学实现简化为 per-VM 单份（覆盖 CPU0 的 Redistributor，`0x080A0000` 起 128KB）。

### 2.3 默认全使能的兼容策略

真实 GICR 复位时 `GICR_ISENABLER0` 全为 0，Guest 必须显式使能 PPI。但本项目历史上唯一的 PPI 是 vtimer（INTID 27），它走 HW 直通（`vgic_inject_virq_hw`），从未经过软件注入路径。

为保持向后兼容并最小化改动，`vgicr_init()` 将 `ppi_enable` 置 `0xFFFFFFFF`（**默认所有 PPI 使能**）。这样：

- 现有 vtimer 流程**零改动**（且它本就不受此位图影响）；
- 新的 Phase 16 测试可通过写 `GICR_ICENABLER0` 显式禁用某个软件 PPI（如 24）来验证门控语义。

---

## 3. GICR 寄存器模拟

### 3.1 寄存器映射

寄存器地址为相对 GICR 基址（`VIRT_VGICR_BASE = 0x080A0000`）的偏移，GICR 分两帧：

| 寄存器 | 帧 | 偏移 | 读 | 写 |
|--------|-----|------|-----|-----|
| `GICR_CTLR` | RD_base | 0x0000 | 返回 `ctlr` | 赋值 |
| `GICR_IIDR` | RD_base | 0x0004 | 返回 `0x43B` | 忽略 |
| `GICR_TYPER` | RD_base | 0x0008 | 返回 `0`（Affinity_Value=0，简化） | 忽略 |
| `GICR_WAKER` | RD_base | 0x0014 | 返回 `0`（未睡眠） | 忽略（Guest 唤醒） |
| `GICR_IGROUPR0` | SGI_base | 0x10080 | 返回 `0xFFFFFFFF`（SGI+PPI 全 Group 1） | 忽略 |
| `GICR_ISENABLER0` | SGI_base | 0x10100 | 返回 `ppi_enable` | `ppi_enable \|= val` |
| `GICR_ICENABLER0` | SGI_base | 0x10180 | 返回 0 | `ppi_enable &= ~val` |
| `GICR_IPRIORITYR(n)` | SGI_base | 0x10400 + n*4 | 返回 `0xA0A0A0A0`（默认优先级 0xA0） | 忽略 |
| 其余 | — | — | 0 | 忽略 |

> 注：`GICR_SGI_BASE = 0x10000` 是 SGI 帧相对 GICR 基址的偏移。因此 `GICR_ISENABLER0` 的**绝对偏移**是 `0x10000 + 0x0100 = 0x10100`，与 ARM 规范一致（`GICR_SGI_base + 0x0100`）。

### 3.2 实现要点

```c
uint32_t vgicr_read(struct vgic_rdist *r, uint32_t offset)
{
    if (!r) return 0;
    if (offset == GICR_CTLR)  return r->ctlr;    /* 0x0000 */
    if (offset == GICR_IIDR)  return 0x43B;      /* 0x0004 */
    if (offset == GICR_TYPER) return 0;          /* 0x0008 */
    if (offset == GICR_WAKER) return 0;          /* 0x0014 */

    if (offset >= GICR_SGI_BASE) {
        uint32_t sgi = offset - GICR_SGI_BASE;
        if (sgi == GICR_IGROUPR0)       return 0xFFFFFFFFu;   /* 0x0080 */
        if (sgi == GICR_ISENABLER0)     return r->ppi_enable; /* 0x0100 */
        if (sgi >= 0x0400 && sgi < 0x0420 && ((sgi - 0x0400) & 3) == 0)
            return 0xA0A0A0A0u;                              /* IPRIORITYR(n) */
    }
    return 0;
}
```

```c
void vgicr_write(struct vgic_rdist *r, uint32_t offset, uint32_t val)
{
    if (!r) return;
    if (offset == GICR_CTLR) { r->ctlr = val; return; }      /* 0x0000 */
    if (offset == GICR_WAKER) return;                        /* 0x0014 唤醒忽略 */

    if (offset >= GICR_SGI_BASE) {
        uint32_t sgi = offset - GICR_SGI_BASE;
        if (sgi == GICR_ISENABLER0) { r->ppi_enable |= val;  return; }  /* 0x0100 */
        if (sgi == GICR_ICENABLER0) { r->ppi_enable &= ~val; return; }  /* 0x0180 */
    }
    /* 其余写忽略 */
}
```

- **读模拟**返回稳定、自洽的静态/位图值，保证 Guest 的探测循环（按 `TYPER` 遍历使能寄存器、唤醒流程读 `WAKER`）不会读到异常值。
- **写模拟**只关心影响注入门控的 `ISENABLER0/ICENABLER0` 与 `CTLR`，其余写被忽略——这是教学型 Hypervisor 的最小够用实现。

---

## 4. PPI 注入门控

### 4.1 门控边界（关键设计决策）

GICR 的 `ISENABLER0/ICENABLER0` 只作用于 **SGI/PPI（INTID 0..31）**。SPI（32..1019）由 GICD 管理，LPI（8192+）由 ITS 管理。

本 Phase 的关键设计决策是**门控边界**：vtimer PPI 27 走 `vgic_inject_virq_hw`（LR0 HW 直通，split EOI 模式），硬件在 Guest 写虚拟 `EOIR` 时自动 deactivate 链接的物理中断。若对 vtimer 施加软件 PPI 门控，会破坏物理中断的 deactivate 生命周期。因此：

| 中断类型 | 虚拟中断号 | 注入入口 | 是否受 GICR PPI 门控 |
|----------|-----------|----------|---------------------|
| vtimer PPI | 27 | `vgic_inject_virq_hw`（LR0 HW 映射） | ❌（HW 直通） |
| 软件 PPI | 16..31（除 27 语义外） | `vgic_inject_virq` | ✅ |
| 测试 PPI | 24 | `HVC_GICR_INJECT` → `vgic_inject_virq` | ✅ |
| SPI | 32..1019 | `vgic_inject_virq` | ❌（受 GICD 门控，Phase 15） |
| SGI | 0..15 | `vgic_inject_virq_to_vm` | ❌（SGI 语义） |
| LPI | 8192+ | `vgic_inject_virq_to_vm` | ❌（vITS 管理） |

### 4.2 门控实现

Phase 15 已将注入函数拆分为 `vgic_inject_virq`（门控入口）与 `vgic_inject_virq_raw`（无门控内部实现）。Phase 16 在 `vgic_inject_virq` 中**前置增加 PPI 门控**，使该函数成为"设备 SPI/PPI 统一门控入口"：

```c
int vgic_inject_virq(uint32_t vintid)
{
    /* PPI (16-31) 受 GICR enable 门控（软件 PPI） */
    if (vintid >= 16 && vintid < 32 &&
        !virt_vgicr_ppi_enabled(current, vintid))
        return -1;

    /* SPI (32-1019) 受 GICD enable 门控；SGI/LPI 不在此列 */
    if (vintid >= 32 && vintid < 1020 &&
        !virt_vgicd_spi_enabled(current, vintid))
        return -1;

    return vgic_inject_virq_raw(vintid);
}
```

- `vgic_inject_virq_raw` 保持不变，继续供 `vgic_inject_virq_to_vm` 的 LPI/vSGI 路径、`vgic_ipi_update_handle` 运行态注入使用（这些路径不受 PPI/SPI enable 门控）。
- 该实现让 Phase 15 的"设备 SPI 入口"自然泛化为"设备 SPI/PPI 统一入口"，语义清晰。

### 4.3 getter：跨模块耦合的收敛点

`struct vgic_rdist` 定义在 `vgic.h`，但实际存储于 `virt_board.c` 的私有 `struct virt_vm_devices`。为让 `vgic.c` 查询门控状态而不暴露 `virt_vm_devices` 布局，在 `virt_board.c` 提供 getter：

```c
int virt_vgicr_ppi_enabled(struct task_struct *vm, uint32_t vintid)
{
    struct virt_vm_devices *devs = (struct virt_vm_devices *)vm->board_data;
    if (!devs)
        return 1;  /* 无设备模型（如 idle）时默认放行 */
    return (devs->vgicr.ppi_enable >> (vintid & 31)) & 1;
}
```

---

## 5. MMIO 分派接入

### 5.1 地址区间

```c
#define VIRT_VGICR_BASE   0x080A0000UL
#define VIRT_VGICR_SIZE   0x00020000UL   /* 128KB：RD_base + SGI_base 两帧 */
```

与 QEMU `virt` 机器的物理 GICR 基址一致（`base.h` 中 `VIRT_GICR_BASE = 0x080A0000`），Guest 用标准 GIC 地址即可访问虚拟 GICR。它与虚拟 vITS（`0x0C000000`）不冲突。

### 5.2 分派逻辑

在 `virt_board.c` 中，`virt_is_mmio_addr` 增加 GICR 区间判断，`virt_mmio_read/write` 增加 GICR 分派：

```c
static int virt_is_mmio_addr(struct task_struct *tsk, unsigned long ipa)
{
    if (ipa >= VIRT_VGICR_BASE && ipa < VIRT_VGICR_BASE + VIRT_VGICR_SIZE)
        return 1;
    /* ... GICD / virtio-net / ivshmem / vITS ... */
}
```

```c
static unsigned long virt_mmio_read(struct task_struct *tsk, unsigned long ipa)
{
    ...
    if (ipa >= VIRT_VGICR_BASE && ipa < VIRT_VGICR_BASE + VIRT_VGICR_SIZE) {
        uint32_t offset = (uint32_t)(ipa - VIRT_VGICR_BASE);
        return vgicr_read(&devs->vgicr, offset);
    }
    ...
}
```

### 5.3 初始化

`virt_initialize()` 在 `vgicd_init(&devs->vgicd)` 之后调用：

```c
vgicr_init(&devs->vgicr);
```

`vgicr_init` 置 `ctlr = 0`、`ppi_enable = 0xFFFFFFFF`。

---

## 6. 测试 hypercall：HVC_GICR_INJECT(43)

为在 Guest 侧直接验证 PPI 门控语义（无需真实设备触发），新增 hypercall：

```c
/* include/common/hvc.h */
#define HVC_GICR_INJECT 43  /* x0=vPPI(16-31)，直接经 vgic_inject_virq 注入，返回 0/-1 */
```

```c
/* arch/aarch64/sync_exc.c，handle_hvc64 的 PLATFORM_VIRT 区块 */
case HVC_GICR_INJECT:
    regs->regs[0] = vgic_inject_virq((uint32_t)regs->regs[0]);
    break;
```

返回值：

- `0`：PPI 使能，注入成功（写入 LR1）
- `-1`：PPI 被 GICR 禁用而丢弃，或 LR 无空闲

该 hypercall 与 Phase 15 的 `HVC_GICD_INJECT(42)` 底层同为 `vgic_inject_virq`，差别仅在传入的中断号区间（PPI 走 GICR 门控，SPI 走 GICD 门控）。

---

## 7. Guest 侧测试

### 7.1 新增宏与辅助

```c
/* guest.h */
#define HVC_GICR_INJECT  43
#define VGICR_TEST_PPI   24   /* 测试用软件 PPI，避开 vtimer 27 */

/* main.c */
#define GICR_BASE        0x080A0000UL
#define GICR_CTLR       (GICR_BASE + 0x0000)
#define GICR_IIDR       (GICR_BASE + 0x0004)
#define GICR_TYPER      (GICR_BASE + 0x0008)
#define GICR_WAKER      (GICR_BASE + 0x0014)
#define GICR_ISENABLER0 (GICR_BASE + 0x10100)
#define GICR_ICENABLER0 (GICR_BASE + 0x10180)
```

### 7.2 测试流程（`phase16_gicr_test`）

1. `gicr_init()`：读 `GICR_TYPER/WAKER`，若 `WAKER` 指示睡眠则写 0 唤醒（模拟标准 GIC 唤醒流程）
2. 读 `GICR_TYPER/GICR_IIDR`，确认模拟值（`0` / `0x43B`）
3. 读 `ISENABLER0`，确认默认所有 PPI 使能（bit 24 = 1）
4. 写 `ICENABLER0` 禁用 PPI 24 → `HVC_GICR_INJECT(24)` 返回 -1，且收不到中断
5. 写 `ISENABLER0` 重新使能 PPI 24 → `HVC_GICR_INJECT(24)` 返回 0，且 IRQ handler 收到 PPI 24

IRQ handler 新增分支：

```c
} else if (intid == VGICR_TEST_PPI) {
    vgicr_irq_received = 1;
}
```

主核 `guest_main` 在 `gic_dist_init()` 后调用 `gicr_init()`，在 `phase15_vgicd_test()` 后调用 `phase16_gicr_test()`。

### 7.3 预期输出

```
gicr: GICR_TYPER = 0x00000000, GICR_IIDR = 0x0000043b
gicr: ISENABLER0 = 0xffffffff (PPI 24 enabled = 1)
gicr: disabling PPI 24 via ICENABLER0...
gicr: inject while disabled -> ret=-1, irq_received=0
gicr: gating PASSED (disabled PPI dropped)
gicr: re-enabling PPI 24 via ISENABLER0...
gicr: inject while enabled -> ret=0, irq_received=1
gicr: inject PASSED (enabled PPI delivered)
Phase 16: ALL tests done
```

---

## 8. 改动文件清单

| 文件 | 改动 |
|------|------|
| `include/boards/virt/vgic.h` | 新增 `struct vgic_rdist` + `vgicr_init/read/write` 声明 |
| `hypervisor/boards/virt/vgic.c` | 实现 `vgicr_init/read/write`；`vgic_inject_virq` 前置增加 PPI 门控 |
| `include/boards/virt/virt_board.h` | 新增 `virt_vgicr_ppi_enabled` 声明 |
| `hypervisor/boards/virt/virt_board.c` | 新增 GICR 地址区间、MMIO 分派、`vgicr` 字段、`virt_vgicr_ppi_enabled` 实现、`vgicr_init` 调用 |
| `include/common/hvc.h` | 新增 `HVC_GICR_INJECT(43)` |
| `hypervisor/arch/aarch64/sync_exc.c` | 新增 `HVC_GICR_INJECT` 处理 |
| `guests/el1_test/guest.h` | 新增 `HVC_GICR_INJECT` / `VGICR_TEST_PPI` |
| `guests/el1_test/main.c` | 新增 GICR 宏、`gicr_init`、`phase16_gicr_test`、IRQ 分支、主核调用 |

---

## 9. 验证与回归

### 9.1 构建

```bash
make -f Makefile.virt all
```

### 9.2 运行回归

```bash
timeout 40 make -f Makefile.virt run
```

退出码 124（timeout 正常终止）表示 VM 未崩溃。关键断言：

- 两个 VM 均输出 `gicr: gating PASSED (disabled PPI dropped)` 与 `gicr: inject PASSED (enabled PPI delivered)`
- Phase 1~15 既有测试全部通过（virtio loopback / ivshmem ping-pong / edu / vITS / scheduler / mem / shm / SMP / vSGI / vGICD）
- 进入主循环后持续输出 `tick N` 心跳，证明 PPI 门控改动未破坏正常中断流（尤其 vtimer PPI 27 的 HW 直通）

### 9.3 回归结果（实际输出摘录）

```
[vm1] gicr: GICR_TYPER = 0x00000000, GICR_IIDR = 0x0000043b
[vm1] gicr: ISENABLER0 = 0xffffffff (PPI 24 enabled = 1)
[vm1] gicr: disabling PPI 24 via ICENABLER0...
[vm1] gicr: inject while disabled -> ret=18446744073709551615, irq_received=0
[vm1] gicr: gating PASSED (disabled PPI dropped)
[vm1] gicr: re-enabling PPI 24 via ISENABLER0...
[vm1] gicr: inject while enabled -> ret=0, irq_received=1
[vm1] gicr: inject PASSED (enabled PPI delivered)
[vm1] Phase 16: ALL tests done
```

（`ret=18446744073709551615` 为 `-1` 的无符号打印，因 `puts_dec` 只处理无符号数，语义正确。）

---

## 10. 后续规划（方向1 剩余子项）

1. **虚拟 SGI 的 trap-and-emulate**（延续 Phase 14 方向）：捕获 Guest 写 `ICC_SGI1R_EL1`，替代 `HVC_VSGI_SEND` 半虚拟化。
2. **优先级与 pending 状态的完整建模**：实现 `GICR_IPRIORITYR/GICD_IPRIORITYR` 写、`ISPENDR/ICPENDR`、虚拟中断优先级排序，使 vGIC 更接近真实硬件语义。
3. **vGIC 注入路径统一**：把 `ivshmem doorbell` / `shm_notify` 的"直写 saved state"老式注入统一为 `vgic_inject_virq` 或 `vgic_inject_virq_to_vm`，消除分散的门控逻辑。
4. **per-PE GICR 状态**：当前 per-VM 单份 GICR，若要完整支持多 vCPU 私有中断，可将 `struct vgic_rdist` 扩展为 per-vCPU 数组。

---

## 11. 版本信息

- **Phase**：16
- **主题**：虚拟 GIC Redistributor（GICR）模拟 + PPI 使能门控（trap-and-emulate）
- **Tag**：`v0.16.0-phase16-vgicr`
- **前序 Tag**：`v0.15.0-phase15-vgicd`
