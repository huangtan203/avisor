# Phase 18：优先级与 pending 完整建模 详细设计文档

## 1. 概述

### 1.1 背景

Phase 15（vGICD）和 Phase 16（vGICR）完成了虚拟 Distributor 和 Redistributor 的框架建模，但只实现了**使能位图**（ISENABLER/ICENABLER），对以下关键 GICv3 特性仅做占位或忽略：

- **优先级寄存器**（`GICD_IPRIORITYR` / `GICR_IPRIORITYR`）：硬编码返回 `0xA0A0A0A0`，写操作被丢弃
- **pending 状态位图**（`GICD_ISPENDR/ICPENDR` / `GICR_ISPENDR0/ICPENDR0`）：读返回 0，写被忽略
- **ICH_LR Priority 字段**：注入虚拟中断时未设置 LR 的优先级位（bits[55:48]），默认由硬件置 0

这导致 vGIC 语义不完整——Guest 读/写优先级或 pending 寄存器不会得到预期结果，虚拟中断的优先级也无法体现。

### 1.2 目标

- ✅ 为 `struct vgic_dist` 增加 `spi_pending[32]`（软件 pending 位图）和 `spi_priority[1020]`（SPI 优先级）
- ✅ 为 `struct vgic_rdist` 增加 `pending`（SGI/PPI pending 位图）和 `priority[32]`（SGI/PPI 优先级）
- ✅ 实现 `GICD_ISPENDR/ICPENDR/IPRIORITYR` 的 trap-and-emulate 读写
- ✅ 实现 `GICR_ISPENDR0/ICPENDR0/IPRIORITYR` 的 trap-and-emulate 读写
- ✅ 注入虚拟中断时，将对应 vINTID 的优先级写入 `ICH_LR[55:48]` 字段
- ✅ 写 `ICPENDR` 时，同步清除硬件 LR 中对应 vINTID 的 pending 条目
- ✅ Guest 侧 Phase 18 测试 4 项全通过，全量回归不破坏既有功能

### 1.3 平台与基础

- **硬件平台**：QEMU `virt` 机器，ARMv8-A，GICv3 + ITS + vGIC，2 物理核
- **Hypervisor**：aVisor（Type-1，EL2）
- **Guest**：EL1 测试程序（裸金属）
- **前序版本**：`v0.17.0-phase17-vsgi-trap`

---

## 2. 数据结构扩展

### 2.1 vgic_dist 扩展

```c
struct vgic_dist {
    uint32_t ctlr;
    uint32_t spi_enable[32];    /* 已有：SPI 使能位图 */
    /* Phase 18 新增 */
    uint32_t spi_pending[32];   /* SPI 软件 pending 位图（INTID 0..1023） */
    uint8_t  spi_priority[1020];/* SPI 优先级（INTID 32..1019，index = intid - 32） */
};
```

复位默认值：
- `spi_pending[]` 全 0（无 pending）
- `spi_priority[]` 全 `0xA0`（中等默认优先级，数值越小优先级越高）

### 2.2 vgic_rdist 扩展

```c
struct vgic_rdist {
    uint32_t ctlr;
    uint32_t ppi_enable;        /* 已有：SGI/PPI 使能位图 */
    /* Phase 18 新增 */
    uint32_t pending;           /* SGI/PPI 软件 pending 位图（INTID 0..31） */
    uint8_t  priority[32];      /* SGI/PPI 优先级（INTID 0..31） */
};
```

复位默认值：
- `pending = 0`
- `priority[0..31]` 全 `0xA0`

### 2.3 ICH_LR Priority 字段宏

```c
#define ICH_LR_PRIORITY_SHIFT 48
#define ICH_LR_PRIORITY_MASK  (0xFFUL << 48)
```

---

## 3. GICD 寄存器读写扩展

### 3.1 读（vgicd_read）

新增对以下寄存器的读支持：

| 寄存器 | 偏移 | 行为 |
|--------|------|------|
| `GICD_ISPENDR(n)` | 0x0200 + n*4 | 返回 `d->spi_pending[n]` |
| `GICD_ICPENDR(n)` | 0x0280 + n*4 | 读同 ISPENDR（写才是清除） |
| `GICD_IPRIORITYR(n)` | 0x0400 + n*4 | 从 `spi_priority[]` 拼 4 字节返回 |

