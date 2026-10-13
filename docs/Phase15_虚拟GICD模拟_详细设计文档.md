# Phase 15：虚拟 GIC Distributor（GICD）模拟 详细设计文档

## 1. 概述

### 1.1 背景

ARMv8-A 的 GICv3 中断控制器由三个物理模块组成：

| 模块 | 类型 | 访问方式 | 本项目现状 |
|------|------|----------|-----------|
| CPU Interface（`ICC_*_EL1`） | 系统寄存器 | 硬件自动虚拟化（HCR_EL2 控制重定向） | Phase 3 起直通，无需软件介入 |
| Distributor（GICD） | MMIO | 访问会 trap 到 EL2 | 此前未模拟 |
| Redistributor（GICR） | MMIO | 访问会 trap 到 EL2 | 未模拟（Guest 未访问） |

GICv3 的虚拟化把 **CPU 接口**交给硬件自动完成（Guest 读 `ICC_IAR1_EL1` 自动拿到虚拟中断号、写 `ICC_EOIR1_EL1` 自动失效 List Register），但 **Distributor** 是纯 MMIO 设备，Guest 访问 `GICD_*` 寄存器时必然触发 Stage-2 数据中止，陷入 EL2。要支撑一个标准的 Guest GIC 初始化流程（写 `GICD_CTLR` 使能、读 `GICD_TYPER` 探测中断线数量、写 `GICD_ISENABLER/ICENABLER` 使能/禁用 SPI），Hypervisor 必须**软件模拟（trap-and-emulate）GICD**。

Phase 14 之前，Hypervisor 的注入路径**无条件**把虚拟 SPI（48/49/50/60）注入 List Register，Guest 无需也无法自行禁用某个 SPI。这意味着 Guest 一旦启动，virtio-net/ivshmem/edu/shm 的中断"永远使能"，无法体现真实 GICD 的 SPI 使能门控语义。

### 1.2 目标

- ✅ 为每个 VM 维护一份虚拟 GICD 状态（`struct vgic_dist`），核心是 **SPI 使能位图**
- ✅ Guest 访问 `GICD_*` MMIO 时 trap-and-emulate，支持读/写 `CTLR/TYPER/IIDR/IGROUPR/ISENABLER/ICENABLER/IPRIORITYR`
- ✅ 在虚拟 SPI 注入路径上增加 **enable 门控**：被 Guest 禁用的 SPI 直接丢弃（返回 -1），模拟真实 GICD 的屏蔽行为
- ✅ 保持向后兼容：默认所有 SPI 使能，现有 virtio/ivshmem/edu/shm 测试无需修改即可通过
- ✅ 门控边界清晰：仅设备 SPI 受门控；LPI（vITS）/ vSGI / vtimer PPI / PCI INTx（HW 映射）不受 GICD SPI 门控
- ✅ 新增 `HVC_GICD_INJECT(42)` 测试 hypercall + Guest 侧 Phase 15 自检，全量回归通过

### 1.3 平台与基础

- **硬件平台**：QEMU `virt` 机器，ARMv8-A，GICv3 + ITS + vGIC，2 物理核
- **Hypervisor**：aVisor（Type-1，EL2）
- **Guest**：EL1 测试程序（裸金属）
- **前序版本**：`v0.14.0-phase14-dual-vm-smp`

---

## 2. 整体架构设计

### 2.1 数据流

```
Guest 访问 GICD MMIO (0x08000000 + offset)
        │  Stage-2 data abort
        ▼
handle_mem_abort (common/mm.c)
        │  is_mmio_addr() == true
        ▼
virt_mmio_read / virt_mmio_write (virt_board.c)
        │  按地址区间分派到 GICD
        ▼
vgicd_read / vgicd_write (vgic.c)
        │  读写 struct vgic_dist（per-VM 状态）
        ▼
返回读值 / 更新 ctlr 与 spi_enable[]
```

```
设备 SPI 注入（virtio / ivshmem / edu MSI / shm notify / HVC_GICD_INJECT）
        │
        ▼
vgic_inject_virq(vintid)          ← 设备 SPI 入口（带门控）
        │  vintid ∈ [32, 1020) 且 !virt_vgicd_spi_enabled(vm, vintid) → 丢弃，返回 -1
        ▼
vgic_inject_virq_raw(vintid)      ← 无门控内部实现（写 LR1）
```

### 2.2 per-VM GICD 状态

