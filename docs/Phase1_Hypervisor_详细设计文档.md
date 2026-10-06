# aVisor Hypervisor Phase 1 详细设计文档 (QEMU virt 平台)

> 版本：v0.1.0 (Phase 1)
> 平台：QEMU virt (ARMv8-A, GICv3, 虚拟化扩展)
> 目标：搭建 Hypervisor 最小可运行核心，验证启动、MMU、中断、定时器等基础功能

---

## 1. 项目概述

### 1.1 项目背景

aVisor 是一个用于学习目的的 ARM64 Hypervisor，从树莓派 3 (BCM2837, GICv2) 平台移植到 QEMU virt 平台（GICv3, 标准 ARMv8-A 虚拟化扩展）。

Phase 1 是项目的第一个里程碑，目标是验证 Hypervisor 能够在 EL2 正常运行，并验证以下核心功能：

- ✅ 启动代码正确，能从 EL2 进入 C 函数
- ✅ MMU 开启正常，内存映射正确
- ✅ UART 驱动正常，能打印字符
- ✅ GICv3 中断控制器正常工作
- ✅ 架构定时器能产生周期性中断（100Hz tick）
- ✅ 调度器基本框架就绪

### 1.2 目录结构

```
avisor/
├── Makefile.virt              # Phase 1 构建脚本
├── bin_virt/                  # 编译输出（kernel8.img）
├── build_virt/                # 中间构建文件
├── docs/                      # 文档
│   ├── AGX_Orin_KVM_使用指南.md
│   └── Phase1_Hypervisor_详细设计文档.md  （本文档）
├── hypervisor/
│   ├── arch/aarch64/          # ARM64 架构相关代码
│   │   ├── entry.S            # 异常向量表
│   │   ├── irq.S              # 中断使能/禁用
│   │   ├── sched_s.S          # 任务切换汇编
│   │   ├── sync_exc.c         # 同步异常处理
│   │   ├── task.c             # 任务系统寄存器
│   │   └── utils_s.S          # 工具汇编函数
│   ├── boards/virt/           # QEMU virt 平台相关代码
│   │   ├── boot_virt.S        # 启动代码（EL2 入口、MMU 初始化）
│   │   ├── main_virt.c        # 主入口函数
│   │   ├── gicv3.c            # GICv3 中断控制器驱动
│   │   ├── arch_timer.c       # ARM 架构定时器驱动
│   │   ├── pl011_uart.c       # PL011 UART 驱动
│   │   ├── irq.c              # 中断分发器
│   │   ├── virt_board.c       # 平台板级初始化
│   │   └── linker_virt.ld     # 链接脚本
│   └── common/                # 通用代码（与平台无关）
│       ├── sched.c            # 调度器
│       ├── task.c             # 任务管理
│       ├── mm.c               # 内存管理
│       ├── printf.c           # 格式化输出
│       ├── timer.c            # 通用定时器框架
│       ├── fifo.c             # 环形缓冲区
│       ├── delays.c           # 延时函数
│       ├── debug.c            # 调试/断言
│       ├── utils.c            # 通用工具函数
│       └── entry.c            # 异常入口通用处理
└── include/
    ├── arch/aarch64/          # ARM64 架构头文件
    ├── boards/virt/           # virt 平台头文件
    └── common/                # 通用头文件
```

---

## 2. 内存布局

### 2.1 物理内存布局

QEMU virt 平台的物理地址空间：

| 地址范围 | 大小 | 用途 |
|---------|------|------|
| 0x0000_0000 ~ 0x0800_0000 | 128MB | Flash / 低地址外设 |
| 0x0800_0000 ~ 0x080A_0000 | 640KB | GIC Distributor |
| 0x080A_0000 ~ 0x080C_0000 | 128KB | GIC Redistributor (CPU0) |
| 0x0900_0000 ~ 0x0900_1000 | 4KB | UART0 (PL011) |
| 0x4000_0000 ~ 0x8000_0000 | 1GB | DRAM |

### 2.2 Hypervisor 内存布局

Hypervisor 镜像被 QEMU 加载到 `DRAM_BASE + 0x80000 = 0x4008_0000`（ARM64 Linux 内核标准加载偏移）。

虚拟地址 = 物理地址（等值映射 identity mapping）：

