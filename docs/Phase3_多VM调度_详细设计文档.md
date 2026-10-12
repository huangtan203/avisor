# aVisor Hypervisor Phase 3 详细设计文档 (多 VM 与调度)

> 版本：v0.3.0 (Phase 3)
> 平台：QEMU virt (ARMv8-A, GICv3, 虚拟化扩展)
> 目标：多 VM 并发运行、时间片轮转调度公平性、VM 间隔离强化

---

## 1. 概述

### 1.1 目标达成情况

| 目标 | 状态 | 实现方式 |
|------|------|---------|
| 多 VM 并发运行 | ✅ | 同镜像创建两个 Guest 实例，各自独立 Stage 2 + 虚拟化状态 |
| 调度公平性 | ✅ | 轮转起点 (rr_start) + 时间片轮转，两 VM tick 数 1:1 交替 |
| 世界切换保存/恢复 | ✅ | switch_to 中调用 board_ops.leaving_vm / entering_vm |
| vGIC 虚拟接口隔离 | ✅ | ICH_VMCR / ICH_LR0 按 VM 保存恢复 |
| 虚拟定时器隔离 | ✅ | CNTV_CVAL / CNTV_CTL 按 VM 保存恢复 + CNTVOFF 偏移 |
| Stage 2 TLB 隔离强化 | ✅ | VMID 切换时 tlbi vmalle1 失效旧 VMID 的 TLB 条目 |
| PPI 27 active 状态切换 | ✅ | 切出 VM 时 ICC_DIR 强制 deactivate 物理 27 |
| HVC_GET_VMID 调用 | ✅ | Guest 查询自身 VM 编号，用于差异化频率配置 |

### 1.2 验证结果

```
[vm1] guest: hello from EL1!
[vm1] guest: I am VM 1, running on aVisor hypervisor
[vm1] guest: virtual GIC CPU interface initialized
[vm1] guest: virtual timer started
[vm1] guest: interrupts enabled, entering main loop
[vm2] guest: hello from EL1!
[vm2] guest: I am VM 2, running on aVisor hypervisor
[vm2] guest: virtual GIC CPU interface initialized
[vm2] guest: virtual timer started
[vm2] guest: interrupts enabled, entering main loop
[tick 100] Hypervisor is alive at EL2
[vm1] tick 1
[vm2] tick 1
[vm1] tick 2
[tick 200] Hypervisor is alive at EL2
[vm2] tick 2
[vm1] tick 3
[tick 300] Hypervisor is alive at EL2
[vm2] tick 3
...（两 VM 每秒各一次心跳，严格交替）
```

**关键指标**：
- 双 VM 均正常启动，独立运行
- VM1（10Hz vtimer）+ VM2（5Hz vtimer）→ PPI 27 合计约 15Hz
- EL2 物理定时器稳定 100Hz
- Spurious 中断 = 0
- 调度公平性：两 VM 心跳 1:1 交替，无饥饿

### 1.3 修改的关键文件

```
avisor/
├── include/
│   ├── common/sched.h            # ★ 新增 struct vm_virt_state（per-VM 虚拟化状态）
│   └── common/hvc.h              # ★ 新增 HVC_GET_VMID 调用号
├── hypervisor/
│   ├── common/sched.c            # ★ 世界切换移入 switch_to + 轮转公平性 + TLB 失效
│   ├── arch/aarch64/sync_exc.c   # ★ 新增 HVC_GET_VMID 处理
│   └── boards/virt/
│       ├── virt_board.c          # ★ 完整世界切换实现（leaving_vm / entering_vm）
│       ├── gicv3.c               # ★ 开启 split EOI 模式（EOImode=1）
│       └── irq.c                 # ★ PPI 27 HW 注入 + split EOI 处理；PPI 26 手动 DIR
└── guests/el1_test/
    └── main.c                    # ★ 差异化频率（vm1:10Hz, vm2:5Hz）+ VMID 查询
```

---

## 2. 总体架构

### 2.1 Phase 3 系统视图