```c
/* include/boards/virt/vgic.h */
struct vgic_dist {
    uint32_t ctlr;             /* GICD_CTLR：全局使能 + 亲和路由 */
    uint32_t spi_enable[32];   /* GICD_ISENABLERn：INTID 0..1023 使能位图 */
};
```

- `spi_enable[]` 共 32 个字，覆盖 INTID 0..1023（`bit = vintid & 31`，`word = vintid >> 5`）。
- `struct vgic_dist` 定义在公共头 `vgic.h`，嵌入到 `virt_board.c` 私有的 `struct virt_vm_devices` 中，作为每个 VM 的设备模型之一。
- 次级 vCPU 通过 `virt_attach_secondary` 共享主 vCPU 的 `board_data`，因此 **同一 VM 的所有 vCPU 共享同一份 GICD 状态**（符合真实硬件语义：GICD 是 VM 级、非 per-vCPU）。

### 2.3 默认全使能的兼容策略

真实 GICD 复位时 `GICD_ISENABLER` 全为 0，Guest 必须显式使能 SPI。但本项目的历史设备 SPI（virtio 48 / ivshmem 49 / edu 50 / shm 60）一直由 Hypervisor 预启用，Guest 从未配置过 GICD。

为保持向后兼容，`vgicd_init()` 将 `spi_enable[]` 全部置 1（`0xFFFFFFFF`），即**默认所有 SPI 使能**。这样：

- 现有 Guest 测试**零改动**即可继续工作；
- 新的 Phase 15 测试可以通过写 `GICD_ICENABLER` 显式禁用某个 SPI 来验证门控语义。

---

## 3. GICD 寄存器模拟

### 3.1 寄存器映射

寄存器地址为相对 GICD 基址（`VIRT_VGICD_BASE = 0x08000000`）的偏移：

| 寄存器 | 偏移 | 读 | 写 |
|--------|------|-----|-----|
| `GICD_CTLR` | 0x0000 | 返回 `ctlr` | 仅保留 `EnableGrp0/1NS/1S + ARE_NS/ARE_S` 位 |
| `GICD_TYPER` | 0x0004 | 返回 `0x1F`（ITLinesNumber=31，即 32 条中断线组 → 1020 个 SPI） | 忽略 |
| `GICD_IIDR` | 0x0008 | 返回 `0x43B` | 忽略 |
| `GICD_IGROUPRn` | 0x0080 + n*4 | 返回 `0xFFFFFFFF`（全 Group 1） | 忽略 |
| `GICD_ISENABLERn` | 0x0100 + n*4 | 返回 `spi_enable[n]` | `spi_enable[n] \|= val` |
| `GICD_ICENABLERn` | 0x0180 + n*4 | 返回 0 | `spi_enable[n] &= ~val` |
| `GICD_IPRIORITYRn` | 0x0400 + n*4 | 返回 `0xA0A0A0A0`（默认优先级 0xA0） | 忽略 |
| 其余 | — | 0 | 忽略 |

### 3.2 实现要点

```c
uint32_t vgicd_read(struct vgic_dist *d, uint32_t offset)
{
    if (offset == 0x0000) return d->ctlr;                 /* GICD_CTLR */
    if (offset == 0x0004) return 0x1F;                    /* GICD_TYPER */
    if (offset == 0x0008) return 0x43B;                   /* GICD_IIDR */

    if (offset >= 0x0080 && offset < 0x0100 && ((offset - 0x0080) & 3) == 0)
        return 0xFFFFFFFFu;                               /* IGROUPRn */

    if (offset >= 0x0100 && offset < 0x0180 && ((offset - 0x0100) & 3) == 0)
        return d->spi_enable[(offset - 0x0100) >> 2];     /* ISENABLERn */

    if (offset >= 0x0400 && offset < 0x0800 && ((offset - 0x0400) & 3) == 0)
        return 0xA0A0A0A0u;                               /* IPRIORITYRn */

    return 0;
}
```

- **读模拟**返回稳定、自洽的静态/位图值，保证 Guest 的探测循环（按 `TYPER.ITLinesNumber` 遍历 `ISENABLER`）不会越界读到异常值。
- **写模拟**只关心影响注入门控的 `ISENABLER/ICENABLER` 与全局使能 `CTLR`，其余写被忽略——这是教学型 Hypervisor 的最小够用实现。

---

## 4. SPI 注入门控

### 4.1 门控边界（关键设计决策）