| 地址 | 大小 | 段 | 说明 |
|------|------|----|------|
| 0x4008_0000 | - | `.text.boot` | 启动代码（EL2 入口） |
| - | - | `.text.user` | EL1 测试代码（Phase 1 未使用） |
| - | - | `.text` | 主代码段 |
| - | - | `.rodata` | 只读数据 |
| - | - | `.data` | 初始化数据 |
| - | - | `.bss` | 未初始化数据（清零） |
| - | 4KB 对齐 | `pg_dir` | 页表（5 个页面，20KB） |

### 2.3 页表设计

Phase 1 使用 **等值映射（identity mapping）**，即 VA = PA。

使用 4KB 粒度的 4 级页表（ARMv8-A 标准），但 Phase 1 只映射了两块内存：

1. **DRAM 低 2MB**（0x4000_0000 ~ 0x4020_0000）— 存放 Hypervisor 代码和数据
2. **外设 MMIO 区域**（0x0800_0000 ~ 0x0A00_0000）— GIC、UART 等外设

页表属性：
- 代码段：只读 + 可执行（XN=0）
- 数据段：读写 + 不可执行（XN=1）
- 外设区域：设备内存（Device-nGnRnE）+ 不可执行

---

## 3. 启动流程

### 3.1 启动级别

QEMU virt 平台支持两种启动模式：
- **默认模式**：CPU 从 EL2 启动（HYP 模式）
- **Secure 模式**（`-machine secure=on`）：CPU 从 EL3 启动

aVisor 支持两种情况，如果在 EL3 启动会自动降到 EL2。

### 3.2 启动步骤

```
_start (EL2 或 EL3)
  │
  ├─ 判断 CPU ID（MPIDR_EL1）
  │   ├─ 主核 (CPU0) → 继续执行
  │   └─ 次核 → WFE 挂起
  │
  ├─ 设置 SCTLR_EL2（关闭 MMU、缓存）
  │
  ├─ 判断当前 EL（CurrentEL 寄存器）
  │   ├─ EL3 → 配置 HCR_EL2/SCR_EL3，降到 EL2
  │   └─ EL2 → 直接继续
  │
  ├─ 清零 BSS 段
  │
  ├─ 创建页表（identity mapping）
  │   ├─ 映射 DRAM 区域（0x4000_0000 ~ 0x4020_0000）
  │   └─ 映射外设 MMIO（0x0800_0000 ~ 0x0A00_0000）
  │
  ├─ 配置 MAIR_EL2（内存属性间接寄存器）
  ├─ 设置 TTBR0_EL2（页表基地址）
  ├─ 设置 TCR_EL2（转换控制寄存器）
  │
  ├─ 开启 MMU（SCTLR_EL2.M = 1）
  ├─ 开启 I-Cache 和 D-Cache
  │
  └─ 跳转到 hypervisor_main() （C 语言入口）
```

### 3.3 关键寄存器配置

| 寄存器 | 配置值 | 说明 |
|--------|--------|------|
| SCTLR_EL2 | M=1, C=1, I=1 | 开启 MMU 和缓存 |
| MAIR_EL2 | Attr0=Normal WB, Attr1=Device | 内存属性 |
| TCR_EL2 | T0SZ=25 (39位地址空间), TG0=4KB | 转换控制 |
| TTBR0_EL2 | pg_dir 物理地址 | 页表基址 |
| VBAR_EL2 | vectors 地址 | 异常向量表基址 |
| HCR_EL2 | IMO=1, FMO=1, AMO=1 | 物理异常路由到 EL2 |

---

## 4. GICv3 中断控制器

### 4.1 GICv3 架构概述

GICv3 是 ARMv8-A 的标准中断控制器，相比 GICv2 的主要变化：
- CPU 接口从 MMIO 访问改为**系统寄存器访问**（ICC_*_EL1）
- 支持**亲和路由**（Affinity Routing），适合大规模多核
- 支持 LPI (Locality-specific Peripheral Interrupt)，即 MSI

GICv3 包含三个主要组件：

| 组件 | 功能 | 访问方式 |
|------|------|----------|
| Distributor (GICD) | 全局中断路由、优先级、使能 | MMIO |
| Redistributor (GICR) | 每个 CPU 一个，PPI/SGI 配置 | MMIO (每个 CPU 偏移) |
| CPU Interface | 中断应答、结束、优先级屏蔽 | 系统寄存器 |

### 4.2 中断类型