```
                    ┌───────────────────────────────────────────┐
                    │          aVisor Hypervisor (EL2)          │
                    │                                            │
                    │  ┌──────────┐    ┌─────────────────────┐  │
                    │  │ Scheduler│    │ World Switch Logic  │  │
                    │  │  (tick)  │───▶│  switch_to()        │  │
                    │  │ rr_start │    │  - leaving_vm(prev) │  │
                    │  └──────────┘    │  - entering_vm(next)│  │
                    │                  └─────────────────────┘  │
                    │                       │                   │
                    │                       ▼                   │
                    │        ┌───────────────────────┐          │
                    │        │   board_ops (virt)    │          │
                    │        │  ┌─────────────────┐  │          │
                    │        │  │  leaving_vm     │  │          │
                    │        │  │  - save VMCR    │  │          │
                    │        │  │  - save LR0     │  │          │
                    │        │  │  - DIR PPI27    │  │          │
                    │        │  │  - save CNTV    │  │          │
                    │        │  │  - disable CNTV │  │          │
                    │        │  └─────────────────┘  │          │
                    │        │  ┌─────────────────┐  │          │
                    │        │  │  entering_vm    │  │          │
                    │        │  │  - restore OFF  │  │          │
                    │        │  │  - restore VMCR │  │          │
                    │        │  │  - restore LR0  │  │          │
                    │        │  │  - restore CNTV │  │          │
                    │        │  │  - inject pend  │  │          │
                    │        │  └─────────────────┘  │          │
                    │        └───────────────────────┘          │
                    └───────────────────────────────────────────┘
                          │                   │
                          ▼                   ▼
                    ┌──────────┐       ┌──────────┐
                    │  VM 1    │       │  VM 2    │
                    │ (EL1)    │       │ (EL1)    │
                    │ 10Hz vtmr│       │ 5Hz vtmr │
                    │ VMID=1   │       │ VMID=2   │
                    └──────────┘       └──────────┘
```

### 2.2 关键设计决策

| 决策 | 方案 | 理由 |
|------|------|------|
| 世界切换时机 | **仅在 switch_to 中做**，不在每次进出 EL2 做 | 进出 EL2 太频繁（每次中断/异常都进出），反复保存恢复会破坏中断上下文（如 active 状态的 vtimer），且性能差 |
| PPI 27 active 切换 | 切出时 ICC_DIR 强制 deactivate，vtimer_pending 重注入 | 物理 PPI 27 只有一个，上一个 VM 的 active 状态会屏蔽下一个 VM 的 vtimer 信号。定时器中断可重入，退化为 pending 延迟注入是可接受的 |
| CNTV 禁用时机 | 切出 VM 时禁用（CTL=0），切入时恢复 | VM 不在 CPU 上时其定时器不应产生物理中断，否则中断归属歧义 |
| 调度公平性 | rr_start 轮转起点 + counter-based 时间片 | 原实现按固定索引扫描，counter 相同时索引小的永远先跑，产生系统性偏置。轮转起点确保相同 counter 下轮流胜出 |
| TLB 隔离 | VMID 切换时 tlbi vmalle1 | 硬件 TLB 按 VMID 标记，但 QEMU 等模拟器实现可能不严格；且 VM 销毁后 VMID 复用必然错误。切换前保守失效 |
| split EOI | ICC_CTLR_EL1.EOImode = 1 | vtimer HW 直通注入的前提：EL2 ack 后只降优先级不 deactivate，保持物理 27 active 以屏蔽电平信号，防止 re-pend 风暴 |

---

## 3. 世界切换（World Switch）

### 3.1 为什么世界切换不能放在每次进出 EL2

最初的设计将 `leaving_vm` / `entering_vm` 放在 `vm_leaving_work` / `vm_entering_work` 中，即**每次进出 EL2 都做一次世界切换**。这导致了两个严重问题：

**问题 1：Spurious 中断风暴**

leaving_vm 在 `handle_irq` **之前**执行。如果异常是 PPI 27（虚拟定时器）引起的：

