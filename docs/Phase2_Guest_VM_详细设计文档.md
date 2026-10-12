# aVisor Hypervisor Phase 2 详细设计文档 (Guest VM 基本运行)

> 版本：v0.2.0 (Phase 2)
> 平台：QEMU virt (ARMv8-A, GICv3, 虚拟化扩展)
> 目标：EL1 Guest 镜像加载与启动、Stage 2 地址转换、vGIC 虚拟中断注入、虚拟定时器

---

## 1. 概述

### 1.1 目标达成情况

| 目标 | 状态 | 实现方式 |
|------|------|---------|
| EL1 Guest 镜像加载与启动 | ✅ | 内嵌镜像 + Stage 2 映射 + ERET 进 EL1 |
| Stage 2 地址转换 | ✅ | 3 级页表 + 按需分页 (demand paging) |
| 虚拟中断注入 (vGIC) | ✅ | GICv3 虚拟 CPU 接口 + ICH_LR HW 映射 + split EOI |
| 虚拟定时器 | ✅ | CNTV 寄存器直通 + CNTVOFF 隔离 + PPI #27 翻译注入 |

### 1.2 验证结果

```
[vm1] guest: hello from EL1!
[vm1] guest: running on aVisor hypervisor (QEMU virt)
[vm1] guest: virtual GIC CPU interface initialized
[vm1] guest: virtual timer started (10Hz)
[vm1] guest: interrupts enabled, entering main loop
[vm1] guest: tick 1 (timer + vGIC working)
[vm1] guest: tick 2 (timer + vGIC working)
...（每秒精确一次心跳）
```

Guest 在 EL1 以 10Hz 虚拟定时器运行：每 100ms 收到一次虚拟中断
（GIC 虚拟接口注入），每秒打印心跳。全程 Hypervisor 与 Guest
交替运行，Hypervisor 的 100Hz 调度 tick 不受影响。

### 1.3 新增目录结构

```
avisor/
├── guests/el1_test/               # ★ EL1 Guest 裸机程序（Phase 2 新增）
│   ├── Makefile                   #   独立构建，输出 el1_test.bin
│   ├── linker.ld                  #   链接基址 IPA 0x40000000
│   ├── start.S                    #   EL1 入口（设 VBAR_EL1、栈、清 BSS）
│   ├── vectors.S                  #   EL1 异常向量表（2KB 对齐）
│   ├── main.c                     #   主逻辑 + vGIC/CNTV 驱动
│   └── guest.h                    #   寄存器定义 + HVC 接口
├── hypervisor/
│   ├── boards/virt/
│   │   ├── vgic.c                 # ★ GICv3 虚拟化驱动（vGIC）
│   │   ├── guest_loader.c         # ★ 内嵌镜像加载器
│   │   └── virt_board.c           #   board_ops（CNTVOFF 恢复等）
│   └── common/
│       └── guest_image.S          # ★ .incbin 嵌入 Guest 镜像
└── include/
    ├── boards/virt/
    │   ├── vgic.h                 # ★ vGIC 接口
    │   └── guest_loader.h         # ★ 加载器接口
    ├── arch/aarch64/sysregs.h     #   新增 ICH_*/CNTVOFF 寄存器定义
    └── common/hvc.h               # ★ Hypercall 编号约定
```

---

## 2. 总体架构

### 2.1 Phase 2 系统视图

```
        ┌─────────────────────────────────────────────┐
        │              QEMU virt 机器                  │
        │                                             │
   EL2  │  ┌───────────────────────────────────────┐  │
        │  │         aVisor Hypervisor              │  │
        │  │                                       │  │
        │  │  ┌─────────┐ ┌────────┐ ┌──────────┐  │  │
        │  │  │ 调度器   │ │ mm.c   │ │ sync_exc │  │  │
        │  │  │ sched.c │ │Stage2  │ │ HVC 处理  │  │  │
        │  │  └─────────┘ └────────┘ └──────────┘  │  │
        │  │  ┌─────────┐ ┌────────┐ ┌──────────┐  │  │
        │  │  │ vGIC    │ │vtimer  │ │ gicv3    │  │  │
        │  │  │ vgic.c  │ │翻译注入 │ │ 物理驱动  │  │  │
        │  │  └─────────┘ └────────┘ └──────────┘  │  │
        │  └───────────────────┬───────────────────┘  │
        │                      │ ERET / trap          │
   EL1  │  ┌───────────────────┴───────────────────┐  │
        │  │         Guest VM (pid=1)               │  │
        │  │                                       │  │
        │  │  ┌─────────┐ ┌────────┐ ┌──────────┐  │  │
        │  │  │ 虚拟GIC  │ │ CNTV   │ │ HVC      │  │  │
        │  │  │ ICC_*   │ │ 虚拟   │ │ console  │  │  │
        │  │  │ CPU接口  │ │ 定时器  │ │ 半虚拟化  │  │  │
        │  │  └─────────┘ └────────┘ └──────────┘  │  │
        │  └───────────────────────────────────────┘  │
        └─────────────────────────────────────────────┘
```

