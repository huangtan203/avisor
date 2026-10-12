# aVisor Hypervisor Phase 6 详细设计文档 (SGI 核间通信)

> 版本：v0.6.0 (Phase 6)
> 平台：QEMU virt (ARMv8-A, GICv3, 虚拟化扩展)
> 目标：基于 GICv3 SGI（Software Generated Interrupt）实现核间中断（IPI），为后续核间协作（TLB shootdown、调度器 IPI、PCI 直通中断转发等）打下基础

---

## 1. 概述

### 1.1 目标达成情况

| 目标 | 状态 | 实现方式 |
|------|------|---------|
| SGI 发送 | ✅ | `gic_send_sgi()` 通过 `ICC_SGI1R_EL1` 按亲和路由模式触发，INTID 在 [27:24]，目标核用 TargetList（aff0 位图）指定 |
| SGI 使能 | ✅ | `gicr_init()` 中对每个核的 Redistributor 使能 SGI 0-15（`GICR_ISENABLER0 = 0xFFFF`） |
| SGI 接收分发 | ✅ | `irq.c` 的 `handle_irq()` 对 `irq < 16` 分发到 `smp_ipi_handle()` |
| SGI 边沿触发解除激活 | ✅ | split EOI 模式下显式写 `ICC_DIR_EL1` 解除激活，保证同号 SGI 可二次触发 |
| SMP IPI 抽象层 | ✅ | `smp.h` / `smp.c`：`smp_send_ipi()` + `smp_ipi_handle()` + `smp_online[]` |
| 核间就绪握手 | ✅ | 次核置 `smp_online[1]` 后 `sev`，主核 `wfe` 等待后发起 ping-pong |
| ping-pong 闭环验证 | ✅ | CPU0 →(SGI#1)→ CPU1 →(SGI#1)→ CPU0 完整闭环 |

### 1.2 验证结果

**SGI ping-pong 闭环验证：**
```
[cpu0] CPU1 is online, testing SGI IPI...
[cpu0] IPI: sending SGI#1 (ping) to CPU1
[cpu1] IPI: received SGI#1 (count=1)
[cpu0] IPI: received SGI#1 (count=1)
```
- CPU0 向 CPU1 发送 SGI#1（ping），CPU1 收到后回发 SGI#1（pong），CPU0 收到
- 双向核间中断均正常送达，验证了 `ICC_SGI1R_EL1` 的亲和路由与 SGI 边沿触发的 deactivate 流程

**回归验证（Phase 5 功能不受影响）：**
```
[vm1] virtio-net: test1 PASSED (64 bytes, single desc)
[vm1] virtio-net: test2 PASSED (1024 bytes, 4-desc chain)
[vm1] virtio-net: test3 PASSED (256 bytes, 1 desc boundary)
[vm1] virtio-net: test4 PASSED (257 bytes, 2-desc chain)
[vm1] virtio-net: ALL loopback tests PASSED!
[vm2] virtio-net: test1 PASSED (64 bytes, single desc)
[vm2] virtio-net: test2 PASSED (1024 bytes, 4-desc chain)
[vm2] virtio-net: test3 PASSED (256 bytes, 1 desc boundary)
[vm2] virtio-net: test4 PASSED (257 bytes, 2-desc chain)
[vm2] virtio-net: ALL loopback tests PASSED!
```
- VM1（单核）和 VM2（虚拟 SMP）的多描述符链环回测试均全部通过
- 双核 tick 心跳、次核启动均正常，SGI 改动未引入回归

### 1.3 修改的关键文件

```
avisor/
├── include/
│   ├── common/
│   │   └── smp.h               # ★ 新增：SMP 核间通信抽象（smp_online、smp_send_ipi、smp_ipi_handle）
│   └── boards/virt/
│       └── gicv3.h             # ★ 新增 SGI ID 宏（SGI_IPI_NOTIFY/SGI_IPI_TEST）
├── hypervisor/
│   ├── common/
│   │   └── smp.c               # ★ 新增：SMP IPI 分发实现 + 各核 IPI 计数
│   └── boards/virt/
│       ├── gicv3.c             # ★ gicr_init 使能 SGI 0-15；修复 gic_send_sgi 的 ICC_SGI1R_EL1 位布局
│       ├── irq.c               # ★ handle_irq 增加 SGI（irq<16）分发 + 显式 ICC_DIR_EL1 deactivate
│       └── main_virt.c         # ★ 次核就绪后置 smp_online + sev；主核 wfe 等待后发起 SGI ping-pong
```

---

## 2. 总体架构

### 2.1 SGI 核间通信数据流

```
        CPU0 (主核)                              CPU1 (次核)
        ───────────                              ───────────
   smp_send_ipi(1, SGI_IPI_TEST)
        │
        ▼
   gic_send_sgi(1, 1)
        │  写 ICC_SGI1R_EL1
        │  (INTID=1, TargetList bit1)
        ▼
   Redistributor(CPU1) 记录 pending ────────►  GIC 边沿触发 IRQ
        │                                         │
        │                                         ▼
        │                              handle_irq() 读 ICC_IAR1_EL1=1
        │                                         │
        │                                         ▼
        │                              smp_ipi_handle(1)
        │                                   │ (cpu==1 → 回发 pong)
        │                                   ▼
        │                              gic_send_sgi(0, 1)
        │                                         │
        │  写 ICC_DIR_EL1 deactivate   ◄──────────┘
        ▼
   handle_irq() 读 ICC_IAR1_EL1=1
        │
        ▼
   smp_ipi_handle(1)   (cpu==0，不再回发)
        │
        ▼
   写 ICC_DIR_EL1 deactivate（闭环结束）
```

### 2.2 核心设计原则

1. **SGI 是 GICv3 标准的核间中断机制**：与平台无关，任何 ARMv8-A + GICv3 平台（含 AGX Orin）均适用。
2. **亲和路由模式（ARE）**：`GICD_CTLR.ARE_NS=1` 下，`ICC_SGI1R_EL1` 用 aff0/aff1/aff2/aff3 指定目标 PE，天然支持大规模多核。
3. **SGI 号约定集中管理**：在 `gicv3.h` 中定义 `SGI_IPI_NOTIFY`/`SGI_IPI_TEST`，避免魔法数字散落各处。
4. **IPI 抽象层解耦**：上层调用 `smp_send_ipi(cpu, sgi_id)`，不关心底层是 GICv3 还是其他中断控制器。

---

## 3. GICv3 SGI 原理

### 3.1 SGI 中断特性

SGI（Software Generated Interrupt）是 GICv3 定义的 0-15 号软中断，专用于核间通信：

| 特性 | 说明 |
|------|------|
| 编号范围 | 0-15（每核独立，共 16 个） |
| 触发方式 | 软件写 `ICC_SGI1R_EL1`（非外设信号） |
| 触发类型 | **边沿触发**（固定，不可配置为电平） |
| 作用域 | 由发送方指定目标 PE（一个或多个） |
| 配置位置 | 目标 PE 的 Redistributor（SGI 帧） |
| 典型用途 | IPI、TLB shootdown、调度器唤醒、远程函数调用 |

### 3.2 ICC_SGI1R_EL1 位布局（AArch64）

这是本次实现的关键。`ICC_SGI1R_EL1` 在亲和路由模式（ARE_NS=1）下的位布局：

```
63                48 47     44 43 40 39        32
┌──────────────────┬─────────┬──┬──┬─────────────┐
│       Aff3       │   RS    │ R│IRM│    Aff2     │
└──────────────────┴─────────┴──┴──┴─────────────┘
31            27 24 23     16 15                0
┌──────────────┬──────┬──────┬───────────────────┐
│   (RES0)     │INTID │ Aff1 │    TargetList     │
└──────────────┴──────┴──────┴───────────────────┘
```

| 字段 | 位域 | 说明 |
|------|------|------|
| TargetList | [15:0] | aff0 位图：bit n 对应 aff0=n 的 PE |
| Aff1 | [23:16] | 目标 PE 的 Aff1 |
| INTID | [27:24] | SGI 中断号（0-15） |
| Aff2 | [39:32] | 目标 PE 的 Aff2 |
| IRM | [40] | 中断路由模式：1 = 广播给除自身外所有 PE |
| RS | [47:44] | Range Selector（RSS=0 时 RES0） |
| Aff3 | [55:48] | 目标 PE 的 Aff3 |

> **易错点**：SGI 号 INTID 在 [27:24]，**不是** [3:0]；目标核通过 TargetList（aff0 位图）指定，**没有**独立的 Aff0 字段。初次实现把 SGI 号放到 [3:0]、目标核放到 [39:32]，导致 SGI 完全无法送达（目标 Redistributor pending 恒为 0）。

### 3.3 QEMU virt 的亲和路由约定

QEMU virt 平台所有 CPU 的 Aff1/Aff2/Aff3 均为 0，仅 aff0 区分（CPU0 aff0=0，CPU1 aff0=1），故：

- 发送给 CPU n → `TargetList = (1 << n)`，Aff1/Aff2/Aff3 = 0
- 广播给除自身外所有 PE → `IRM=1`，TargetList 忽略

---

## 4. SGI 发送实现

### 4.1 gic_send_sgi()

```c
void gic_send_sgi(unsigned int cpu_id, unsigned int sgi_id)
{
    uint64_t sgi_val;

    sgi_val = ((uint64_t)(sgi_id & 0xF) << 24)   /* INTID [27:24] */
            | (1ULL << (cpu_id & 0xF));            /* TargetList: bit cpu_id */

    icc_sgi1r_write(sgi_val);
    asm volatile("isb" ::: "memory");
}
```

**要点：**
- SGI 号取低 4 位，左移 24 到 INTID 字段
- 目标核取低 4 位，作为 TargetList 的位偏移
- 写完后 `isb` 确保系统寄存器写入生效

---

## 5. SGI 使能与配置

### 5.1 Redistributor 中使能 SGI

SGI/PPI 是 per-CPU 中断，使能/配置在各自 Redistributor 的 SGI 帧（+0x10000）中完成：

```c
static void gicr_init(int cpu_id)
{
    ...
    /* 禁用所有 SGI + PPI */
    put32(sgi_base + GICR_ICENABLER0, 0xFFFFFFFF);

    /* 使能所有 SGI（0-15，核间软中断） */
    put32(sgi_base + GICR_ISENABLER0, 0x0000FFFF);
    ...
    /* SGI 配置为边沿触发 */
    put32(sgi_base + GICR_ICFGR0, 0xFFFFFFFF);
}
```

**要点：**
- `GICR_ISENABLER0` 的 bit 0-15 对应 SGI 0-15，全部使能
- PPI（16-31）仍保持禁用，由具体驱动（如定时器）按需使能
- SGI 固定边沿触发（`ICFGR0` 全 1），这是 SGI 的架构规定

### 5.2 主核 vs 次核

- 主核：`gic_init()` → `gicr_init(0)`，使能 SGI 0-15
- 次核：`gic_init_secondary()` → `gicr_init(smp_processor_id())`，使能本核 SGI 0-15
- 两个核都必须独立使能自己的 SGI，才能作为目标接收核间中断

---

## 6. SGI 接收与解除激活

### 6.1 中断分发

`handle_irq()` 读到 `ICC_IAR1_EL1` 返回的中断号后，对 `irq < 16` 走 SGI 分支：

```c
default:
    if (irq < 16) {
        smp_ipi_handle(irq);
        /*
         * SGI 是边沿触发中断：split EOI 模式下必须显式 DIR
         * 解除激活，否则该 SGI 号一直保持 active，无法再次触发。
         */
        WRITE_SYSREG64(irq, ICC_DIR_EL1);
        asm volatile("isb");
        break;
    }
    WARN("Unknown IRQ: %u", irq);
    break;
```

### 6.2 split EOI 模式下的 deactivate 必要性

本 Hypervisor 使用 split EOI 模式（`ICC_CTLR_EL1.EOImode=1`）：

| 操作 | 效果 |
|------|------|
| 写 `ICC_EOIR1_EL1` | 仅做 priority drop（清 APR），**不**解除激活 |
| 写 `ICC_DIR_EL1` | 真正解除激活（active → inactive） |

对于**边沿触发**的 SGI：
- 若处理完只写 EOIR 不写 DIR，该 SGI 号会一直保持 active
- active 状态下同号 SGI 再次触发会被 GIC 视为已在处理而无法重新 pending
- 因此必须在处理完后**显式写 `ICC_DIR_EL1`** 解除激活

这与此前 EL2 物理定时器（PPI #26）的处理一致，而与虚拟定时器（PPI #27）不同——后者需保持 active 由 Guest 虚拟 EOI 联动硬件 deactivate。

---

## 7. SMP IPI 抽象层

### 7.1 头文件 (include/common/smp.h)

```c
extern volatile int smp_online[MAX_CPUS];
void smp_send_ipi(int cpu, int sgi_id);
void smp_ipi_handle(int sgi_id);
```

### 7.2 实现 (hypervisor/common/smp.c)

```c
volatile int smp_online[MAX_CPUS] = { 0 };
static volatile unsigned long ipi_count[MAX_CPUS];

void smp_send_ipi(int cpu, int sgi_id)
{
    gic_send_sgi(cpu, sgi_id);
}

void smp_ipi_handle(int sgi_id)
{
    int cpu = smp_processor_id();
    ipi_count[cpu]++;

    switch (sgi_id) {
    case SGI_IPI_NOTIFY:
        printf("[cpu%d] IPI: received SGI#%d (count=%lu)\n", ...);
        break;
    case SGI_IPI_TEST:
        printf("[cpu%d] IPI: received SGI#%d (count=%lu)\n", ...);
        if (cpu == 1)   /* ping-pong：CPU1 收到后回 pong 给 CPU0 */
            gic_send_sgi(0, SGI_IPI_TEST);
        break;
    default:
        WARN("Unknown SGI#%d", sgi_id);
        break;
    }
}
```

**职责划分：**
- `smp_send_ipi()`：上层统一的核间中断发送入口，当前直接封装 `gic_send_sgi()`
- `smp_ipi_handle()`：当前核收到 SGI 后的分发处理，在中断上下文执行
- `smp_online[]`：各核初始化完成标志，用于启动握手

---

## 8. 核间就绪握手与 ping-pong 验证

### 8.1 启动握手

次核完成自身初始化（GIC/定时器/vGIC/开中断）后，置起就绪标志并唤醒主核：

```c
/* 次核 hypervisor_secondary_main() 末尾 */
asm volatile("dmb ish" ::: "memory");
smp_online[cpu] = 1;
asm volatile("sev" ::: "memory");
```

主核在启动次核后，用 `wfe` 低功耗等待次核就绪：

```c
/* 主核 hypervisor_main() */
while (!smp_online[1])
    asm volatile("wfe" ::: "memory");
asm volatile("dmb ish" ::: "memory");
```

**要点：**
- `dmb ish` 保证次核写 `smp_online` 前所有初始化对主核可见
- `sev` 唤醒主核的 `wfe`；即便 `sev` 丢失，主核自身的 100Hz 定时器 tick 也会周期性唤醒它重新检查

### 8.2 ping-pong 验证流程

```
CPU0 等待 smp_online[1]
   └─ smp_send_ipi(1, SGI_IPI_TEST)      # ping
        └─► CPU1 smp_ipi_handle(SGI_IPI_TEST)
              └─ cpu==1 → gic_send_sgi(0, SGI_IPI_TEST)   # pong
                    └─► CPU0 smp_ipi_handle(SGI_IPI_TEST)  # 闭环，cpu==0 不再回发
```

---

## 9. 构建与运行

### 9.1 构建

```bash
cd avisor
make -f Makefile.virt clean && make -f Makefile.virt
```

### 9.2 运行（必须 -smp 2 + secure=on）

```bash
qemu-system-aarch64 \
    -M virt,virtualization=on,gic-version=3,secure=on \
    -cpu max -smp 2 -m 1G \
    -kernel bin_virt/kernel8.img \
    -nographic -serial mon:stdio
```

**退出 QEMU：** Ctrl+A 然后按 X

---

## 10. 已知限制与未来优化

### 10.1 当前限制

1. **IPI 语义单一**：目前 SGI 仅用于 ping-pong 演示和通用通知，尚未用于真实协作场景（TLB shootdown、跨核调度唤醒等）
2. **广播未实现**：`ICC_SGI1R_EL1` 的 IRM（广播）位未使用，仅支持单播
3. **TargetList 位图未用满**：当前按 aff0 位图发送，未按多核 Aff 层级路由
4. **UART 输出无锁**：两核并发写 UART 导致输出字符交错（不影响功能，但影响日志可读性）

### 10.2 未来优化方向（Phase 7 预告）

1. **PCI 直通（下一阶段）**：将物理 PCI/PCIe 设备（如 virtio-blk、网卡）直接分配给 Guest，SGI/IPI 可用于设备中断在 vCPU 间的路由与通知
2. **TLB shootdown**：跨核页表变更时用 SGI 通知其他核失效本地 TLB
3. **调度器 IPI**：负载均衡、抢占通知
4. **虚拟 IPI（vSGI）**：向 Guest 注入虚拟 SGI，支撑 Guest 内部多 vCPU 的核间通信
5. **UART 输出锁**：为串口打印加锁，避免多核日志交错

---

## 11. 各阶段对比

| 阶段 | 核心能力 | 关键技术 |
|------|---------|---------|
| Phase 1 | Hypervisor 自身启动到 EL2 | 异常向量、UART、GIC、定时器 |
| Phase 2 | 单 Guest VM 启动 | Stage 2 页表、世界切换、vGIC/vtimer |
| Phase 3 | 多 VM 调度 | 时间片轮转、VMID 切换、TLB 失效 |
| Phase 4 | 设备虚拟化 | MMIO 陷获、virtio 设备模型、ivshmem |
| Phase 5 | SMP 多核 + 多描述符链 | 物理多核启动、虚拟 SMP、per-CPU 调度器、SMP 锁、描述符链 |
| **Phase 6** | **SGI 核间通信** | **ICC_SGI1R_EL1 亲和路由、SGI 边沿触发 deactivate、SMP IPI 抽象、核间握手** |

---

*文档版本：v0.6.0 | 对应代码 tag：phase6-sgi-ipi*