```
PPI 27 pending → CPU 响应 → 进入 EL2
  → vm_leaving_work
    → save_sysregs
    → leaving_vm
      → ...
      → 写 CNTV_CTL=0 （禁用虚拟定时器）
      → ISTATUS=0 → 电平信号变低
      → GIC 中 PPI 27 仍在 pending 状态（尚未 ack）
      → 电平变低 → pending 被清除
  → handle_irq
    → gic_ack_irq() → 读 IAR → 返回 1023（spurious！）
```

CPU 已经响应了 IRQ（进入异常），但软件读 IAR 之前，leaving_vm 把 CNTV 禁用了，清除了电平信号，GIC 撤销了 pending 状态 → spurious 中断。

然后 entering_vm 中又恢复 CNTV → PPI 27 重新 pending → eret 回 EL1 → 又触发 → 无限循环。

**问题 2：PPI 27 中断风暴（Guest 虚拟 EOI 联动 deactivate）**

如果 leaving_vm 中做 ICC_DIR deactivate 物理 27，每次进出 EL2 都 deactivate 一次，然后恢复 CNTV 又导致 re-pend，同样产生风暴。

**结论**：世界切换（保存/恢复 + 清理）只能在**真正切换 VM 时**做，即 `switch_to` 中。同一个 VM 进出 EL2 时，虚拟化状态保持不变。

### 3.2 正确的调用路径

```
异常进入 EL2:
  kernel_entry
    → vm_leaving_work
      → save_sysregs            // 保存 EL1 系统寄存器（必须最先做）
      → flush_task_console      // UART 转发（如有）
  → handle_irq / handle_sync_exception  // 处理异常
  → [可能调用 _schedule → switch_to]
    → leaving_vm(prev)          // 保存 prev 虚拟化状态 + 清理
    → current = next
    → entering_vm(next)         // 恢复 next 虚拟化状态
    → cpu_switch_to(prev, next) // 栈切换
异常返回 EL1:
  kernel_exit
    → vm_entering_work
      → flush_task_console
      → set_cpu_sysregs          // 恢复系统寄存器 + Stage 2 + TLB 失效
      → set_cpu_virtual_interrupt
    → eret
```

**关键点**：
- `vm_leaving_work` / `vm_entering_work` 每次进出都执行，但**只做系统寄存器的保存/恢复**，不碰虚拟化状态
- `leaving_vm` / `entering_vm` **只在 switch_to 中调用**，即只有真正切换 VM 时才做世界切换
- leaving_vm 在 handle_irq **之后**执行，此时中断已 ack，可以安全地禁用 CNTV、deactivate 物理中断等操作

### 3.3 per-VM 虚拟化状态结构

```c
struct vm_virt_state {
    unsigned long gic_vmcr;     // ICH_VMCR_EL2（虚拟 CPU 接口控制）
    unsigned long gic_lr0;      // ICH_LR0_EL2（虚拟中断 List Register 0）
    unsigned long cntv_ctl;     // CNTV_CTL_EL0（虚拟定时器控制）
    unsigned long cntv_cval;    // CNTV_CVAL_EL0（虚拟定时器比较值）
    int vtimer_pending;         // 虚拟定时器延迟注入标志
};
```

嵌入在 `struct task_struct` 中：

```c
struct task_struct {
    ...
    struct mm_struct mm;        // 内存管理（含 Stage 2 + CNTVOFF）
    struct cpu_sysregs cpu_sysregs; // EL1 系统寄存器
    struct vm_virt_state virt;  // ★ per-VM 虚拟化状态（Phase 3 新增）
    ...
};
```

### 3.4 leaving_vm 完整流程

在 `switch_to` 中、切换 `current` 之前调用。此时 prev VM 的状态仍在硬件寄存器中。