### 2.2 关键设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| Guest 镜像分发 | 内嵌 (.incbin) | QEMU virt 无 FAT/SD 卡链路；对应树莓派版从 SD 加载 .bin |
| Guest 控制台 | HVC 半虚拟化 | 无需 MMIO 模拟（树莓派版模拟 BCM2837 UART 的方式太重） |
| Guest 定时器 | CNTV 直通 + 中断翻译 | GICv3 架构原生支持，比树莓派版的软件模拟定时器高效得多 |
| 中断注入 | GICv3 虚拟接口 (ICH_LR) | 硬件级虚拟化，Guest 跑原生 GICv3 驱动零修改 |
| 物理 EOI 策略 | split EOI (EOImode=1) | 防止 vtimer 电平信号风暴 + 防止 RPR 阻塞（见第 9 章） |

---

## 3. Guest VM 生命周期

### 3.1 创建流程

```
hypervisor_main() (main_virt.c)
  │
  ├─ create_task(embedded_binary_loader, &guest_args)   (task.c)
  │    ├─ allocate_page()                # task_struct + 内核栈
  │    ├─ p->board_ops = &virt_board_ops # virt 设备模型
  │    │    └─ virt_initialize(p):       # p->mm.cntvoff = CNTPCT 快照
  │    ├─ prepare_initial_sysregs()      # "干净" EL1 系统寄存器模板
  │    │    （SCTLR_EL1.M=0：Guest 初始 MMU 关闭）
  │    ├─ p->cpu_context.pc = switch_from_kthread
  │    └─ init_task_console(p)           # console fifo
  │
  ├─ task[pid]->priority = 10            # 高优先级（防调度饥饿）
  │
  └─ while (1) { disable_irq(); schedule(); enable_irq(); }  # HV idle 循环
```

### 3.2 首次调度与 ERET 进 EL1

```
schedule() → switch_to(guest) → cpu_switch_to(init, guest)   (sched.S)
  → 恢复 guest->cpu_context → ret 到 switch_from_kthread     (entry.S)
      ├─ prepare_task(loader, arg)                            (task.c)
      │    ├─ regs->pstate = PSR_MODE_EL1h | DAIF 全屏蔽
      │    ├─ loader(arg, regs):                              (guest_loader.c)
      │    │    ├─ for each page of image:
      │    │    │    page = allocate_task_page(current, IPA + off)
      │    │    │    // ↑ 从页池取物理页 + map_stage2_page 建 IPA→PA 映射
      │    │    │    memcpy(page, image_src + off, chunk)
      │    │    └─ regs->pc = 0x40000000 (入口)
      │    └─ set_cpu_sysregs(current)
      │         ├─ set_stage2_pgd: VTTBR_EL2 = PGD | (VMID=pid << 48)
      │         └─ restore_sysregs: 恢复 Guest 的 EL1 系统寄存器
      └─ kernel_exit → vm_entering_work → eret
           → CPU 进入 EL1 @ IPA 0x40000000，开始执行 Guest 的 start.S
```

**关键点**：
- `pt_regs` 就是异常栈帧：`ERET` 用 `ELR_EL2/SPSR_EL2`（= pt_regs.pc/pstate）返回 EL1
- Guest 的 sp 不由 loader 设置（kernel_exit 不恢复 pt_regs.sp），
  由 Guest 的 start.S 自己设栈

### 3.3 运行期的世界切换

Guest 运行期间的世界切换全部通过异常机制：