| 类型 | 中断号范围 | 说明 |
|------|-----------|------|
| SGI | 0 ~ 15 | 软件生成中断（核间中断） |
| PPI | 16 ~ 31 | 私有外设中断（每个 CPU 私有） |
| SPI | 32 ~ 1019 | 共享外设中断（所有 CPU 共享） |
| LPI | 8192+ | 基于消息的中断（Phase 1 未实现） |

### 4.3 QEMU virt 平台的 GIC 地址

| 组件 | 基地址 | 大小 |
|------|--------|------|
| GICD | 0x0800_0000 | 64KB |
| GICR (CPU0) | 0x080A_0000 | 128KB |
| GICR 步长 | - | 0x2_0000 (128KB per CPU) |

每个 Redistributor 有两个 64KB 帧：
- **RD_base**（偏移 0x0000）：控制/配置寄存器
- **SGI_base**（偏移 0x1_0000）：SGI+PPI 寄存器

### 4.4 初始化流程

```
gic_init()
  │
  ├─ gicd_init() — Distributor 初始化
  │   ├─ 关闭 Distributor（GICD_CTLR = 0）
  │   ├─ 设置 ARE_NS 位（启用亲和路由）
  │   ├─ 设置所有 SPI 为 Group 1
  │   └─ 开启 Distributor（GICD_CTLR.EnableGrp1A = 1）
  │
  ├─ gicr_init() — Redistributor 初始化（当前 CPU）
  │   ├─ 等待 GICR 唤醒完成（GICR_WAKER.ProcessorSleep = 0）
  │   ├─ 设置所有 SGI/PPI 为 Group 1
  │   ├─ 清除所有 SGI/PPI 的 pending 状态
  │   └─ 使能 SGI #0（用于核间通信测试）
  │
  └─ gic_cpuif_init() — CPU Interface 初始化
      ├─ 设置 ICC_SRE_EL2（启用系统寄存器模式）
      ├─ 设置 ICC_PMR_EL1（优先级屏蔽，最低优先级 = 0xFF）
      └─ 开启 Group 1（ICC_IGRPEN1_EL1.Enable = 1）
```

### 4.5 中断生命周期

```
  pending → active → inactive
     ↑         │
     │         ↓
  中断触发   处理完成
  (GIC 置位) (写 EOIR)
```

1. **Pending**：中断源触发，GIC 将中断标记为 pending
2. **Active**：CPU 应答中断（读 ICC_IAR1_EL1），中断变为 active
3. **Inactive**：处理完成，写 ICC_EOIR1_EL1 结束中断

### 4.6 关键函数

| 函数 | 文件 | 功能 |
|------|------|------|
| `gic_init()` | gicv3.c | GICv3 总初始化 |
| `gic_ack_irq()` | gicv3.c | 应答中断，返回中断号 |
| `gic_eoi_irq(irq)` | gicv3.c | 结束中断 |
| `gic_enable_irq(irq)` | gicv3.c | 使能指定中断 |
| `gic_disable_irq(irq)` | gicv3.c | 禁用指定中断 |
| `gic_set_priority(irq, prio)` | gicv3.c | 设置中断优先级 |
| `gic_send_sgi(cpu, sgi_id)` | gicv3.c | 发送 SGI 到指定 CPU |

---

## 5. 架构定时器

### 5.1 定时器概述

ARMv8-A 提供架构定时器（Architected Timer），每个 CPU 有多个定时器：

| 定时器 | 寄存器 | 异常级别 | 用途 |
|--------|--------|---------|------|
| EL1 物理定时器 | CNTP_CTL_EL0 | EL1 | Guest OS 系统 tick |
| EL1 虚拟定时器 | CNTV_CTL_EL0 | EL1 | Guest 虚拟时间 |
| EL2 物理定时器 | CNTHP_CTL_EL2 | EL2 | Hypervisor 系统 tick |
| EL2 虚拟定时器 | CNTHV_CTL_EL2 | EL2 | - |

Phase 1 使用 **EL2 物理定时器**作为 Hypervisor 的系统 tick。

### 5.2 定时器频率

QEMU virt 平台的定时器频率为 **62.5 MHz**（读取 CNTFRQ_EL0 获得）。

100Hz tick 的计数间隔：`62500000 / 100 = 625000`

### 5.3 中断号

EL2 物理定时器中断是 **PPI #26**（每个 CPU 私有）。

### 5.4 初始化流程

