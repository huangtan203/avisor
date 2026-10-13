# Phase 9：vITS 虚拟化 — 详细设计文档

## 1. 概述

### 1.1 目标

实现 GICv3 vITS（虚拟 Interrupt Translation Service），让 Guest OS 能够使用标准的 ITS 驱动接口（通过 MMIO 读写 GITS 寄存器、向命令队列发命令）来管理 MSI 中断，而不需要 Hypervisor 静态配置 MSI 映射。

### 1.2 背景

- Phase 8 实现了物理 ITS 驱动 + MSI 直通：Hypervisor 静态配置 edu 设备的 MSI→LPI 映射，Guest 收到固定的 vIRQ 50。
- Phase 9 的目标是把 ITS 的控制权下放到 Guest，Guest 可以动态分配 vLPI、配置 EventID→vINTID 映射，Hypervisor 负责翻译到物理 ITS。

### 1.3 平台

- 平台：QEMU virt (ARMv8-A, GICv3 + ITS + 虚拟化扩展)
- Hypervisor：aVisor（EL2，Stage-2 页表 + vGIC）
- Guest：EL1 裸机测试程序（开 Stage-1 MMU，1GB block 等值映射）

### 1.4 交付物

- 虚拟 ITS 寄存器模拟（trap-and-emulate）
- vITS 命令翻译引擎（MAPC / MAPD / MAPTI / SYNC）
- 物理 LPI → (vmid, vINTID) 全局映射表
- Guest 端 vITS 驱动 + 测试用例
- 全量回归通过（virtio-net / ivshmem / edu MSI / vtimer / SMP）

---

## 2. 架构设计

### 2.1 总体架构

```
+------------------ Guest EL1 ------------------+
|                                               |
|  Guest vITS 驱动     edu 设备(直通)            |
|  (读写 0x0C000000)  (BAR0 0x10000000)         |
|       |                    |                  |
+-------|--------------------|------------------+
        | Stage-2 MMIO trap  | Stage-2 直通
        v                    v
+------------------- EL2 ----------------------+
|                                               |
|  vITS 模拟 (0x0C000000)     物理 ITS          |
|  - 寄存器模拟                (0x08080000)      |
|  - 命令队列解析                ^               |
|  - v->p 翻译                   | 写物理命令    |
|  - 分配物理 LPI                |               |
|       |                        |               |
|       v                        v               |
|  +---pLPI---->----vINTID----映射表----+        |
|  |  (irq.c LPI 处理查表注入 vIRQ)      |        |
|  +-----------------------------------+        |
+-----------------------------------------------+
```

### 2.2 地址空间

| 地址范围 | 大小 | 用途 | 映射方式 |
|---------|------|------|---------|
| 0x0C000000 | 64KB | vITS 虚拟 ITS 寄存器 | trap-and-emulate (MMIO) |
| 0x0A000000 | 4KB | virtio-net MMIO | trap-and-emulate |
| 0x0A001000 | 4KB | ivshmem 控制寄存器 | trap-and-emulate |
| 0x0B000000 | 1MB | ivshmem 共享内存 | 真实内存映射 |
| 0x10000000 | 1MB | edu BAR0（PCI 直通） | device 直通 |
| 0x3F000000 | 16MB | PCI ECAM 配置空间 | device 直通 |
| 0x40000000 | ~960MB | Guest RAM | demand paging |

### 2.3 中断路径

MSI 中断从设备到 Guest 的完整路径：

```
设备写 MSI (EventID=1)
  → 物理 ITS 查 DTE → ITT → CTE
    → 产生物理 LPI 9000
      → CPU LPI 中断（EL2）
        → irq.c 读 IAR 拿到 9000
          → 查 vits_lpi_to_virq[9000] → (vmid=1, vINTID=8200)
            → vLPI 转 vSPI：64 + (8200-8192) = 72
              → vgic_inject_virq(72) 注入到 VM1
                → ERET 回 EL1
                  → Guest ICC_IAR1 读到 vSPI 72
```