`IPRIORITYR` 字节序：低字节 = 低 INTID（标准 GICv3 语义）。对 INTID < 32 的位置（SGI/PPI）返回默认 0xA0（GICD 不负责 SGI/PPI 优先级）。

### 3.2 写（vgicd_write）

新增对以下寄存器的写支持：

| 寄存器 | 偏移 | 行为 |
|--------|------|------|
| `GICD_ISPENDR(n)` | 0x0200 + n*4 | 置位 `spi_pending[n] |= val` |
| `GICD_ICPENDR(n)` | 0x0280 + n*4 | 清位 `spi_pending[n] &= ~val`；同时对每个置 1 的位调用 `vgic_clear_pending_virq()` 清除硬件 LR 中的 pending |
| `GICD_IPRIORITYR(n)` | 0x0400 + n*4 | 写 `spi_priority[intid - 32]`（仅对 32..1019 有效） |

**设计要点**：`ICPENDR` 写同时同步软件位图和硬件 LR，保证 Guest 清除 pending 后，即使中断已注入到 LR 中也会被取消。

---

## 4. GICR 寄存器读写扩展

### 4.1 读（vgicr_read）

在 SGI_base 帧（offset >= 0x10000）中新增：

| 寄存器 | SGI_base 偏移 | 行为 |
|--------|---------------|------|
| `GICR_ICENABLER0` | 0x0180 | 返回 `~r->ppi_enable` |
| `GICR_ISPENDR0` | 0x0200 | 返回 `r->pending` |
| `GICR_ICPENDR0` | 0x0280 | 读同 ISPENDR0 |
| `GICR_IPRIORITYR(n)` | 0x0400 + n*4 | 从 `r->priority[]` 拼 4 字节 |

### 4.2 写（vgicr_write）

新增：

| 寄存器 | SGI_base 偏移 | 行为 |
|--------|---------------|------|
| `GICR_ISPENDR0` | 0x0200 | `r->pending |= val` |
| `GICR_ICPENDR0` | 0x0280 | `r->pending &= ~val`；每位置 1 则调 `vgic_clear_pending_virq()` |
| `GICR_IPRIORITYR(n)` | 0x0400 + n*4 | 写 `r->priority[intid]`（intid < 32） |

---

## 5. vgic_clear_pending_virq：硬件 LR pending 清除

```c
static void vgic_clear_pending_virq(uint32_t vintid)
{
    for (int i = 1; i < NR_VGIC_LRS - 1; i++) {
        uint64_t lr = read_lr(i);
        if ((lr & ICH_LR_STATE_MASK) != ICH_LR_PENDING)
            continue;
        if ((lr & ICH_LR_HW) != 0)
            continue;  /* 跳过 HW 映射条目 */
        if ((uint32_t)(lr & 0xffffffffULL) == vintid) {
            write_lr(i, 0);
            asm volatile("isb");
            return;
        }
    }
}
```

设计原则：
- **只扫描软件 LR（LR1..NR-2）**：LR0 保留给 vtimer HW 映射（ICH_LR_HW=1），LR2 给 PCI INTx HW 映射，均不处理
- **只清除 pending 状态**：active 状态的中断不应被软件 ICPENDR 清除（符合 GICv3 语义）
- **找到即返回**：同一 vINTID 理论上不会同时出现在多个 LR 中

---

## 6. 注入带优先级

### 6.1 virt_vgic_vintid_priority

新增 getter 函数（位于 `virt_board.c`），根据 vINTID 类型查询优先级：

```c
uint8_t virt_vgic_vintid_priority(struct task_struct *vm, uint32_t vintid)
{
    if (vintid < 32)      return r->priority[vintid];      /* SGI/PPI → GICR */
    if (vintid < 1020)    return d->spi_priority[vintid - 32]; /* SPI → GICD */
    return 0xA0;  /* LPI 默认 */
}
```

无设备模型（如 idle task）时返回默认 0xA0。

### 6.2 vgic_inject_virq_raw 修改

注入时从 `virt_vgic_vintid_priority` 获取优先级，写入 `ICH_LR[55:48]`：

```c
static int vgic_inject_virq_raw(uint32_t vintid)
{
    uint8_t prio = virt_vgic_vintid_priority(current, vintid);
    ...
    lr = ((uint64_t)vintid & 0xffffffffULL)
         | ICH_LR_GROUP1
         | ICH_LR_PENDING
         | ((uint64_t)prio << ICH_LR_PRIORITY_SHIFT);
    ...
}
```