```
timer_init()
  │
  ├─ 读取 CNTFRQ_EL0（获取定时器频率）
  ├─ 计算 100Hz 的 tick 间隔
  ├─ 使能 GIC 中的 PPI #26
  ├─ 设置 CNTHP_TVAL_EL2（加载初始值）
  └─ 开启定时器（CNTHP_CTL_EL2.ENABLE = 1）
```

### 5.5 中断处理

```
handle_timer_irq()  (arch_timer.c)
  │
  ├─ 读取中断状态（确认是定时器中断）
  ├─ 重新加载 TVAL（下一次中断的间隔）
  └─ 调用 timer_tick()  (common/timer.c)
       │
       └─ tick_count++
           └─ 每 100 tick（1秒）打印一次状态
```

---

## 6. 异常处理

### 6.1 异常向量表

ARMv8-A 的异常向量表包含 **16 个向量**，按异常类型和来源分类：

| 偏移 | 类型 | 来源 | 当前实现 |
|------|------|------|---------|
| 0x000 | Synchronous | Current EL with SP0 | `handle_invalid_entry` |
| 0x080 | IRQ | Current EL with SP0 | `handle_invalid_entry` |
| 0x100 | FIQ | Current EL with SP0 | `handle_invalid_entry` |
| 0x180 | SError | Current EL with SP0 | `handle_invalid_entry` |
| 0x200 | Synchronous | Current EL with SPx | `handle_invalid_entry` |
| 0x280 | IRQ | Current EL with SPx | **`kernel_entry` → `handle_irq`** |
| 0x300 | FIQ | Current EL with SPx | `handle_invalid_entry` |
| 0x380 | SError | Current EL with SPx | `handle_invalid_entry` |
| 0x400 | Synchronous | Lower EL (AArch64) | `handle_invalid_entry` |
| 0x480 | IRQ | Lower EL (AArch64) | `handle_invalid_entry` |
| 0x500 | FIQ | Lower EL (AArch64) | `handle_invalid_entry` |
| 0x580 | SError | Lower EL (AArch64) | `handle_invalid_entry` |
| 0x600 | Synchronous | Lower EL (AArch32) | `handle_invalid_entry` |
| 0x680 | IRQ | Lower EL (AArch32) | `handle_invalid_entry` |
| 0x700 | FIQ | Lower EL (AArch32) | `handle_invalid_entry` |
| 0x780 | SError | Lower EL (AArch32) | `handle_invalid_entry` |

### 6.2 IRQ 处理流程

Phase 1 中，IRQ 异常的完整流程：

```
外设/定时器产生中断
        │
        ▼
  GIC 检测到中断
        │
        ▼
  GIC 向 CPU 发送 IRQ 信号
        │
        ▼
  CPU 跳转到 0x280 (IRQ, Current EL, SPx)
        │
        ▼
  kernel_entry (entry.S)
    ├─ 保存寄存器上下文（x0-x30, elr_el2, spsr_el2 等）
    ├─ bl vm_leaving_work()  （对 idle task 跳过）
    └─ bl handle_irq()
             │
             ▼
      handle_irq() (irq.c)
        ├─ gic_ack_irq()   — 读 ICC_IAR1_EL1，获取中断号
        ├─ switch (irq)    — 根据中断号分发
        │   ├─ PPI #26 → handle_timer_irq()  （定时器）
        │   └─ SPI #33 → handle_uart_irq()   （串口）
        └─ gic_eoi_irq()   — 写 ICC_EOIR1_EL1，结束中断
             │
             ▼
  kernel_exit (entry.S)
    ├─ bl vm_entering_work()  （对 idle task 跳过）
    └─ 恢复寄存器上下文，eret 返回
```

### 6.3 HCR_EL2 的作用

**这是 Phase 1 调试中发现的关键知识点：**

`HCR_EL2.IMO`（bit 4, IRQ Override）必须设为 1，EL2 才能接收物理 IRQ。

当 `IMO=0` 时：
- 即使 DAIF.I=0（中断已开启）
- 即使 GIC 中断 pending
- WFI 能被唤醒（说明中断信号到达了 CPU）
- 但**异常向量不会触发**（CPU 不会跳转到 IRQ 向量）

这是因为 HCR_EL2.IMO 控制物理 IRQ 是否路由到 EL2。当 IMO=0 时，物理 IRQ 会被路由到 EL1（但我们在 EL2，所以没有任何异常级别处理它）。

