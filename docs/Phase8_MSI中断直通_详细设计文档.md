# aVisor Hypervisor Phase 8 详细设计文档 (MSI/MSI-X 中断直通)

> 版本：v0.8.0 (Phase 8)
> 平台：QEMU virt (ARMv8-A, GICv3 + ITS, 虚拟化扩展)
> 目标：启用 GICv3 ITS，把 edu 设备的中断从 INTx 线中断升级为 MSI 消息中断，走 ITS → LPI 翻译路径直通到 Guest
> 前置：Phase 7 PCI 直通（ECAM + BAR0 MMIO + INTx）

---

## 1. 概述

### 1.1 目标达成情况

| 目标 | 状态 | 实现方式 |
|------|------|---------|
| GICv3 ITS 初始化 | ✅ | 命令队列 + 设备表 + 集合表分配与编程，`GITS_CTLR` 使能 |
| Redistributor LPI 使能 | ✅ | PROPBASER/PENDBASER 安装 + `GICR_CTLR.EnableLPIs` |
| ITS 映射链建立 | ✅ | MAPC（collection→CPU）+ MAPD（device→ITT）+ MAPTI（event→LPI）命令 |
| PCI MSI Capability 编程 | ✅ | 动态扫描 cap 列表，写 Message Address/Data，使能 MSI，禁用 INTx |
| LPI 中断处理 | ✅ | `handle_irq` 中 INTID ≥ 8192 分支：软件注入 vIRQ + EOIR |
| Guest 端验证 | ✅ | edu 中断测试改走 MSI 路径（Guest 代码零修改，完全无感知） |

### 1.2 验证结果

```
INFO[0]: ITS: TYPER=0x1f0001efb1
INFO[0]: ITS: LPI tables PROP=0x41001000 PEND=0x41000000 (IDbits=13)
INFO[0]: ITS: cmdq=0x41002000 dt=0x41003000 ct=0x41004000 (enabled)
INFO[0]: PCI: edu MSI cap at 0x40 (64-bit, max 1 vectors)
INFO[0]: ITS: MSI map devid=16 vec=1 lpi=8192..8192 -> cpu0
INFO[0]: PCI: edu MSI enabled (LPI base=8192, msg_addr=0x8090040 msg_data=0x0)
...
[vm1] edu: identification=0x010000ed
[vm1] edu: liveness PASSED (0x21524110)
[vm1] edu: factorial PASSED
[vm1] edu: starting interrupt test (MSI)...
[vm1] edu: MSI interrupt PASSED (irq_status=0x12345678)
```

- 3 次连续运行全部通过，无 WARN/PANIC
- 回归验证：virtio-net（VM1/VM2）、vtimer tick、SMP 调度均正常
- **Guest 代码零修改**（仅测试消息措辞更新）：MSI 配置完全由 Hypervisor 在 `pci_init` 完成，Guest 驱动只看到虚拟 SPI 50，与 Phase 7 INTx 模式完全一致——这正是"中断直通模式对 Guest 透明"的体现

### 1.3 修改的关键文件

```
avisor/
├── include/
│   ├── boards/virt/
│   │   ├── base.h          # ★ VIRT_GITS_BASE/SIZE（ITS @ 0x08080000）
│   │   ├── its.h           # ★ 新增：its_init / its_setup_pci_msi / MSI 消息接口
│   │   ├── gicv3.h         # ★ ICC_RPR/HPPIR1/AP1R0 调试寄存器（Phase 7 遗留）
│   │   └── pci.h           # ★ pci_edu_lpi_base / pci_edu_msi_enabled 导出
├── hypervisor/
│   ├── boards/virt/
│   │   ├── its.c           # ★ 新增：完整 ITS 驱动（表分配/命令队列/MAPC/MAPD/MAPTI）
│   │   ├── pci.c           # ★ MSI Capability 扫描与编程，MSI 成功后关 INTx
│   │   ├── irq.c           # ★ LPI 分支（INTID ≥ 8192）：软件注入 + EOIR
│   │   └── main_virt.c     # ★ [4.25] its_init() 调用（在 gic 之后、pci 之前）
│   └── arch/aarch64/
│       └── utils.S         # ★ 新增 get64/put64（64 位 MMIO 访问原语）
├── include/common/utils.h  # ★ get64/put64 声明
└── guests/el1_test/main.c  # ★ 测试消息更新为 MSI（逻辑不变）
```