```
virt_leaving_vm(tsk):
  1. 保存 ICH_VMCR_EL2 → tsk->virt.gic_vmcr
  2. 保存 ICH_LR0_EL2 → tsk->virt.gic_lr0
  
  3. 若 LR0 是 HW 模式且非 invalid：
     - 提取 pINTID
     - 若 pINTID == 27（vtimer）：
       → ICC_DIR_EL1 = 27   // 强制 deactivate 物理 PPI 27
       // 原因：物理 27 只有一个，上一个 VM 的 active 状态
       // 会屏蔽下一个 VM 的 vtimer 信号
       
  4. 保存 CNTV_CVAL_EL0 → tsk->virt.cntv_cval
  5. 保存 CNTV_CTL_EL0 → tsk->virt.cntv_ctl
  
  6. 若 CTL.ISTATUS == 1：
     → tsk->virt.vtimer_pending = 1
     // 离开时定时器已到期，恢复时需重新注入
     
  7. CNTV_CTL_EL0 = 0   // 禁用虚拟定时器
     // VM 不在 CPU 上时，其定时器不应产生物理中断
```

**关于 active 状态丢失**：

如果切出 VM 时它的 vtimer 中断正处于 active 状态（guest 正在处理，还没 EOI），我们强制 deactivate 了物理 27。下次这个 VM 被调度回来时，恢复 LR0 中的 active 状态，但物理 27 已经 inactive 了——**HW 状态不一致**。

简化处理：依赖 `vtimer_pending` 标志，进入时重新注入一个 pending 状态的虚拟中断。对于定时器这种"只要时间到了就该处理"的中断，延迟一次、退化为 pending 是完全可接受的。

### 3.5 entering_vm 完整流程

在 `switch_to` 中、切换 `current` 之后调用。此时 next 已经是 current，需要把它的状态加载到硬件。

```
virt_entering_vm(tsk):
  1. CNTVOFF_EL2 = tsk->mm.cntvoff   // 恢复虚拟时间偏移
  
  2. ICH_VMCR_EL2 = tsk->virt.gic_vmcr  // 恢复虚拟 CPU 接口控制
  3. ICH_LR0_EL2  = tsk->virt.gic_lr0   // 恢复虚拟中断 LR
  
  4. CNTV_CVAL_EL0 = tsk->virt.cntv_cval  // 恢复虚拟定时器比较值
  5. CNTV_CTL_EL0  = tsk->virt.cntv_ctl   // 恢复虚拟定时器控制
     // 若离开期间虚拟时间已越过 CVAL，恢复 ENABLE 的瞬间
     // ISTATUS 立即置位 → PPI 27 pending → EL2 正常翻译注入
     
  6. 若 tsk->virt.vtimer_pending:
     → vtimer_pending = 0
     → vgic_inject_virq_hw(27, 27)  // 补注入延迟的 vtimer 中断
```

---

## 4. 调度公平性

### 4.1 原设计的偏置问题

原调度器采用 classic Unix 时间片算法（counter-based），每次 `_schedule` 从索引 0 开始扫描，选 counter 最大的任务。

```c
for (int i = 0; i < NR_TASKS; i++) {
    p = task[i];
    if (p->state == TASK_RUNNING && p->counter > c) {
        c = p->counter;
        next = i;
    }
}
```

**问题**：当多个任务 counter 相同时（刚重算完 counter 的时候），永远是索引最小的任务先被选中。因为 `p->counter > c` 是严格大于，第一个任务先被设为基准，后续相等的不会替换。

对于多 VM 场景，所有 VM 初始 priority 相同，重算 counter 后 counter 值相同，导致**索引小的 VM 总是先跑**，产生系统性公平性偏置。

### 4.2 解决方案：轮转起点

引入 `rr_start`（轮转起点），每次扫描从上次选中任务的下一个开始：

```c
static int rr_start = 0;

for (int i = 0; i < NR_TASKS; i++) {
    int idx = (rr_start + i) % NR_TASKS;
    p = task[idx];
    if (p->state == TASK_RUNNING && p->counter > c) {
        c = p->counter;
        next = idx;
    }
}

rr_start = (next + 1) % NR_TASKS;  // 下次从下一个开始
```

**效果**：counter 相同时，每次是不同的任务先被扫描到，因此轮流胜出。实现了真正的轮转调度（Round-Robin）。

### 4.3 验证结果

两 VM 同优先级（priority 相同），运行 8 秒：