**必须设置的位：**
- `IMO` (bit 4) = 1：物理 IRQ → EL2
- `FMO` (bit 3) = 1：物理 FIQ → EL2
- `AMO` (bit 5) = 1：物理 SError → EL2

---

## 7. 调度器框架

### 7.1 任务结构

`struct task_struct` 定义在 `include/common/sched.h`，包含：

| 字段 | 类型 | 说明 |
|------|------|------|
| cpu_context | struct | 切换时保存/恢复的寄存器 |
| state | - | 任务状态（RUNNING, ZOMBIE, ...） |
| pid | long | 进程 ID（也用作 VMID） |
| name | char[] | 任务名称 |
| mm | - | 内存管理（Stage 2 页表） |
| cpu_sysregs | - | VM 的系统寄存器上下文 |
| stat | - | 统计信息 |
| console | - | 控制台缓冲区 |

### 7.2 Idle Task

系统中有一个特殊的 idle task（pid = 0），代表 Hypervisor 自身。

在 Phase 1 中：
- `current` 指针始终指向 `init_task`（idle task）
- 没有任务切换
- 主循环执行 `WFI` 等待中断

### 7.3 vm_leaving_work / vm_entering_work

这两个函数在异常入口/出口处调用，用于保存/恢复 VM 的系统寄存器状态。

**对 idle task（pid=0）直接跳过**，因为：
1. idle task 不是 VM，不需要保存/恢复系统寄存器
2. `save_sysregs` 会访问 `fpcr`/`fpsr`（浮点寄存器），如果浮点未启用会触发 undefined instruction 异常，导致异常递归

---

## 8. 平台差异（vs 树莓派版本）

| 特性 | 树莓派 3 (BCM2837) | QEMU virt |
|------|-------------------|-----------|
| GIC 版本 | GICv2 (MMIO CPU IF) | GICv3 (系统寄存器 CPU IF) |
| 中断控制器 | BCM2837 + GICv2 | GICv3 (标准) |
| DRAM 基地址 | 0x0000_0000 | 0x4000_0000 |
| 内核加载地址 | 0x0000_0000 | 0x4008_0000 |
| UART | mini UART (BCM2835) | PL011 |
| 外设基地址 | 0x3F00_0000 | 0x0800_0000 |
| 定时器 | BCM2835 系统定时器 | ARM 架构定时器 |
| CPU | Cortex-A53 (4核) | Cortex-A72/A57 (可配置) |

---

## 9. 构建与运行

### 9.1 编译

```bash
make -f Makefile.virt
```

输出：`bin_virt/kernel8.img`（约 45KB）

### 9.2 运行

```bash
qemu-system-aarch64 \
  -M virt,virtualization=on,gic-version=3 \
  -cpu max \
  -smp 1 \
  -m 1G \
  -kernel bin_virt/kernel8.img \
  -nographic \
  -serial mon:stdio
```

参数说明：
- `-M virt,virtualization=on,gic-version=3`：virt 平台，启用虚拟化，GICv3
- `-cpu max`：使用最高级的 CPU 模型（支持虚拟化扩展）
- `-kernel bin_virt/kernel8.img`：加载 Hypervisor 镜像
- `-nographic`：无图形界面
- `-serial mon:stdio`：串口重定向到标准输入输出

退出 QEMU：`Ctrl+A` 然后按 `X`

### 9.3 预期输出

```
========================================
  aVisor Hypervisor (QEMU virt port)
  Phase 1: Core bring-up
========================================

[0] UART initialized (PL011, 115200 baud)
[1] Initial task set up (pid=0, name=HV)
[2] Exception vectors installed (VBAR_EL2)
INFO[0]: GICv3: support 224 SPI interrupts
INFO[0]: GICv3: ICC_SRE_EL2 = 0xf (SRE=1)
INFO[0]: GICv3 initialized.
[3] GICv3 initialized (Distributor + Redistributor + CPU IF)
INFO[0]: Architected Timer frequency: 62500000 Hz
INFO[0]: Timer tick: 100 Hz, interval: 625000 ticks
INFO[0]: Architected Timer initialized.
[4] Architected timer initialized (100Hz tick)
[5] HCR_EL2 configured (IMO=1, FMO=1, AMO=1)
[6] Interrupts enabled (IRQ + FIQ)

All core systems online. Entering idle loop...
(Timer ticks at 100Hz, status prints every second)

[tick 100] Hypervisor is alive at EL2
[tick 200] Hypervisor is alive at EL2
[tick 300] Hypervisor is alive at EL2
...
```