| 触发 | 路径 | 处理 |
|------|------|------|
| Guest WFI | TWI=1 trap → el01_sync | handle_trap_wfx → schedule() + PC+4 |
| Guest HVC | HCE=1 trap → el01_sync | handle_hvc64（console / 查询频率） |
| 物理定时器 tick (PPI 26) | IMO=1 → el01_irq | timer_tick → 调度 |
| 虚拟定时器到期 (PPI 27) | IMO=1 → el01_irq | 翻译注入 vGIC（见第 6 章） |
| Stage 2 缺页 | el01_sync (DABT) | handle_mem_abort → demand paging |

每次进入 EL2：`kernel_entry` → `vm_leaving_work`（保存 Guest EL1 系统寄存器）；
每次返回 EL1：`vm_entering_work` → `set_cpu_sysregs`（切 Stage 2 页表 + VMID、
恢复系统寄存器）→ `virt_entering_vm`（恢复 CNTVOFF_EL2 + 补注入延迟中断）。

---

## 4. Stage 2 地址转换

### 4.1 两级翻译模型

```
Guest VA ──(Stage 1: Guest 自己的 MMU, 当前关闭)──→ IPA ──(Stage 2: HV)──→ PA
                                          │                        │
                                     VA == IPA (MMU off)    VTTBR_EL2 (per-VM, VMID=pid)
```

- VTCR_EL2 在 boot_virt.S 中设置：38 位 IPA、SL0=1（3 级表）、4KB 粒度、PS=40
- Stage 2 表由 mm.c 的 `map_stage2_page()` 惰性构建（L1→L2→L3）
- 页标志：`MMU_STAGE2_PAGE_FLAGS` = AF + SH + AP(RW) + Normal 属性

### 4.2 物理内存分配

```
0x40080000 ┌──────────────────────────┐
           │ Hypervisor 镜像 (~50KB)  │
           │ + Stage 1 页表 (5 页)     │
           ├──────────────────────────┤
           │ EL2 栈（向下增长）        │
0x41000000 ├──────────────────────────┤ ← LOW_MEMORY
           │ 页池 (位图分配器)         │   PAGING_PAGES 页
           │ - task_struct / stage2 表 │
           │ - Guest 物理页            │
0x80000000 └──────────────────────────┘ ← HIGH_MEMORY
```

### 4.3 按需分页 (Demand Paging)

Guest 访问未映射的 IPA（如第一次压栈写 0x403FFFxx）触发
Stage 2 translation fault：

```
el01_sync (DABT) → handle_mem_abort(addr, esr)     (mm.c)
  ├─ get_ipa(addr):
  │    ├─ Guest MMU 关闭 (SCTLR_EL1.M=0)? → VA == IPA 直接返回
  │    └─ 否则 at s1e1r 硬件翻译
  ├─ ESR.DFSC >> 2 == 0x1 (translation fault)?
  │    └─ 是: allocate_task_page → 分配物理页 + map_stage2_page → 返回重执行
  └─ 0x3 (permission fault)? → MMIO 模拟（Phase 2 未用，guest 走 HVC）
```

**注意**：`get_ipa` 必须检查 Guest 的 SCTLR_EL1.M——
对 MMU 关闭的 guest 执行 `at s1e1r` 会返回 PAR.F=1 导致 IPA 计算错误。

---

## 5. HVC 半虚拟化接口

### 5.1 调用约定

```
Guest:  hvc #0
        x8 = 功能号, x0 = 参数（返回值也放 x0）

HV:     el01_sync → handle_sync_exception (EC=0x16 HVC64)
        → handle_hvc64(hvc_nr=x8)
```

### 5.2 已实现的 Hypercall

| 编号 | 名称 | 参数/返回 | 说明 |
|------|------|----------|------|
| 1 | HVC_CONSOLE_PUTC | x0=字符 | 输出到 HV 控制台，行首自动加 `[vmN]` 前缀 |
| 2 | HVC_GET_TIMER_FREQ | 返回 x0=频率 | 告诉 Guest 定时器频率（62.5MHz） |

Guest 的返回值通过修改 `pt_regs->regs[0]` 实现（kernel_exit 恢复时生效），
与 syscall 返回值的传递方式一致。

---

## 6. vGIC：GICv3 虚拟中断注入

这是 Phase 2 的核心，也是 GICv3 相对 BCM2837/GICv2 的代际优势。

### 6.1 三大硬件机制

**① 虚拟 CPU 接口（自动重定向）**