```
[vm1] tick 1    [vm2] tick 1
[vm1] tick 2    [vm2] tick 2
[vm1] tick 3    [vm2] tick 3
...
[vm1] tick 8    [vm2] tick 8
```

心跳严格交替，两 VM 运行时间基本相等，无饥饿现象。

---

## 5. VM 间隔离强化

### 5.1 内存隔离：VMID + Stage 2 TLB 失效

每个 VM 有独立的 VMID（使用 task 的 pid 作为 VMID），Stage 2 TLB 条目按 VMID 标记。理论上不同 VMID 的 TLB 条目不会互相污染。

但有两个问题：
1. 硬件（尤其是 QEMU 模拟器）对 TLB VMID 标记的实现可能不严格
2. VM 销毁后 VMID 可能被复用，旧 TLB 条目必然错误

因此在切换 VM 时保守地失效旧 VMID 的全部 TLB 条目：

```c
void set_cpu_sysregs(struct task_struct *tsk)
{
    static unsigned long last_vmid = ~0UL;

    if ((unsigned long)tsk->pid != last_vmid) {
        asm volatile("tlbi vmalle1" ::: "memory");
        asm volatile("dsb ish" ::: "memory");
        last_vmid = (unsigned long)tsk->pid;
    }

    set_stage2_pgd(tsk->mm.first_table, tsk->pid);
    restore_sysregs(&tsk->cpu_sysregs);
}
```

**关键点**：
- `tlbi vmalle1` 失效的是"当前 VTTBR 的 VMID"对应的所有 EL1 翻译条目
- 此时 VTTBR 还没切换（还是上一个 VM 的），所以失效的正是即将离开的 VM 的 TLB
- 用 `last_vmid` 缓存，同一个 VM 反复进出不重复失效

### 5.2 时间隔离：CNTVOFF_EL2

每个 VM 有独立的 `CNTVOFF_EL2` 值，在 `virt_initialize` 中设置为当前物理计数器值。

```
虚拟 CNTVCT = 物理 CNTPCT - CNTVOFF_EL2
```

每个 VM 看到的虚拟时间起点不同，互相不可见。

### 5.3 中断隔离：vGIC 虚拟接口

每个 VM 有独立的虚拟 CPU 接口状态（ICH_VMCR, ICH_LR0），在世界切换时保存恢复。

VM A 的虚拟中断不会泄漏到 VM B，反之亦然。

---

## 6. split EOI 模式

### 6.1 为什么必须开启 split EOI

GICv3 vtimer 虚拟化采用 **HW 直通模式**：EL2 收到物理 PPI 27 后，注入一条 HW 模式的 LR 条目，然后写 EOIR 降优先级，但**不 deactivate 物理中断**。

物理中断保持 active 状态的作用：**屏蔽电平信号**。虚拟定时器是电平触发的，只要 ISTATUS=1，PPI 27 的输入信号就是高电平。如果物理中断被 deactivate 了，高电平会立即让中断重新 pending → 再次触发 EL2 → 无限循环（中断风暴）。

物理 PPI 27 保持 active 后：
- 电平信号被 GIC 的 active 状态"吸收"了，不会反复 re-pend
- Guest 收到虚拟中断（通过 LR），处理后写虚拟 EOI
- HW 模式下，Guest 写虚拟 EOI 自动 deactivate 物理 27
- 此时 Guest 已经重装了 CVAL（清除了电平信号），所以不会 re-pend

### 6.2 开启方式

```c
icc_ctlr_write(0x1 << 1);  // ICC_CTLR_EL1.EOImode = 1
```

EOImode=1 时：
- 写 ICC_EOIR1_EL1：只做 priority drop（清 APR，降运行优先级），不 deactivate
- 写 ICC_DIR_EL1：单独 deactivate 中断

### 6.3 各中断的处理方式

| 中断 | EOI 方式 | 原因 |
|------|---------|------|
| PPI 26（EL2 物理定时器） | EOIR + DIR（完全结束） | 普通中断，处理完就结束 |
| PPI 27（虚拟定时器） | 只 EOIR（保持 active） | HW 直通模式，由 Guest 虚拟 EOI 联动 deactivate |
| SPI（UART 等） | EOIR + DIR | 普通中断 |