GICD 的 `ISENABLER/ICENABLER` 只作用于 **SPI（INTID 32..1019）**。SGI（0..15）、PPI（16..31）、LPI（8192+）各有独立的使能机制，**不受 GICD SPI 门控**。

本项目注入路径按中断类型划分：

| 中断类型 | 虚拟中断号 | 注入入口 | 是否受 GICD 门控 |
|----------|-----------|----------|------------------|
| vtimer PPI | 27 | `vgic_inject_virq_hw`（LR0 HW 映射） | ❌ |
| virtio-net SPI | 48 | `vgic_inject_virq` | ✅ |
| ivshmem SPI | 49 | `vgic_inject_virq` / ivshmem 直写 saved state | ✅ |
| edu INTx SPI | 50 | `vgic_inject_virq_hw_eoi`（LR2 HW 映射） | ❌（HW 映射，物理 INTx 转发） |
| edu MSI → LPI/vSPI | 64+(vLPI-8192) | `vgic_inject_virq_to_vm` | ❌（LPI 语义，由 vITS 管理） |
| vSGI | 0..15 | `vgic_inject_virq_to_vm` | ❌（SGI 语义） |
| shm notify SPI | 60 | `shm_notify` 直写 saved state | ✅ |
| 测试 SPI | 52 | `HVC_GICD_INJECT` → `vgic_inject_virq` | ✅ |

### 4.2 raw / 门控 函数拆分

为避免 LPI/vSGI 路径被误门控，将原注入函数拆成两层：

```c
/* 无门控内部实现：找空闲 LR（LR1 起）写 pending 虚拟中断 */
static int vgic_inject_virq_raw(uint32_t vintid);

/* 设备 SPI 入口：先做 GICD enable 门控，再走 raw */
int vgic_inject_virq(uint32_t vintid)
{
    if (vintid >= 32 && vintid < 1020 &&
        !virt_vgicd_spi_enabled(current, vintid))
        return -1;
    return vgic_inject_virq_raw(vintid);
}
```

- `vgic_inject_virq_raw`：无门控，供 `vgic_inject_virq`（门控后）、`vgic_inject_virq_to_vm` 的"情况1"、`vgic_ipi_update_handle` 的运行态注入使用。
- `vgic_inject_virq`：仅作为**设备 SPI** 的对外入口（virtio/ivshmem 自注入、edu MSI 回退、HVC 测试）。

### 4.3 跨 VM / 非当前 VM 的门控

两条"直写 saved state"的路径（不经过 `vgic_inject_virq`）也必须门控：

1. **ivshmem doorbell**（`ivshmem.c`）：目标非当前 VM 时，注入前检查：
   ```c
   if (!virt_vgicd_spi_enabled(target->vm, VIRQ_IVSHMEM))
       return;
   ```
2. **shm notify**（`shmem.c`）：遍历 attach 列表时，对每个目标 VM 检查：
   ```c
   if (!virt_vgicd_spi_enabled(vm, SHM_NOTIFY_VIRQ))
       continue;
   ```

这两处是跨 VM 注入，`current` 不是目标 VM，无法复用 `vgic_inject_virq`（它基于 `current` 门控），因此直接调用 `virt_vgicd_spi_enabled(target_vm, vintid)` 显式门控。

### 4.4 getter：跨模块耦合的收敛点

`struct vgic_dist` 定义在 `vgic.h`，但实际存储于 `virt_board.c` 的私有 `struct virt_vm_devices`。为了让 `vgic.c`/`ivshmem.c`/`shmem.c` 都能查询门控状态而不暴露 `virt_vm_devices` 布局，在 `virt_board.c` 提供唯一 getter：

```c
int virt_vgicd_spi_enabled(struct task_struct *vm, uint32_t vintid)
{
    struct virt_vm_devices *devs = (struct virt_vm_devices *)vm->board_data;
    if (!devs)
        return 1;  /* 无设备模型（如 idle）时默认放行 */
    return (devs->vgicd.spi_enable[vintid >> 5] >> (vintid & 31)) & 1;
}
```

---

## 5. MMIO 分派接入

### 5.1 地址区间

```c
#define VIRT_VGICD_BASE   0x08000000UL
#define VIRT_VGICD_SIZE   0x00010000UL   /* 64KB */
```

与 QEMU `virt` 机器的物理 GICD 基址一致（`base.h` 中 `VIRT_GICD_BASE = 0x08000000`），因此 Guest 用标准 GIC 地址即可访问虚拟 GICD。