HCR_EL2.IMO=1 后，Guest 在 EL1 访问 `ICC_*_EL1` 系统寄存器时，硬件
自动重定向到"虚拟视图"：

| Guest 访问 | 实际效果 |
|-----------|---------|
| 读 ICC_IAR1_EL1 | 读 List Register 中的虚拟中断号 |
| 写 ICC_EOIR1_EL1 | 硬件处理 LR 状态机（HW 映射时联动 deactivate 物理中断） |
| 写 ICC_PMR_EL1 | 写入 ICH_VMCR_EL2 的后端字段 (bits[31:24]) |
| 写 ICC_IGRPEN1_EL1 | 写入 ICH_VMCR_EL2.VENG1 (bit1) |

**Guest 跑的是原生 GICv3 驱动，一行不用改。**

**② List Register（注入通道）**

EL2 写 `ICH_LR<n>_EL2` 向 Guest 注入虚拟中断：

```
LR 位布局（与 QEMU/KVM 编码一致）：
  bits[31:0]   vINTID（虚拟中断号）
  bits[41:32]  pINTID（HW=1 时关联的物理中断）
  bits[55:48]  priority
  bit 60       Group（1=Group1）
  bit 61       HW（硬件映射模式）
  bits[63:62]  state（0=invalid, 1=pending, 2=active）
```

**③ HW 映射 + EOI 联动**

`HW=1 + pINTID` 建立虚拟/物理中断的硬件联动：Guest 的虚拟 EOIR
由 GIC 硬件自动 deactivate 对应的物理中断，全程无需 EL2 介入、
无需 maintenance interrupt。

### 6.2 vtimer 中断的完整翻译流程

```
Guest 的 CNTV 到期（寄存器直通，不 trap）
    │ 电平信号
    ▼
物理 PPI #27 ──(IMO=1)──► EL2 IRQ 异常
    │
    ▼
handle_irq() (irq.c):
  ├─ irq = gic_ack_irq()           # 读物理 IAR → 27 变 active
  ├─ current 是 VM? ── 是 ──► vgic_inject_virq_hw(27, 27)
  │      │                        # LR0 = 27 | 27<<32 | GROUP1 | HW | PENDING
  │      │                        # → Guest 虚拟接口立刻看到 v27 pending
  │      └── 否则（HV 在跑）──► vgic_vtimer_defer()
  │                               # 置延迟标志，回 Guest 前 entering_vm 补注入
  └─ gic_eoi_irq(27)              # 物理 EOIR
                                  # ★ split EOI 模式：只降优先级（清 APR），
                                  #   不 deactivate——27 保持 active，
                                  #   电平信号被屏蔽，不会风暴式 re-pend
    │ eret 回 EL1
    ▼
Guest 收到 vIRQ → IRQ 向量 → guest_irq_handler():
  ├─ iar = 读 ICC_IAR1_EL1        # 虚拟 IAR → 27，LR 变 active
  ├─ 重装 CNTV_CVAL               # 清除 ISTATUS（电平信号撤销）
  └─ 写 ICC_EOIR1_EL1             # 虚拟 EOIR
                                  # ★ 硬件联动 deactivate 物理 27
                                  #   （信号已撤销 → 不会再 pending）
```

### 6.3 split EOI：为什么必须设置 EOImode=1

这是 vtimer HW 直通的**最关键配套**，两个死锁场景的解药：

**场景 A（无 split，EOIR 即 deactivate）**：
EOIR → 27 deactivate → vtimer 电平信号（ISTATUS=1）还在 →
GIC 立即 re-pend 27 → EL2 再收 irq27 → …**中断风暴**，
EL2 困在自己身上，Guest 在 EL1 几乎没有执行窗口。

**场景 B（不执行物理 EOIR）**：
EL2 ack 27 时 GIC 置了**物理 APR**（运行优先级 RPR=0）。
guest 的虚拟 EOIR 硬件联动只 deactivate（清 active 位）、
**不清 APR** → RPR 永远卡在 0 → **所有物理中断（包括 HV 自己的
tick PPI 26）被优先级屏蔽，系统静默挂死**。

**split EOI（ICC_CTLR_EL1.EOImode=1）**：
EL2 的物理 EOIR 只降优先级（清 APR，解除对其他中断的阻塞），
不 deactivate（27 保持 active 屏蔽电平信号，无风暴）；
deactivate 交给 Guest 的虚拟 EOIR 硬件联动完成。两个死锁同时解开。