---

## 7. Guest 多实例设计

### 7.1 同镜像多实例

Guest 镜像（`guests/el1_test/`）只有一份，在 `main_virt.c` 中被加载为两个 VM：

```c
create_task("vm1", (uint64_t)guest_bin, guest_bin_size);
create_task("vm2", (uint64_t)guest_bin, guest_bin_size);
```

每个 VM 有独立的：
- Stage 2 页表（同一 IPA 映射到不同物理页）
- 虚拟时间（CNTVOFF 不同）
- 虚拟 GIC 状态（世界切换保存恢复）
- VMID

### 7.2 差异化配置

Guest 通过 `HVC_GET_VMID` 查询自己的 VM 编号，然后配置不同的定时器频率：

```c
vmid = guest_vmid();
tick_hz = (vmid == 1) ? 10 : (vmid == 2) ? 5 : 1;
```

- VM1: 10Hz 虚拟定时器
- VM2: 5Hz 虚拟定时器

这样可以直观地观察到两个 VM 的定时器是完全独立的。

### 7.3 HVC_GET_VMID

新增 Hypercall：

| 调用号 | 名称 | 参数 | 返回值 |
|--------|------|------|--------|
| 3 | HVC_GET_VMID | 无 | 当前 VM 的编号（task pid） |

---

## 8. 调试实录：三大深层问题

### 问题 1：Spurious 中断风暴（IAR=1023）

**现象**：启用 leaving_vm / entering_vm 后，系统产生 1000+ 次/秒的 spurious 中断，PPI 26 几乎不触发，PPI 27 计数为 0。

**定位过程**：二分法逐步增加功能，最终定位到"CNTV 禁用/恢复"操作。

**根因**：leaving_vm 在 handle_irq 之前执行，写 `CNTV_CTL=0` 禁用定时器时，PPI 27 还没被 ack（仍在 pending 状态）。电平信号变低后，GIC 清除了 pending 状态。等 handle_irq 读 IAR 时，已经没有 pending 中断了，返回 1023（spurious）。

然后 entering_vm 中又恢复 CNTV → PPI 27 重新 pending → eret 回 EL1 → 又触发 → 循环往复。

**解决**：将世界切换从 `vm_leaving_work` / `vm_entering_work` 移到 `switch_to` 中（handle_irq 之后执行），同一个 VM 进出 EL2 时不碰虚拟化状态。

### 问题 2：PPI 27 中断风暴（5000+ 次/秒）

**现象**：split EOI 开启后，PPI 27 仍以 ~5000Hz 的频率触发，远高于预期的 ~15Hz。

**定位过程**：读 GICR_ISACTIVER0 确认写 EOIR 后 PPI 27 仍是 active 的（split EOI 正常工作）。但 PPI 27 计数仍在快速增长。

最终发现：entering_vm 每次返回 EL1 都被调用（因为在 vm_entering_work 中），每次都做 ICC_DIR deactivate 物理 27 + 禁用 CNTV + 恢复 CNTV。同一个 VM 反复进出 EL2 时，每次都把物理 27 deactivate 一次，导致每次都 re-pend。

**根因**：世界切换的调用时机错误——每次进出 EL2 都做，而不是只在切换 VM 时做。

**解决**：同问题 1，将世界切换移入 switch_to。

### 问题 3：EOImode 配置未生效（之前以为的问题）

**现象**：最初怀疑 EOImode 没设置对，导致 EOIR 同时 deactivate。

**验证**：写 ICC_CTLR_EL1 后读回，确认值为 0x8f02（EOImode 位为 1）。且写 EOIR 后读 ISACTIVER0，PPI 27 仍为 active。证明 split EOI 正常工作。

**结论**：split EOI 配置正确，中断风暴另有原因（见问题 2）。

---

## 9. 构建与运行

### 9.1 编译

```bash
cd avisor
make -f Makefile.virt
```

输出：`bin_virt/kernel8.img`

### 9.2 运行