---

## 3. 模块设计

### 3.1 vITS 核心模块（vits.c / vits.h）

#### 3.1.1 数据结构

```c
struct vits_state {
    uint32_t ctlr;              // GITS_CTLR
    uint64_t cbaser;            // GITS_CBASER（Guest GPA）
    uint64_t cwriter;           // GITS_CWRITER（字节偏移）
    uint64_t creadr;            // GITS_CREADR
    uint64_t baser[8];          // GITS_BASERn
    uint32_t cmdq_bytes;        // 命令队列大小
    struct task_struct *vm;     // 所属 VM
    uint32_t write_lo_cache[8]; // 64位寄存器写缓存
};
```

#### 3.1.2 全局映射表

```c
struct lpi_map_entry {
    uint16_t vmid;     // 0 = 空闲
    uint32_t vintid;   // Guest 视角的中断号
};
static struct lpi_map_entry lpi_map[8192]; // pLPI 8192..16383
```

- 索引方式：`index = pLPI - 8192`
- 分配策略：从 9000 开始递增（避开 Phase 8 的 8192..8207）

#### 3.1.3 命令处理

支持的 ITS 命令：

| 命令 | 操作码 | 处理方式 |
|------|--------|---------|
| SYNC | 0x05 | 空操作（同步处理模型） |
| MAPC | 0x09 | 记录，物理 ITS 已预配置 collection 0 |
| MAPD | 0x08 | 记录，物理 ITS 已预配置设备表 |
| MAPTI | 0x0A | 分配物理 LPI → 建表 → 发物理 MAPTI → 配 LPI 属性 |

**MAPTI 命令处理流程**：
1. 解析 vDevID / vEventID / vINTID / vICID
2. 分配新物理 LPI（`next_alloc_plpi++`）
3. 在 `lpi_map` 中建立 `pLPI → (vmid, vINTID)` 映射
4. 配置物理 LPI 的属性表（优先级 0x60 + 使能）
5. 向物理 ITS 发 MAPTI 命令（使用物理 DevID + 新 pLPI + vEventID）
6. 等物理 ITS 处理完成（CREADR == CWRITER）

### 3.2 MMIO 陷获与分派（virt_board.c）

- 在 `virt_is_mmio_addr` 中增加 vITS 地址范围判断
- 在 `virt_mmio_read` / `virt_mmio_write` 中分派到 `vits_mmio_read` / `vits_mmio_write`
- 在 `virt_initialize` 中为 VM1 初始化 vITS 状态

### 3.3 IPA→PA 翻译（mm.c）

新增 `ipa_to_pa(vm, ipa, pa)` 函数：
- 3 级页表遍历（Level 1 → Level 2 → Level 3）
- 支持 block 和 page 描述符
- 返回 0 成功，-1 未映射

用途：vITS 命令队列在 Guest GPA 空间，Hypervisor 需要翻译到 PA 才能读取命令内容。

### 3.4 LPI 中断注入（irq.c）

改造 LPI 处理逻辑：
1. 先查 vITS 的 `pLPI → (vmid, vINTID)` 映射表
2. 若找到：向目标 VM 注入对应的虚拟 SPI
3. 若没找到：回退到 Phase 8 的硬编码方式（edu 的 8192..8207 → vIRQ 50）

**vLPI → vSPI 转换公式**：
```
vSPI = 64 + (vLPI - 8192)
```

因为 vGIC 的 List Register 只有 10 位 vINTID 字段（最大 1023），无法直接写 LPI 号（≥8192）。因此做线性转换，Guest 侧使用同样的公式。

### 3.5 Guest 端 vITS 驱动（guests/el1_test/main.c）

#### 3.5.1 初始化流程
1. 读 GITS_TYPER 确认能力
2. 分配命令队列（4KB 对齐）
3. 写 GITS_CBASER / GITS_BASER0 / GITS_BASER1
4. 置 GITS_CTLR.Enabled = 1