### 6.4 vtimer_pending 延迟注入

WFI-trap 调度循环中，PPI 27 到达 EL2 时若 current 恰好是 HV：
- 不能立即注入（LR 是给 Guest 的）
- 不能丢弃——27 保持 active 屏蔽信号，Guest 收不到中断就不会
  重装 CVAL，**中断永久丢失**

方案：置 `vtimer_pending` 标志，`virt_entering_vm`（每次 ERET 回
Guest 前）检查标志补注入 HW LR。单 VM 场景用一个全局标志即可；
多 VM 时应改为 per-VM 标志。

### 6.5 调度优先级

`_schedule()` 在 counter 相同时永远选索引小的任务（init_task 是 0 号）。
若 Guest 与 HV 同优先级，HV 的调度主循环会反复选自己，Guest 被饿死——
表现为 vtimer 中断到达 EL2 时 current 几乎总是 HV。Phase 2 给
Guest 设 priority=10（HV 为 1），时间片 10 倍于 HV。

---

## 7. 虚拟定时器

### 7.1 寄存器直通

`CNTHCTL_EL2 = 0`（vgic_init 中设置）允许 EL1 直接访问：
- CNTVCT_EL0（虚拟计数器）
- CNTV_CTL/CNTV_CVAL/CNTV_TVAL（虚拟定时器控制）

全部不 trap——这是架构级虚拟化支持（树莓派版需要软件模拟）。

### 7.2 CNTVOFF_EL2（时间隔离）

- VM 创建时（`virt_initialize`）：`tsk->mm.cntvoff = CNTPCT_EL0`（物理计数快照）
- 每次进入 VM（`virt_entering_vm`）：`CNTVOFF_EL2 = tsk->mm.cntvoff`
- 效果：Guest 读到的 CNTVCT 从 0 开始（"新机器刚开机"）
- 多 VM 时每个 VM 有独立偏移，Guest 间时间视图隔离

### 7.3 中断号

| 定时器 | PPI | 使用者 |
|--------|-----|--------|
| EL2 物理定时器 (CNTHP) | #26 | Hypervisor 调度 tick（100Hz） |
| 虚拟定时器 (CNTV) | #27 | Guest（10Hz，翻译注入） |

两者互不干扰：HV 用 CNTHP 自己编程，Guest 用 CNTV 自己编程，
PPI 是 per-CPU 私有的。

---

## 8. Guest 程序设计 (guests/el1_test)

### 8.1 程序结构

```
start.S    EL1 入口：设 VBAR_EL1 + 栈(0x40400000) + 清 BSS → bl guest_main
vectors.S  16 槽异常向量表（.balign 2048！VBAR 要求 2KB 对齐）
main.c     初始化序列 + IRQ handler + 主循环
guest.h    寄存器访问宏 + HVC 接口
```

### 8.2 初始化序列

```c
guest_main():
  1. banner（HVC console）
  2. gic_vcpu_init():
       ICC_PMR_EL1 = 0xFF      // 自动重定向 → VMCR.VPMR
       ICC_IGRPEN1_EL1 = 1     // 自动重定向 → VMCR.VENG1
  3. vtimer_start():
       timer_freq = HVC_GET_TIMER_FREQ()
       CNTV_CVAL = CNTVCT + freq/10    // 10Hz
       CNTV_CTL = ENABLE
  4. msr daifclr, #2           // 开 IRQ
  5. while (1) wfi             // 主循环（WFI trap 给 EL2 顺便触发调度）
```

### 8.3 IRQ Handler

```c
guest_irq_handler():
  iar = ICC_IAR1_EL1           // 虚拟 IAR（读到 LR 注入的 27）
  if (iar == 1023) return      // spurious
  if (iar == 27):
      tick_count++
      CNTV_CVAL = CNTVCT + freq/10   // 重装（清 ISTATUS）
      if (tick % 10 == 0) 打印心跳
  ICC_EOIR1_EL1 = iar          // 虚拟 EOIR（硬件联动 deactivate 物理 27）
```

---

## 9. 调试实录：六个深层 Bug

Phase 2 的调试过程本身是最有价值的产出，以下按发现顺序记录。

### Bug 1：guest 向量表未 2KB 对齐

- **现象**：guest 的 IRQ 向量从未执行（IAR 永远 1023）
- **根因**：vectors.S 只做了 `.align 7`（128 字节），VBAR_EL1 低 12 位
  写入时被硬件忽略，向量表实际地址错位