---

## 2. 中断架构：INTx vs MSI

### 2.1 两种模式的信号路径对比

```
INTx 模式（Phase 7）：
  edu 拉高 INTA 线
    → PCI swizzle（slot 2 的 INTA → 主桥 INTC）
    → GIC SPI 5（全局 37），电平触发
    → EL2 应答（IAR1）→ split EOI（物理保持 active）
    → LR2 HW 映射注入 vIRQ 50
    → Guest ISR 清设备线 → Guest EOI → HW 联动去活物理 SPI

MSI 模式（Phase 8）：
  edu 向 doorbell 地址写 EventID（内存写，边沿语义）
    → PCIe 事务携带 requester_id = DeviceID（16）
    → ITS 查表翻译：DTE → ITT → CTE
    → LPI 8192 在目标 Redistributor 置 pending（优先级 0x60）
    → EL2 应答（IAR1，顺带清 pending 位）
    → LR1 软件注入 vIRQ 50（LPI 不能用 HW 映射）
    → EOIR（LPI 无 active 状态，无需 DIR）
    → Guest ISR 处理 → Guest EOI 结束软件注入的虚拟中断
```

### 2.2 关键差异

| 维度 | INTx | MSI (ITS/LPI) |
|------|------|---------------|
| 信号 | 专用物理线（电平） | 内存写（边沿） |
| 中断号 | 固定连接（swizzle 决定） | 软件映射（MAPTI 自由分配 LPI） |
| INTID 空间 | 32–1019（SPI） | ≥ 8192（LPI） |
| active 状态 | 有（需要 split EOI + HW 联动/DIR） | 无（IAR 读即清 pending，EOIR 即完成） |
| 多向量 | 4 线共享，需要共享处理 | 每 EventID 独立向量 |
| vGIC 注入 | LR HW 映射（物理去活联动） | 纯软件注入（LR1） |
| 优先级来源 | GICD_IPRIORITYR | LPI 属性表（prop table 字节） |
| 配置复杂度 | 低（一个 SPI） | 高（ITS 三张表 + 命令队列 + LPI 表） |

### 2.3 为什么 MSI 是真实设备的主流

真实 PCIe 设备几乎都支持 MSI/MSI-X：无共享线、向量数多（MSI-X 可达 2048）、无电平保持问题。实现 ITS/LPI 是从"教育设备"迈向"真实设备直通"的必经之路。

---

## 3. GICv3 ITS 架构

### 3.1 三张翻译表 + 命令队列

```
                    MSI 写 (DeviceID, EventID)
                            │
                            ▼
              ┌──────────────────────────┐
              │  Device Table (DT/BASER0) │  DeviceID → DTE
              │  DTE: V | size | ITT 地址 │
              └────────────┬─────────────┘
                           ▼
              ┌──────────────────────────┐
              │  ITT（每设备一张）        │  EventID → ITE
              │  ITE: (pINTID, ICID)     │
              └────────────┬─────────────┘
                           ▼
              ┌──────────────────────────┐
              │  Collection Table (CT/   │  ICID → CTE
              │  BASER1): CTE: procnum   │
              └────────────┬─────────────┘
                           ▼
              目标 Redistributor 置 LPI pending 位
```

- **DTE（Device Table Entry，8 字节）**：bit0=Valid，[5:1]=ITT 大小字段，[49:6]=ITT 物理地址>>8
- **ITE（ITT Entry，12 字节 = 8+4）**：itel 的 bit0=Valid、bit1=IntType（1=物理）、[25:2]=LPI INTID；iteh 的 [15:0]=ICID
- **CTE（Collection Entry，8 字节）**：bit0=Valid，[16:1]=procnum（目标 CPU）

**表的写入方是 QEMU**（处理 MAPD/MAPC/MAPTI 命令时自动写），Hypervisor 只需发命令，不直接操作表项。

### 3.2 命令队列