**影响范围**：所有通过 `vgic_inject_virq_raw` 注入的软件虚拟中断（vSGI、virtio、ivshmem、vITS 翻译的 vSPI 等）都会带上正确的优先级。硬件映射中断（LR0/LR2）由硬件自身管理优先级，不受影响。

---

## 7. Guest 测试

Phase 18 测试为主核单线程，4 项子测试：

| 编号 | 测试项 | 验证内容 |
|------|--------|----------|
| [1] | GICD IPRIORITYR 读写 | 默认 0xA0、写自定义值读回一致 |
| [2] | GICD ISPENDR/ICPENDR | 置位、清除、读值一致（含 ICPENDR 读） |
| [3] | GICR IPRIORITYR 读写 | SGI 和 PPI 优先级读写 |
| [4] | GICR ISPENDR0/ICPENDR0 | SGI/PPI pending 置位与清除 |

预期输出：
```
Phase 18: Priority & pending modeling
  [1] GICD IPRIORITYR read/write ... PASSED
  [2] GICD ISPENDR/ICPENDR ... PASSED
  [3] GICR IPRIORITYR read/write ... PASSED
  [4] GICR ISPENDR0/ICPENDR0 ... PASSED

Phase 18: ALL tests done
```

---

## 8. 改动文件清单

| 文件 | 改动 |
|------|------|
| `include/arch/aarch64/sysregs.h` | 新增 `ICH_LR_PRIORITY_SHIFT/MASK` 宏 |
| `include/boards/virt/vgic.h` | `vgic_dist` 增加 `spi_pending[32]` + `spi_priority[1020]`；`vgic_rdist` 增加 `pending` + `priority[32]` |
| `include/boards/virt/virt_board.h` | 新增 `virt_vgic_vintid_priority` 声明 |
| `hypervisor/boards/virt/virt_board.c` | 实现 `virt_vgic_vintid_priority` |
| `hypervisor/boards/virt/vgic.c` | vgicd/ vgicr 读写扩展优先级和 pending；新增 `vgic_clear_pending_virq`；注入带优先级 |
| `guests/el1_test/main.c` | 新增 GICD/GICR pending/优先级寄存器宏；新增 `phase18_priority_pending_test` + 主核调用 |

---

## 9. 验证与回归

### 9.1 构建

```bash
make -f Makefile.virt all
```
结果：构建通过（仅 pre-existing warnings）。

### 9.2 回归结果

```
[vm1] Phase 18: Priority & pending modeling
[vm1]   [1] GICD IPRIORITYR read/write ... PASSED
[vm1]   [2] GICD ISPENDR/ICPENDR ... PASSED
[vm1]   [3] GICR IPRIORITYR read/write ... PASSED
[vm1]   [4] GICR ISPENDR0/ICPENDR0 ... PASSED
[vm1] Phase 18: ALL tests done
[vm2] Phase 18: Priority & pending modeling
[vm2]   [1] GICD IPRIORITYR read/write ... PASSED
[vm2]   [2] GICD ISPENDR/ICPENDR ... PASSED
[vm2]   [3] GICR IPRIORITYR read/write ... PASSED
[vm2]   [4] GICR ISPENDR0/ICPENDR0 ... PASSED
[vm2] Phase 18: ALL tests done
```

- 两个 VM 均 Phase 18 全部通过
- Phase 1~17 既有测试全部通过（含 vtimer PPI、vSGI HVC/trap 两条路径、virtio-net、ivshmem、edu MSI、vITS、SMP 等）
- 进入主循环后心跳正常，优先级建模未破坏任何既有中断流

---

## 10. 后续规划（方向 1 剩余子项）

1. ~~虚拟 GICR（Redistributor）模拟~~（Phase 16）
2. ~~虚拟 SGI trap-and-emulate~~（Phase 17）
3. ~~优先级与 pending 完整建模~~（Phase 18，本 Phase）
4. **vGIC 注入路径统一**：把 ivshmem doorbell、shm_notify 等"直写 saved state"的老式注入统一为 `vgic_inject_virq_to_vm`，消除分散的门控逻辑，统一走 enable + 优先级 + pending 检查。

---

## 11. 版本信息

- **Phase**：18
- **主题**：优先级与 pending 完整建模
- **Tag**：`v0.18.0-phase18-priority-pending`
- **前序 Tag**：`v0.17.0-phase17-vsgi-trap`