- **修复**：`.balign 2048`

### Bug 2：HCR_VALUE 符号扩展

- **现象**：读回 HCR_EL2 = 0xffffffff8027613b，**bit34 (E2H/VHE) 意外置位**，
  EL1 的 ICC_* 访问不再重定向到虚拟接口
- **根因**：`HCR_RW` 定义为 `(1 << 31)`——有符号 int 负数，
  `WRITE_SYSREG64` 的 `uint64_t _r = v` 发生符号扩展，高位全 1
- **修复**：所有 HCR 位定义改用 `1UL`
- **教训**：位定义宏必须无符号，尤其是 bit31 这种临界位

### Bug 3：EL2 自身中断风暴（PPI 27 电平信号）

- **现象**：5 秒内 EL2 收到 95810 次 irq27；GIC trace 证明虚拟接口
  一直在正确发 vIRQ 信号，但 guest 几乎没有 EL1 执行窗口
- **根因**：EL2 对 27 做完整 EOIR → deactivate → vtimer 电平信号
  （ISTATUS=1）还在 → GIC re-pend → 无限风暴
- **修复**：见 Bug 5（split EOI 是最终解）

### Bug 4：物理 APR 卡死 RPR

- **现象**：guest 完整走完 IAR/EOIR 流程（trace 铁证），但 tick 26
  之外的物理中断全部停摆（`HPPI update: irq 26 prio 128` 无限刷屏，
  信号发不出）
- **根因**：EL2 ack 27 时置了物理 APR；guest 虚拟 EOIR 的硬件联动只
  deactivate 不清 APR → RPR 永远 0 → 所有物理中断被优先级屏蔽
- **修复**：见 Bug 5

### Bug 5（终解）：split EOI 模式

Bug 3 和 Bug 4 是同一问题的两副面孔：
- 完整 EOIR → 风暴（Bug 3）
- 不做 EOIR → RPR 卡死（Bug 4）

**解药**：`ICC_CTLR_EL1.EOImode=1`（split EOI）——EL2 的物理 EOIR
只降优先级（清 APR）不 deactivate；deactivate 由 guest 虚拟 EOIR 的
HW LR 联动完成。这是 GICv3 下 vtimer 直通的标准姿势。

### Bug 6：调度饥饿

- **现象**：vtimer 中断到达 EL2 时 current 几乎总是 HV，注入无法送达
- **根因**：`_schedule()` 在 counter 相同时选索引小的任务（init_task=0），
  Guest 与 HV 同优先级时被永久饿死
- **修复**：`task[pid]->priority = 10`

### Bug 7：HV 上下文丢弃 vtimer 中断

- **现象**：干净版（WFI 主循环）只有 tick 1，之后心跳消失但系统不挂死
- **根因**：WFI-trap 调度循环中，PPI 27 到达时若 current=HV，
  丢弃后 27 保持 active 屏蔽电平信号，中断永久丢失
- **修复**：defer/flush——HV 上下文置 `vtimer_pending` 标志，
  `virt_entering_vm` 补注入

### 调试方法论沉淀

1. **分层二分**：先验证信号源（GIC trace），再验证信号通路（VI 软件注入），
   最后验证消费端（IAR）
2. **读 QEMU 源码**：`hw/intc/arm_gicv3_cpuif.c` 和 `gicv3_internal.h`
   是 ground truth——VMCR 复位值、EOIR 匹配逻辑、APR 语义全部以它为准
3. **注意版本差异**：本机 QEMU 6.2 与 master 在 el_change_hook 等
   细节有差异，结论要以实际运行版本的源码为准
4. **自制 printf 的 64 位打印**：排查"寄存器值不对"时先排除打印工具
   自身的 bug

---

## 10. 构建与运行

### 10.1 编译

```bash
make -f Makefile.virt        # 自动先构建 guest，再嵌入、编译、链接
```

构建链：`guests/el1_test/*.c/S → el1_test.bin → guest_image.S (.incbin)
→ .text.user 段 → kernel8.img`

### 10.2 运行

```bash
qemu-system-aarch64 \
  -M virt,virtualization=on,gic-version=3 \
  -cpu max -smp 1 -m 1G \
  -kernel bin_virt/kernel8.img \
  -nographic -serial mon:stdio
```