- 4KB 一页，128 条命令（每条 32 字节 = 4×u64）
- Hypervisor 写命令到队列 → 写 `GITS_CWRITER`（字节偏移）→ QEMU **同步**处理所有命令并推进 `GITS_CREADR`
- 轮询 `CREADR == CWRITER` 即等待完成（SYNC 命令实际是空操作）

### 3.3 QEMU 6.2 仿真行为验证清单（本阶段核心工作）

ITS 的表项格式在 GICv3 规范中是 IMPDEF（除一级间接表外），**必须以 QEMU 源码为准**。以下每一条都从 `hw/intc/arm_gicv3_its.c`、`arm_gicv3_redist.c`、`arm_gicv3_cpuif.c`、`gicv3_internal.h`、`hw/pci/msi.c` 逐条验证：

| 项目 | QEMU 6.2 行为 | 源码依据 |
|------|--------------|---------|
| LPI INTID 下限 | 必须 ≥ 8192，MAPTI 拒绝更小值 | `process_mapti`: `pIntid < GICV3_LPI_INTID_START` |
| MAPI vs MAPTI | MAPI 的 INTID = EventID，不能自由指定 → **必须用 MAPTI** | `process_mapti(ignore_pInt)` |
| ITT 条目大小 | 12 字节（8+4） | `update_ite`: `eventid * (sizeof(uint64_t)+sizeof(uint32_t))` |
| MAPD 布局 | dw0: cmd+devid<<32；dw1: size[4:0]；dw2: V<<63 + ITT_PA[51:8]（256B 对齐） | `process_mapd` |
| MAPC 布局 | dw2: V<<63 + procnum[47:16] + icid[15:0] | `process_mapc` |
| MAPTI 布局 | dw1: pIntid[63:32]+eventid[31:0]；dw2: icid[15:0] | `process_mapti` |
| BASER TYPE/ENTRYSIZE | **只读位**：复位值 BASER0=Device(1)、BASER1=Collection(4)、entry=8B | `gicv3_its_reset` + `GITS_BASER_RO_MASK` |
| BASER PAGESIZE | **复位为 64KB**，必须显式写 0（4KB），否则地址按 64KB 布局解码 | `extract_table_params` + `baser_base_addr` |
| BASER PHYADDR | 4K 模式下字段 [47:12] | `FIELD(GITS_BASER, PHYADDR, 12, 36)` |
| CBASER | V[63] + PA[51:12] + SIZE[7:0]（页数-1）；ITS 使能后只读 | `its_writell` |
| CWRITER | OFFSET 字段在 [19:5]，**直接写 32 对齐的字节偏移**；写入即同步处理 | `its_writell` → `process_cmdq` |
| 命令执行模型 | 完全同步；SYNC/INV 为空操作 | `process_cmdq` |
| EnableLPIs | GICR_CTLR bit0；置位瞬间扫描 pend 表重算 hpplpi | `gicr_write` |
| PROPBASER | IDbits[4:0] + PA[51:12]（4KB 对齐） | `FIELD(GICR_PROPBASER, PHYADDR, 12, 40)` |
| PENDBASER | PA 字段 [51:16]——**pend 表必须 64KB 对齐** | `FIELD(GICR_PENDBASER, PHYADDR, 16, 36)` |
| prop 表索引 | 按 **LPI − 8192** 索引 | `gicv3_redist_check_lpi_priority` |
| pend 表索引 | 按**完整 LPI 号**索引（bit 偏移 = LPI/8） | `gicv3_redist_lpi_pending` |
| LPI 优先级 | GICD_CTLR.DS=1（virt 默认）：`byte & 0xFC`；DS=0：右移一位再置 0x80 | `gicv3_redist_check_lpi_priority` |
| LPI 使能位 | prop 字节 bit0（bit1=Group1） | `LPI_CTE_ENABLED = TABLE_ENTRY_VALID_MASK` |
| IAR1 读取 | **自动清除 LPI pending 位**（激活即去挂起） | `icc_activate_irq` → `gicv3_redist_lpi_pending(irq, 0)` |
| LPI EOI | EOIR 即完成（无 active 状态，无需 DIR） | `icc_eoir_write` |
| DeviceID 来源 | MSI 写的 `attrs.requester_id = (bus<<8)|(dev<<3)|fn` | `msi_send_message` + `pci_requester_id` |
| MSI doorbell | GITS_TRANSLATER @ ITS+0x10040，32 位写入 EventID | `gicv3_its_translation_write` |
| TYPER 解码 | 0x1f0001efb1：ITT 条目 12B、IDbits=15、DevBits=15、CIL=1（collection ID 最多 4 个） | realize 时 FIELD_DP64 |