### 5.2 分派逻辑

在 `virt_board.c` 中，`virt_is_mmio_addr` 增加 GICD 区间判断，`virt_mmio_read/write` 增加 GICD 分派（置于最前，与其它 MMIO 设备并列）：

```c
static int virt_is_mmio_addr(struct task_struct *tsk, unsigned long ipa)
{
    if (ipa >= VIRT_VGICD_BASE && ipa < VIRT_VGICD_BASE + VIRT_VGICD_SIZE)
        return 1;
    /* ... virtio-net / ivshmem / vITS ... */
}
```

```c
static unsigned long virt_mmio_read(struct task_struct *tsk, unsigned long ipa)
{
    ...
    if (ipa >= VIRT_VGICD_BASE && ipa < VIRT_VGICD_BASE + VIRT_VGICD_SIZE)
        return vgicd_read(&devs->vgicd, (uint32_t)(ipa - VIRT_VGICD_BASE));
    ...
}
```

### 5.3 初始化

`virt_initialize()` 在分配并清零 `devs` 后调用：

```c
vgicd_init(&devs->vgicd);
```

由于 `memzero(devs, sizeof(*devs))` 已经把 `vgicd` 清零，`vgicd_init` 随后置 `ctlr = EnableGrp1NS | ARE_NS`、`spi_enable[] = 0xFFFFFFFF`。

---

## 6. 测试 hypercall：HVC_GICD_INJECT(42)

为在 Guest 侧直接验证门控语义（无需真实设备触发），新增 hypercall：

```c
/* include/common/hvc.h */
#define HVC_GICD_INJECT 42  /* x0=vintid，直接经 vgic_inject_virq 注入，返回 0/-1 */
```

```c
/* arch/aarch64/sync_exc.c，handle_hvc64 的 PLATFORM_VIRT 区块 */
case HVC_GICD_INJECT:
    regs->regs[0] = vgic_inject_virq((uint32_t)regs->regs[0]);
    break;
```

返回值：
- `0`：SPI 使能，注入成功（写入 LR1）
- `-1`：SPI 被 GICD 禁用而丢弃，或 LR 无空闲

该 hypercall 让 Guest 能"注入一个指定虚拟 SPI"，配合 `ISENABLER/ICENABLER` 的 MMIO 读写，即可闭环验证门控是否正确。

---

## 7. Guest 侧测试

### 7.1 新增宏与辅助

```c
/* guest.h */
#define HVC_GICD_INJECT  42
#define VGICD_TEST_VIRQ  52   /* 测试用虚拟 SPI，避开 48/49/50/60 */

/* main.c */
#define GICD_BASE        0x08000000UL
#define GICD_CTLR        (GICD_BASE + 0x0000)
#define GICD_TYPER       (GICD_BASE + 0x0004)
#define GICD_IIDR        (GICD_BASE + 0x0008)
#define GICD_ISENABLER(n)(GICD_BASE + 0x0100 + (n) * 4)
#define GICD_ICENABLER(n)(GICD_BASE + 0x0180 + (n) * 4)
```

### 7.2 测试流程（`phase15_vgicd_test`）

1. 读 `GICD_TYPER`/`GICD_IIDR`，确认模拟值（`0x1F` / `0x43B`）
2. 读 `ISENABLER[1]`，确认默认所有 SPI 使能（bit 20 = 1，即 SPI 52 已使能）
3. 写 `ICENABLER[1]` 禁用 SPI 52 → `HVC_GICD_INJECT(52)` 返回 -1，且收不到中断
4. 写 `ISENABLER[1]` 重新使能 SPI 52 → `HVC_GICD_INJECT(52)` 返回 0，且 IRQ handler 收到 SPI 52

IRQ handler 新增分支：

```c
} else if (intid == VGICD_TEST_VIRQ) {
    vgicd_irq_received = 1;
}
```

主核 `guest_main` 在 GIC 初始化后调用 `gic_dist_init()`（读 `GICD_CTLR`，必要时写使能位），在测试序列末尾调用 `phase15_vgicd_test()`。

### 7.3 预期输出