### 10.3 预期输出

```
========================================
  aVisor Hypervisor (QEMU virt port)
  Phase 2: Guest VM bring-up
========================================
[0]~[8] 初始化信息（含 HCR_EL2 = 0x8027613b、vGIC enabled）
...
[vm1] guest: hello from EL1!
[vm1] guest: virtual GIC CPU interface initialized
[vm1] guest: virtual timer started (10Hz)
[vm1] guest: interrupts enabled, entering main loop
[vm1] guest: tick 1 (timer + vGIC working)
[vm1] guest: tick 2 (timer + vGIC working)
...
```

### 10.4 调试技巧

```bash
# GIC 虚拟接口信号追踪
qemu-system-aarch64 ... -d trace:gicv3_cpuif_virt_update,trace:gicv3_cpuif_virt_set_irqs \
  -D /tmp/gic_trace.log

# Guest/EL1 系统寄存器访问追踪
-d trace:gicv3_icv_iar_read,trace:gicv3_icv_eoir_write,trace:gicv3_ich_lr_write
```

---

## 11. 与树莓派版本的实现对比

| 维度 | 树莓派 (BCM2837) | QEMU virt (Phase 2) |
|------|------------------|---------------------|
| Guest 加载 | SD 卡 FAT 文件系统 | 镜像内嵌 (.incbin) |
| Guest 控制台 | MMIO trap 模拟 mini-UART + fifo 转发 | HVC 半虚拟化 |
| 虚拟中断 | HCR.VI 软件注入位 + is_irq_asserted 查询 | GICv3 虚拟接口 + ICH_LR（硬件级） |
| Guest 的中断控制器驱动 | 模拟的 BCM2837 寄存器（MMIO trap） | 原生 GICv3 ICC_* 系统寄存器（零修改） |
| 虚拟定时器 | 软件模拟系统定时器（entering_vm 推进） | CNTV 架构直通 + PPI27 翻译 |
| 物理 EOI | 无 GIC，无此问题 | split EOI + HW LR 联动 |
| 设备模型 | emulator/raspi 全套 MMIO 模拟 | board_ops 仅 CNTVOFF + 延迟注入 |

**核心洞察**：GICv3 的虚拟化设计（虚拟 CPU 接口 + List Register +
EOI 联动）把树莓派版需要软件模拟的所有中断路径都下沉到了硬件，
代码量更少、语义更严格、坑也更深（见第 9 章）。

---

## 12. 后续 Phase 规划

### Phase 3：多 VM 与调度
- 多 Guest 并发（per-VM 的 vtimer_pending、LR 组、VMCR 保存/恢复）
- 调度公平性（轮转指针替代 counter 轮选）
- VM 间 Stage 2 隔离强化（VMID + TLB 失效：切换时 `tlbi vmalle1`）

### Phase 4：设备虚拟化
- MMIO 模拟框架（virt_board_ops.mmio_read/write）
- PL011 UART 直通或模拟（Guest 透明控制台）
- virtio 半虚拟化设备

### Phase 5：AGX Orin 硬件移植
- GICv3 多核 Redistributor 发现
- 真实定时器频率适配
- 参考本项目 docs/AGX_Orin_KVM_使用指南.md

---

## 附录：关键寄存器速查（Phase 2 新增）

| 寄存器 | 编码 | 作用 |
|--------|------|------|
| ICH_HCR_EL2 | S3_4_C12_C11_0 | bit0=En 使能虚拟 CPU 接口 |
| ICH_VTR_EL2 | S3_4_C12_C11_6 | 只读：LR 数量、优先级位宽 |
| ICH_VMCR_EL2 | S3_4_C12_C11_7 | VENG1(bit1)、VPMR[31:24] |
| ICH_LR0_EL2 | S3_4_C12_C12_0 | vINTID[31:0] pINTID[41:32] Group(60) HW(61) State[63:62] |
| CNTVOFF_EL2 | S3_4_C14_C0_3 | 虚拟计数器偏移（per-VM） |
| CNTHCTL_EL2 | S3_4_C14_C1_0 | =0 允许 EL1 直访 CNTV/CNTVCT |
| ICC_CTLR_EL1 | S3_0_C12_C12_4 | bit1=EOImode（split EOI） |
| VBAR_EL1 | S3_0_C12_C0_0 | Guest 向量表基址（2KB 对齐） |