---

## 4. ITS 驱动实现

### 4.1 初始化流程（its_init）

```
1. 分配 LPI 表（顺序关键！）：
   - pend 表最先分配：必须 64KB 对齐（PENDBASER.PA=[51:16]）。
     页池首页 LOW_MEMORY=0x41000000 恰好 64KB 对齐，
     its_init 是页池首个使用者（main 中 [4.25] 最先触发分配）
   - prop 表：4KB 对齐即可（PROPBASER.PA=[51:12]）

2. 安装到 CPU0 Redistributor：
   put64(GICR + 0x78, pend_pa)              /* PENDBASER */
   put64(GICR + 0x70, prop_pa | 13)         /* PROPBASER, IDbits=13 */
   put32(GICR + 0x00, 1)                    /* EnableLPIs */

   IDbits=13 → 覆盖 LPI 0..16383：
   - prop 表 4KB 索引 LPI 8192..12287（够 edu 用）
   - pend 扫描范围 2048 字节（bit 8192..16383）

3. 分配命令队列 + 设备表 + 集合表（各 1 页，分配器已清零）

4. 编程表基址（必须在使能前——使能后 BASER/CBASER 变只读）：
   put64(GITS_BASER(0), V | dt_pa)     /* TYPE/ENTRYSIZE 只读位写 0 保留复位值 */
   put64(GITS_BASER(1), V | ct_pa)
   put64(GITS_CBASER,  V | cmdq_pa)

5. 使能：put32(GITS_CTLR, 1)
   （QEMU 在此瞬间解析 BASER/CBASER）
```

### 4.2 MSI 映射建立（its_setup_pci_msi）

```c
devid = (bus << 8) | (dev << 3) | func;      /* QEMU requester_id 编码 */

/* MAPC: collection 0 → procnum target_cpu */
its_send_cmd(MAPC, 0, V<<63 | procnum<<16 | 0);

/* ITT 分配一页（MAPD 地址字段按 256B 对齐解码，4K 页天然满足） */
itt_pa = allocate_page();

/* MAPD: devid → ITT，size=3（16 个 event） */
its_send_cmd(MAPD | devid<<32, 3, V<<63 | itt_pa);

/* MAPTI: eventid i → LPI(8192+i)，collection 0
 * 同时配置 LPI 属性表：prop[lpi-8192] = 0x60 | 0x1 */
for (i = 0; i < num_vectors; i++) {
    prop[lpi - 8192] = LPI_PROP_PRIO(0x60) | ENABLE;
    its_send_cmd(MAPTI | devid<<32, lpi<<32 | i, 0);
}

its_send_cmd(SYNC, 0, 0, 0);   /* 收尾 */
```

**优先级 0x60 的选择**沿用 Phase 7 的结论：物理直通中断必须高于虚拟中断默认优先级 0x80，否则 LR 中 pending 的虚拟中断会把物理 RPR 压在 0x80，物理中断无法抢占 EL1。

### 4.3 命令发送（its_send_cmd）

```c
slot = cmdq + cq_byte_off;          /* 4×u64 */
slot[0..3] = d0..d3;
dsb sy;
cq_byte_off = (cq_byte_off + 32) % 4096;
put64(GITS_CWRITER, cq_byte_off);  /* 字节偏移，[19:5] 字段自动编码 */
while (get64(GITS_CREADR) != cq_byte_off && --timeout)
    ;                               /* QEMU 同步处理，很快返回 */
```

---

## 5. PCI MSI Capability 编程（pci.c）

### 5.1 Capability 扫描

标准 PCI 配置空间 capability 链表遍历：Status 寄存器 bit4（Cap List）→ CapPtr(0x34) → 找 CapID=0x05（MSI）。

edu 的 MSI cap 在 0x40，**64 位地址格式**（无 per-vector mask）：