```
gicd: GICD_TYPER = 0x0000001f, GICD_IIDR = 0x0000043b
gicd: ISENABLER[1] = 0xffffffff (SPI 52 enabled = 1)
gicd: disabling SPI 52 via ICENABLER...
gicd: inject while disabled -> ret=-1, irq_received=0
gicd: gating PASSED (disabled SPI dropped)
gicd: re-enabling SPI 52 via ISENABLER...
gicd: inject while enabled -> ret=0, irq_received=1
gicd: inject PASSED (enabled SPI delivered)
Phase 15: ALL tests done
```

---

## 8. 改动文件清单

| 文件 | 改动 |
|------|------|
| `include/boards/virt/vgic.h` | 新增 `struct vgic_dist` + `vgicd_init/read/write` 声明 |
| `hypervisor/boards/virt/vgic.c` | 实现 GICD 模拟；拆分 `vgic_inject_virq_raw`；`vgic_inject_virq` 加门控 |
| `include/boards/virt/virt_board.h` | 新增 `virt_vgicd_spi_enabled` 声明 |
| `hypervisor/boards/virt/virt_board.c` | 新增 GICD 地址区间、MMIO 分派、`vgicd` 字段、`virt_vgicd_spi_enabled` 实现 |
| `hypervisor/boards/virt/ivshmem.c` | 跨 VM doorbell 注入前加 GICD 门控 |
| `hypervisor/boards/virt/shmem.c` | `shm_notify` 遍历注入前加 GICD 门控 |
| `include/common/hvc.h` | 新增 `HVC_GICD_INJECT(42)` |
| `hypervisor/arch/aarch64/sync_exc.c` | 新增 `HVC_GICD_INJECT` 处理；include `vgic.h` |
| `guests/el1_test/guest.h` | 新增 `HVC_GICD_INJECT` / `VGICD_TEST_VIRQ` |
| `guests/el1_test/main.c` | 新增 GICD 宏、`gic_dist_init`、`phase15_vgicd_test`、IRQ 分支、主核调用 |

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

- 两个 VM 均输出 `gicd: gating PASSED (disabled SPI dropped)` 与 `gicd: inject PASSED (enabled SPI delivered)`
- Phase 1~12 既有测试全部通过（virtio loopback / ivshmem ping-pong / edu / vITS / scheduler / mem / shm / SMP / vSGI）
- 进入主循环后持续输出 `tick N` 心跳，证明门控改动未破坏正常中断流

### 9.3 回归结果（实际输出摘录）

```
[vm1] gicd: GICD_TYPER = 0x0000001f, GICD_IIDR = 0x0000043b
[vm1] gicd: ISENABLER[1] = 0xffffffff (SPI 52 enabled = 1)
[vm1] gicd: disabling SPI 52 via ICENABLER...
[vm1] gicd: inject while disabled -> ret=18446744073709551615, irq_received=0
[vm1] gicd: gating PASSED (disabled SPI dropped)
[vm1] gicd: re-enabling SPI 52 via ISENABLER...
[vm1] gicd: inject while enabled -> ret=0, irq_received=1
[vm1] gicd: inject PASSED (enabled SPI delivered)
[vm1] Phase 15: ALL tests done
```

（`ret=18446744073709551615` 为 `-1` 的无符号打印，因 `puts_dec` 只处理无符号数，语义正确。）

---

## 10. 后续规划（Phase 16 展望）

1. **虚拟 GICR（Redistributor）模拟**：补全 `GICR_*` 寄存器，让 Guest 能用标准 GICv3 流程完整初始化（当前 Guest 只访问 GICD + CPU 接口）。
2. **虚拟 SGI 的 trap-and-emulate**（延续 Phase 14 方向）：捕获 `ICC_SGI1R_EL1` 写入，替代 `HVC_VSGI_SEND` 半虚拟化。
3. **vSGI pending 表**：当前 vSGI 用共享内存 bitmap 记录，可迁移到 GICD pending 状态统一管理。
4. **优先级与 pending 状态的完整建模**：实现 `GICD_IPRIORITYR` 写、`ISPENDR/ICPENDR`、虚拟中断优先级排序，使 vGIC 更接近真实硬件语义。
5. **ivshmem doorbell 跨核安全**（延续 Phase 14 方向）：把 `ivshmem_mmio_write` 中"写 saved state"的老式注入统一为 `vgic_inject_virq_to_vm`。

---

## 11. 版本信息

- **Phase**：15
- **主题**：虚拟 GIC Distributor（GICD）模拟（trap-and-emulate）
- **Tag**：`v0.15.0-phase15-vgicd`
- **前序 Tag**：`v0.14.0-phase14-dual-vm-smp`