#### 3.5.2 命令发送
- 写命令到命令队列（32 字节/条）
- 写 GITS_CWRITER 触发处理
- 轮询 GITS_CREADR 等完成（同步模型）

#### 3.5.3 vITS 测试流程
1. 发 MAPC：collection 0 → procnum 0
2. 发 MAPD：devid=16（edu 的 requester_id）→ ITT
3. 发 MAPTI：eventid=1 → vLPI=8200（避免 eventid=0 冲突）
4. 通过 PCI ECAM 修改 edu MSI data = 1（EventID = 1）
5. 触发 edu 中断，验证收到 vSPI 72
6. 恢复 MSI data = 0（保持 Phase 8 兼容）

---

## 4. 关键设计决策

### 4.1 vITS 地址选择

**决策**：vITS 放在 0x0C000000（新的虚拟 IPA 地址），不复用物理 ITS 地址 0x08080000。

**理由**：
- 物理 ITS 寄存器仍需被 Hypervisor 自身（EL2）访问
- 如果复用物理地址，需要区分是 EL2 访问（直通）还是 Guest 访问（trap）——Stage-2 只能控制 Guest 访问，无法区分 EL2 访问
- 新地址方案隔离清晰，互不干扰

### 4.2 vINTID 表示方式

**决策**：Guest 指定 vLPI 号（≥8192），Hypervisor 注入时转换为虚拟 SPI 号。

**理由**：
- GICv3 ITS 规范中 MAPTI 配置的是 LPI（INTID ≥ 8192）
- 但 aVisor 的 vGIC 只有软件注入能力（LR 写 vSPI 号），没有虚拟 LPI（GICv4 的 vLPIs 需要更多硬件支持）
- 折中：Guest 视角是 vLPI，实际注入是 vSPI，通过线性公式转换

### 4.3 ITT 表的归属

**决策**：物理 ITS 使用 Hypervisor 分配的 ITT 页（不是 Guest 提供的 GPA）。

**理由**：
- Guest 提供的 ITT 表在 GPA 空间，经过 Stage-2 翻译后物理上确实存在
- 但安全考虑：Guest 可能随意修改 ITT 表内容，干扰物理 ITS 行为
- 简化实现：Hypervisor 自己管理物理 ITT 表，Guest 的 ITT GPA 只记录不使用

### 4.4 命令处理时机

**决策**：写 CWRITER 时同步处理所有积压命令（CREADR 追上 CWRITER）。

**理由**：
- QEMU 的物理 ITS 也是同步处理模型（写 CWRITER 即处理完）
- 简化 vITS 实现，不需要工作队列或中断上下文处理
- Guest 驱动轮询 CREADR 的方式天然兼容

---

## 5. 已踩过的坑与解决方案

### 5.1 64 位寄存器的 32 位访问问题

**现象**：Guest 用两次 32 位 MMIO 写来写 64 位寄存器（CBASER/BASER），先写低 32 位再写高 32 位。vITS 按精确偏移匹配会报 `unknown write offset` 警告。

**解决**：
- MMIO 写时把 offset 对齐到 8 字节边界
- 低 32 位写：缓存到 per-VM `write_lo_cache[]`
- 高 32 位写：和缓存的低 32 位合并成完整 64 位值

### 5.2 vLPI 不能直接写入 LR

**现象**：GICv3 List Register 的 vINTID 字段只有 10 位（bits[9:0]），最大 1023，LPI 号（≥8192）写进去会被截断。

**解决**：vLPI → vSPI 线性转换（`vSPI = 64 + (vLPI - 8192)`），Guest 侧使用同一公式。

### 5.3 Guest ITT GPA 不可直接用于物理 ITS

**现象**：vITS MAPD 命令中 Guest 提供的 ITT 地址是 GPA，物理 ITS 需要 PA，虽然可以通过 stage-2 翻译，但安全上不允许 Guest 直接修改物理 ITS 的表。