```
+0x00  Cap ID (0x05) + Next Ptr
+0x02  Message Control（bit7=64bit 地址）
+0x04  Message Address LO
+0x08  Message Address HI
+0x0C  Message Data（16 位！）
```

### 5.2 编程值

| 字段 | 值 | 说明 |
|------|-----|------|
| Message Address | 0x08090040 | `GITS_TRANSLATER` 的**物理地址**（设备 DMA 写的目标） |
| Message Data | 0x0000 | EventID = 0（ITT[0] → LPI 8192） |
| Message Control | EN=1, MME=0 | 使能 MSI，1 个向量 |
| Command | INTX_DIS=1 | 禁用 INTx 线 |

Message Data 是 16 位字段，写入时用读-改-写避免覆盖相邻寄存器。

### 5.3 模式切换清理

MSI 使能成功后：
- 设备侧：Command 寄存器置 INTx Disable → INTA 线不再拉高
- GICD 侧：`gic_disable_irq(37)` 关闭 INTx SPI → 双保险

### 5.4 Guest 完全无感知

edu 设备的中断触发逻辑（QEMU edu.c `edu_update_irq`）自动判断 `msi_enabled()`：
- MSI 使能 → `msi_notify()` → 写 doorbell → ITS → LPI
- 否则 → `pci_set_irq()` → INTx 线

Guest 的测试代码（写 IRQ_RAISE → 等虚拟 SPI 50 → ISR 读 irq_status → 写 IRQ_ACK）对两种模式完全一致。**Phase 7 的 Guest 二进制不改一行也能在 Phase 8 上通过 MSI 路径**——这正是设计目标。

---

## 6. LPI 中断处理（irq.c）

```c
if (irq >= 8192) {
    if (pci_edu_msi_enabled &&
        irq >= pci_edu_lpi_base && irq < pci_edu_lpi_base + 16) {
        if (current && current->pid != 0 && current->vmid == 1)
            vgic_inject_virq(VIRQ_PCI_EDU);   /* 软件注入 LR1 */
    } else {
        WARN("LPI %u dropped (no mapping)", irq);
    }
    gic_eoi_irq(irq);   /* EOIR：优先级下降即完成（无 DIR） */
    return;
}
```

三个与 INTx 路径的本质区别：

1. **IAR 读取即清 pending**：`icc_activate_irq` 对 LPI 调用 `gicv3_redist_lpi_pending(irq, 0)`，不需要（也不能）手动清挂起表
2. **无 active 状态**：EOIR 一步完成，没有 split EOI / DIR / HW 联动的问题
3. **注入必须纯软件**：LR 的 HW 映射模式（pINTID 联动物理去活）只适用于 SPI/PPI；LPI 走 LR1 软件注入（与 virtio/ivshmem 共用，注入失败会丢中断——当前单设备场景够用）

---

## 7. 调试记录：一个 Level-0 翻译错误的解剖

本阶段最有价值的调试经历，值得完整记录。

### 7.1 现象

第一版 its.c 运行即 PANIC：

```
INFO[0]: ITS: CPU0 LPI tables: PROP=0x41000000 PEND=0x41001000
!!! PANIC[0]: uncaught exception(SYNC_INVALID_EL2)
    esr: 97c18044, elr: 40094f2c, far: 41012000
```

### 7.2 ESR 解码

```
ESR = 0x97c18044
  EC   = [31:26] = 0b100101 = 37   → Data Abort，当前 EL（EL2 自身访问）
  IL   = 1（长度有效）
  ISS  = 0x1C18044
    ISV = 1, SAS = 3（8 字节）, SRT = 1（x1）
    WnR = 1（写）
    DFSC = 0b000100 = 4 → Level 0 翻译错误
```

指向 put64 的 `str x1, [x0]`（x0 = 0x41012000），8 字节写，Level 0 fault。

### 7.3 排除法定位

1. **不是页池缺页**：DRAM 全 1GB 已用 2MB 块映射（boot_virt.S），0x41012000 在 PMD[8] 内，且同块内 0x41000000–0x41011FFF 刚刚访问成功
2. **不是 memzero**：memzero 是纯汇编 `str xzr`，elr 却精确落在 put64 符号上
3. **Level 0 = PGD[0] 失效**——但同一 PGD 入口下的地址刚访问成功，矛盾！