```bash
qemu-system-aarch64 \
  -M virt,virtualization=on,gic-version=3 \
  -cpu max -smp 1 -m 1G \
  -kernel bin_virt/kernel8.img \
  -nographic -serial mon:stdio
```

退出 QEMU：`Ctrl+A` 然后按 `X`

### 9.3 预期输出

```
[vm1] guest: hello from EL1!
[vm1] guest: I am VM 1, running on aVisor hypervisor
[vm1] guest: virtual GIC CPU interface initialized
[vm1] guest: virtual timer started
[vm1] guest: interrupts enabled, entering main loop
[vm2] guest: hello from EL1!
[vm2] guest: I am VM 2, running on aVisor hypervisor
[vm2] guest: virtual GIC CPU interface initialized
[vm2] guest: virtual timer started
[vm2] guest: interrupts enabled, entering main loop
[tick 100] Hypervisor is alive at EL2
[vm1] tick 1
[vm2] tick 1
[vm1] tick 2
[tick 200] Hypervisor is alive at EL2
[vm2] tick 2
...
```

**关键观察点**：
1. 两个 VM 都正常启动，各有独立的 VM 编号
2. "tick N" 每秒各打印一次，交替出现（调度公平）
3. Hypervisor 的 100Hz tick 不受影响

---

## 10. 后续 Phase 规划

### Phase 4：设备虚拟化
- virtio 设备模型（virtio-net / virtio-block）
- MMIO 陷获与模拟
- 中断注入（虚拟 SPI）
- VM 间通信机制

### Phase 5：AGX Orin 硬件移植
- 移植到 NVIDIA AGX Orin 开发板
- GICv3 硬件验证
- 定时器/中断验证
- 性能基准测试

---

## 附录：Phase 3 关键寄存器速查

### 虚拟 CPU 接口（ICH_*）

| 寄存器 | 作用 | 保存/恢复 |
|--------|------|----------|
| ICH_VMCR_EL2 | 虚拟 CPU 接口控制（VPMR, VENG1 等） | 是 |
| ICH_LR0_EL2 | List Register 0（虚拟中断条目） | 是 |
| ICH_HCR_EL2 | 虚拟 CPU 接口控制（En 等） | 否（全局配置，所有 VM 共用） |

### 虚拟定时器

| 寄存器 | 作用 | 保存/恢复 |
|--------|------|----------|
| CNTV_CTL_EL0 | 虚拟定时器控制 | 是 |
| CNTV_CVAL_EL0 | 虚拟定时器比较值 | 是 |
| CNTVOFF_EL2 | 虚拟时间偏移 | 是 |
| CNTVCT_EL0 | 虚拟计数器（只读，由物理+偏移计算） | 否 |

### GIC CPU 接口（ICC_*）

| 寄存器 | 作用 | 备注 |
|--------|------|------|
| ICC_CTLR_EL1 | CPU 接口控制（EOImode 等） | 全局，split EOI 必须开 |
| ICC_IAR1_EL1 | 中断应答 | 每次中断读一次 |
| ICC_EOIR1_EL1 | 中断结束（降优先级） | 每次中断写一次 |
| ICC_DIR_EL1 | 中断 deactivate | split EOI 模式下手动写 |
| ICC_PMR_EL1 | 优先级掩码 | 全局 |
| ICC_IGRPEN1_EL1 | Group 1 中断使能 | 全局 |

### 关键常量

| 常量 | 值 | 说明 |
|------|-----|------|
| PPI_EL2_PHYS_TIMER | 26 | EL2 物理定时器 PPI |
| PPI_VIRT_TIMER | 27 | 虚拟定时器 PPI |
| ICH_LR_HW | 1UL << 61 | LR 的 HW 位 |
| ICH_LR_STATE_MASK | 3UL << 62 | LR 的状态位（63:62） |
| ICH_LR_PENDING | 1UL << 62 | pending 状态 |
| CNTV_CTL_ISTATUS | 1UL << 2 | 定时器中断状态位 |
| GIC_SPURIOUS_IRQ | 1023 | spurious 中断号 |