---

## 10. 已知 Bug 与修复历史

### Bug 1：全局变量初始值全为 0

- **现象**：`current` 指针等全局变量值为 0（NULL）
- **根因**：QEMU `-kernel` 加载到 0x4008_0000，但链接地址是 0x4000_0000，数据段错位
- **修复**：`linker_virt.ld` 起始地址改为 0x4008_0000

### Bug 2：GIC 初始化挂死在 gicr_init

- **现象**：系统在 GIC 初始化时挂死
- **根因**：GICR SGI 帧寄存器宏重复加了 GICR_SGI_BASE 偏移
- **修复**：`gicv3.h` 中 GICR 寄存器宏去掉 GICR_SGI_BASE 前缀

### Bug 3：enable_irq() 不生效

- **现象**：PSTATE.I 始终为 1，中断无法开启
- **根因**：`msr daifclr, #4` 立即数错误（bit2 是 A，不是 I）
- **修复**：`irq.S` 中 `#4` → `#2`

### Bug 4：GIC 中断挂起但不触发 IRQ 异常

- **现象**：ISPENDR 置 1，IAR 能读到中断号，DAIF I=0，WFI 能被唤醒，但异常向量不触发
- **根因**：HCR_EL2.IMO=0，EL2 下物理 IRQ 信号未被正确路由
- **修复**：设置 HCR_EL2.IMO=1（同时设置 FMO=1, AMO=1）

### Bug 5：IRQ 触发后系统崩溃（无限异常递归）

- **现象**：IRQ 触发后系统不停输出调试字符，死循环
- **根因**：`vm_leaving_work` → `save_sysregs` 访问 `fpcr`/`fpsr`，但浮点未启用，触发 undefined instruction 同步异常，异常处理又触发异常，无限递归
- **修复**：`vm_leaving_work`/`vm_entering_work` 对 idle task（pid=0）直接跳过

---

## 11. 后续 Phase 规划

### Phase 2：Guest VM 基本运行
- EL1 Guest 镜像加载与启动
- Stage 2 地址转换
- 虚拟中断注入（vGIC）
- 虚拟定时器

### Phase 3：多 VM 与调度
- 多个 Guest VM 并发运行
- 时间片调度
- VM 间隔离

### Phase 4：设备虚拟化
- 半虚拟化设备（virtio）
- 直通设备（PCIe passthrough）

### Phase 5：AGX Orin 硬件移植
- 移植到 NVIDIA AGX Orin 开发板
- 适配 Orin 的 GICv3 和外设
- KVM 接口兼容

---

## 附录 A：关键寄存器速查表

### A.1 GICv3 系统寄存器

| 寄存器名 | 编码 | 读写 | 功能 |
|---------|------|------|------|
| ICC_IAR1_EL1 | S3_0_C12_C12_0 | RO | 中断应答寄存器（Group 1） |
| ICC_EOIR1_EL1 | S3_0_C12_C12_1 | WO | 中断结束寄存器（Group 1） |
| ICC_PMR_EL1 | S3_0_C4_C6_0 | RW | 优先级屏蔽寄存器 |
| ICC_IGRPEN1_EL1 | S3_0_C12_C12_7 | RW | Group 1 使能寄存器 |
| ICC_SRE_EL2 | S3_4_C12_C9_5 | RW | 系统寄存器使能（EL2） |
| ICC_SGI1R_EL1 | S3_0_C12_C11_5 | WO | SGI 生成寄存器 |

### A.2 定时器寄存器

| 寄存器名 | 功能 |
|---------|------|
| CNTHP_CTL_EL2 | EL2 物理定时器控制 |
| CNTHP_TVAL_EL2 | EL2 物理定时器计数值 |
| CNTHP_CVAL_EL2 | EL2 物理定时器比较值 |
| CNTFRQ_EL0 | 定时器频率 |
| CNTPCT_EL0 | 当前计数 |

### A.3 HCR_EL2 关键位

| 位 | 名称 | 功能 |
|----|------|------|
| 0 | VM | Virtualization enable |
| 3 | FMO | FIQ Override（物理 FIQ 路由到 EL2） |
| 4 | IMO | IRQ Override（物理 IRQ 路由到 EL2） |
| 5 | AMO | SError Override（物理 SError 路由到 EL2） |
| 31 | RW | 下一级 EL 的宽度（1=64位, 0=32位） |