### 7.4 真相

`far` 打印用了 32 位格式化，**高位被截断**。真实 far = `0x8003000041012000`——正是 `baser0` 寄存器的值（Valid 位 63 + Type 位 + 设备表地址）！

根因：第一版 `gits_write64(val, reg)` 内部写的是 `put64(val, reg)`，而 `put64` 的签名是 **put64(addr, val)**——**参数顺序反了**。把 BASER 的值当地址写入，0x8003... 超出 48 位 VA 空间 → Level 0 翻译错误。

### 7.5 教训

1. `put32/put64/put64` 的参数顺序是 (addr, val)——MMIO 写封装必须逐个检查
2. PANIC 打印 far 应该用 %lx（64 位），截断误导了排查方向
3. 凭记忆写外设寄存器格式不可靠——后续所有 ITS 表项/命令格式全部改为从 QEMU 6.2 源码逐条验证后重写，一次通过

---

## 8. 内存布局（Phase 8 页池分配）

```
LOW_MEMORY = 0x41000000（页池起点，64KB 对齐）
┌────────────────┬──────────────────────────────────────┐
│ 0x41000000     │ LPI pend 表（必须 64KB 对齐 → 首页）  │
│ 0x41001000     │ LPI prop 表                          │
│ 0x41002000     │ ITS 命令队列（4KB = 128 条命令）      │
│ 0x41003000     │ 设备表 DT（BASER0）                   │
│ 0x41004000     │ 集合表 CT（BASER1）                   │
│ 0x41005000     │ ITT（edu 设备，MAPD 注册）            │
│ ...            │ （后续 VM 页表 / ivshmem / Guest 页） │
└────────────────┴──────────────────────────────────────┘
```

---

## 9. 已知限制与未来方向

### 9.1 当前限制

| 限制 | 说明 |
|------|------|
| 仅物理 LPI 直通 | Guest 看到的是虚拟 SPI 50（软件注入），不是虚拟 LPI |
| 单 Redistributor | LPI 只路由到 CPU0；多核场景需要每 CPU 的 pend 表 |
| 软件注入丢中断风险 | LR1 与 virtio/ivshmem 共用，占用时 MSI 会丢（无重试） |
| 无 vITS | Guest 不能自己发 ITS 命令（不能配置自己的 MSI） |
| 无 MSI-X | MSI-X 表（BAR 内映射 + PBA）未实现 |
| 无 LPI 重映射 | IRQ 亲和性固定在分配时的 CPU |

### 9.2 未来方向

1. **vITS 虚拟化**（Phase 9 候选）：陷获 Guest 的 GITS 寄存器访问，翻译成物理 ITS 命令——Guest OS 的标准 ITS 驱动可以直接工作
2. **每 CPU pend 表**：LPI 亲和性/负载均衡
3. **MSI-X**：MSI-X Capability（CapID 0x11）+ BAR 内向量表
4. **直接 LPI 注入（GICv4 DirectLPI）**：vLPI 硬件直通，消除 EL2 陷获

---

## 10. 各阶段对比

| 阶段 | 核心功能 | 关键技术 |
|------|---------|---------|
| Phase 1 | Hypervisor 基础框架 | EL2 初始化、异常向量、MMU Stage-1 |
| Phase 2 | Guest VM 启动 | Stage-2 地址转换、Guest 加载、eret 进入 EL1 |
| Phase 3 | 多 VM 调度 | 轮转调度、世界切换（vGIC + vtimer）、TLB 隔离 |
| Phase 4 | 设备虚拟化 | MMIO 陷获框架、virtio-net、ivshmem 共享内存 |
| Phase 5 | SMP 与多描述符链 | 虚拟 SMP、virtio 多描述符链 |
| Phase 6 | SGI 核间通信 | GICv3 SGI、IPI 抽象层、核间就绪握手 |
| Phase 7 | PCI 直通 | ECAM 枚举、BAR0 MMIO 直通、INTx HW 映射注入 |
| Phase 8 | MSI 中断直通 | GICv3 ITS 驱动、LPI 表、MAPC/MAPD/MAPTI、MSI Capability 编程 |