**解决**：Hypervisor 自管理物理 ITT 表（Phase 8 已分配），Guest 的 ITT GPA 只记录不使用。

---

## 6. 验证结果

### 6.1 测试环境

- QEMU 6.2 `-M virt,virtualization=on,gic-version=3,highmem=off -cpu max -smp 2 -m 1G -device edu`
- Hypervisor 镜像：`bin_virt/kernel8.img`（109912 字节）
- Guest 镜像：`guests/el1_test/el1_test.bin`（17768 字节）

### 6.2 vITS 测试用例

| 测试项 | 预期结果 | 实际结果 |
|--------|---------|---------|
| vITS 探测（TYPER） | 读到非零 TYPER 值 | ✅ 0x00A10E11 |
| CBASER/BASER 配置 | 写成功无警告 | ✅ 64 位高/低 32 位合并正确 |
| MAPC 命令 | 成功处理 | ✅ |
| MAPD 命令 | 成功处理 | ✅ |
| MAPTI 命令 | 分配 pLPI 9000，建映射 | ✅ vINTID=8200 → pLPI=9000 |
| 修改 MSI EventID | ECAM 写 MSI data=1 成功 | ✅ |
| 触发 MSI | 收到 vSPI 72 中断 | ✅ **PASSED** |

### 6.3 回归测试

| 测试项 | VM1 | VM2 |
|--------|-----|-----|
| virtio-net loopback (4 个用例) | ✅ 全部通过 | ✅ 全部通过 |
| ivshmem ping-pong | ✅ （VM2 上线后收到回复） | ✅ |
| edu liveness / factorial | ✅ | N/A |
| edu MSI (Phase 8, vIRQ 50) | ✅ | N/A |
| vITS MSI (Phase 9, vSPI 72) | ✅ | N/A |
| 虚拟定时器 tick | ✅ 10Hz | ✅ 5Hz |
| SMP 双核运行 | ✅ CPU0 | ✅ CPU1 |

---

## 7. 文件清单

### 新增文件

| 文件 | 说明 |
|------|------|
| `include/boards/virt/vits.h` | vITS 接口头文件 |
| `hypervisor/boards/virt/vits.c` | vITS 核心实现 |

### 修改文件

| 文件 | 修改内容 |
|------|---------|
| `hypervisor/boards/virt/virt_board.c` | 增加 vITS MMIO 分派 + per-VM 初始化 |
| `hypervisor/boards/virt/main_virt.c` | 调用 vits_global_init() |
| `hypervisor/boards/virt/irq.c` | LPI 处理增加查表注入路径 |
| `hypervisor/common/mm.c` | 新增 ipa_to_pa() 函数 |
| `include/boards/virt/virt_mm.h` | 声明 ipa_to_pa() |
| `guests/el1_test/main.c` | Guest 端 vITS 驱动 + 测试 |

---

## 8. 后续方向（Phase 10+）

- **GICv4 vLPI 直接注入**：利用 GICv4 的虚拟 LPI 能力，vINTID 直接使用 LPI 号无需转换
- **多设备支持**：支持多个直通设备的 vITS 映射
- **多 vITS 实例**：每个 VM 独立的 vITS 设备（目前只有 VM1 有）
- **MSI-X 支持**：扩展到 MSI-X（多个独立 vector）
- **虚拟 ITS doorbell 拦截**：让 Guest 把 MSI 地址配成 vITS doorbell，Hypervisor 翻译后转物理 ITS
- **vITS 命令队列异步处理**：使用 ITS 中断通知处理完成

---

## 9. 版本信息

- 版本：v0.9.0
- Tag：`v0.9.0-phase9-vits`
- 前序版本：v0.8.0 (Phase 8: MSI 中断直通)
- 后续版本：v0.10.0 (Phase 10: TBD)
