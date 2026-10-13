# Phase 13：跨核 vCPU 调度与 IPI 注入 详细设计文档

## 1. 概述

### 1.1 背景

Phase 12 已经实现了 vSGI（虚拟 SGI）的基础能力，但验证场景限定在**同一个物理核**上的多 vCPU 之间：目标 vCPU 未运行时，hypervisor 直接把虚拟中断写进目标 vCPU 的 saved state（`gic_lr[]`），下次调度到该 vCPU 时由 `vgic_restore_state` 加载到硬件 LR，从而送达。

然而真实的多核系统中，一个 VM 的多个 vCPU 通常会被调度到**不同的物理核**上并行运行。此时"写 saved state"不再可靠：

- 目标 vCPU 正运行在**另一个物理核**上时，该核的硬件 LR 才是权威状态；
- 目标核的硬件 LR 是本核 per-CPU 资源，发送方核无法直接访问；
- saved state 会与目标核的世界切换（`vgic_save_state` / `vgic_restore_state`）发生竞态。

Phase 13 的目标是让 aVisor 支持**跨物理核的 vCPU 调度与跨核虚拟中断注入**：把 VM1 的次 vCPU 放到 CPU1 上运行，并通过物理 SGI（IPI）触发目标核在中断上下文中完成 vGIC 维护，最终让跨核 vSGI 稳定工作。

### 1.2 目标

- ✅ 修复 CPU1 的 PSCI 启动问题
- ✅ 把 VM1 次 vCPU（vcpu1）调度到 CPU1 上运行（跨核 vCPU）
- ✅ 实现跨核 vGIC 注入：物理 SGI 触发目标核 vGIC 维护
- ✅ 修复跨核注入的竞态（mailbox + IPI 原子注入）
- ✅ 跨核 vSGI 测试（单次 + 多次握手）稳定通过
- ✅ 全量回归测试通过

### 1.3 平台与基础

- **硬件平台**：QEMU `virt` 机器，ARMv8-A，GICv3 + ITS + vGIC，2 物理核
- **Hypervisor**：aVisor（Type-1，EL2）
- **Guest**：EL1 测试程序（裸金属）
- **前序版本**：`v0.12.0-phase12-vsgi`

---

## 2. 整体架构设计

### 2.1 跨核 vCPU 布局

Phase 13 之前，所有 vCPU 都运行在 CPU0 上：

```
Phase 12（同核）
物理 CPU0
├── idle
├── VM1 vcpu0
├── VM1 vcpu1   ← 与 vcpu0 分时复用 CPU0
└── VM2 vcpu0
物理 CPU1
└── idle（未真正使用）
```

Phase 13 之后，VM1 的次 vCPU 迁移到 CPU1，实现真正的跨核并行：

```
Phase 13（跨核）
物理 CPU0                    物理 CPU1
├── idle                    ├── idle
├── VM1 vcpu0               ├── VM1 vcpu1
└── VM2 vcpu0
```

### 2.2 跨核 vSGI 注入模型

```
Guest VM1 (vmid=1)
┌──────────────────────────────────────────────────────────┐
│  vcpu0 (CPU0)                vcpu1 (CPU1)               │
│  ┌───────────┐               ┌───────────┐              │
│  │  Guest OS │               │  Guest OS │              │
│  └─────┬─────┘               └─────▲─────┘              │
│        │ hvc #0 (VSGI_SEND)        │ vIRQ 送达           │
└────────┼───────────────────────────┼────────────────────┘
         │ EL2 (CPU0)                │ EL2 (CPU1)
         ▼                           │
   vgic_inject_virq_to_vm            │
   ├─ 置 mailbox 标志 ────────────┐  │
   └─ 发物理 SGI#2 (IPI) ────────▶│──► vgic_ipi_update_handle()
                                  │      ├─ 目标运行 → 写硬件 LR
                                  │      └─ 目标未运行 → 写 saved state
```

### 2.3 关键组件

1. **CPU1 启动修复**：`boot_virt.S` 中次核等待信号改用 32-bit 读
2. **跨核任务放置**：`main_virt.c` 把 VM1 vcpu1 创建到 CPU1
3. **跨核注入邮箱**：`vm_virt_state` 新增 `vgic_ipi_pending` / `vgic_ipi_vintid`
4. **物理 SGI 触发**：复用 `smp_send_ipi()` + `SGI_VGIC_UPDATE`
5. **IPI 处理函数**：`vgic_ipi_update_handle()` 在目标核中断上下文完成注入

---

## 3. CPU1 PSCI 启动问题修复

### 3.1 问题现象

`psci_cpu_on()` 返回 0（成功），但 CPU1 无任何输出，`smp_online[1]` 始终为 0，次核卡在启动等待循环。

### 3.2 根因分析

次核启动后进入 `boot_virt.S` 的 `secondary_el2`，在开启 MMU 前进入忙等循环：

```asm
secondary_wait:
    ldr x0, =smp_boot_flag
    ldr w0, [x0]        /* 关键：smp_boot_flag 是 int (32-bit) */
    cmp w0, w19         /* 与保存的 cpu_id (x19) 比较 */
    b.ne secondary_wait
```

而 C 代码中的定义是：

```c
volatile int smp_boot_flag = 0;   /* 32-bit 有符号整数 */
```

**根因**：原实现使用 64-bit 加载（`ldr x0, [x0]`），会把 `smp_boot_flag` 相邻的 4 字节也读进高 32 位，导致与 32-bit 的 `cpu_id` 比较永远不相等，次核永远卡在 `secondary_wait`。

**修复**：改用 `ldr w0, [x0]`（32-bit 加载）+ `cmp w0, w19`（32-bit 比较），使次核能正确识别启动信号。

### 3.3 次核启动时序

```
CPU0                                     CPU1
│ psci_cpu_on(0x1, entry, 0)            │
│   └─ 返回 0                            │
│ smp_boot_flag = 1 ────────────────►    │ secondary_wait 检测到 flag==1
│ sev                                    │ 开启 MMU
│                                        │ bl hypervisor_secondary_main
│                                        │ gic_init_secondary()
│                                        │ smp_online[1] = 1
│ 等待 smp_online[1] == 1 ◄──────────────┘
```

---

## 4. 跨核 vCPU 调度

### 4.1 任务放置

`main_virt.c` 中，VM1 的次 vCPU 通过 `create_task_on_cpu(1, ...)` 创建在 CPU1 上：

```c
/* VM1 vcpu0 → CPU0 */
/* VM1 vcpu1 → CPU1 (create_task_on_cpu(1, shared_vm_loader, ...)) */
/* VM2 vcpu0 → CPU0 */
```

### 4.2 调度器约束

任务**不跨核迁移**：每个物理核有独立的 `cpu_state[cpu]`（current / task 表 / nr_tasks / rr_start）。VM 的 vCPU 绑定到固定的 `phys_cpu`，调度只在同一物理核内的任务间轮转。

- `current` 宏 = `cpu_state[smp_processor_id()].curr`
- `switch_to()` 内先 `leaving_vm(prev)` 保存旧 vCPU 的 vGIC 状态，再 `entering_vm(next)` 恢复新 vCPU 状态

### 4.3 启动打印修正

原启动打印 "CPU0: idle + VM1(vcpu0+vcpu1) + VM2_vcpu0 / CPU1: idle + VM2_vcpu1" 已过时，修正为实际布局：

```
CPU0: idle + VM1 vcpu0 + VM2 vcpu0
CPU1: idle + VM1 vcpu1
```

---

## 5. 跨核 vGIC 注入机制

### 5.1 数据结构扩展

`vm_virt_state` 新增跨核注入邮箱字段：

```c
struct vm_virt_state {
    ...
    int vgic_ipi_pending;           /* 有待注入的虚拟中断 */
    unsigned int vgic_ipi_vintid;   /* 待注入的虚拟中断号 */
};
```

### 5.2 注入入口：`vgic_inject_virq_to_vm`

按目标 vCPU 所处位置分三种情况处理：

```c
int vgic_inject_virq_to_vm(struct task_struct *vm, uint32_t vintid)
{
    if (!vm || vm->state == TASK_ZOMBIE) return -1;

    /* 情况 1：目标就是当前运行的 vCPU → 直接写本核硬件 LR */
    if (vm == current && vm->pid != 0)
        return vgic_inject_virq(vintid);

    /* 情况 3：跨核 → mailbox + 物理 SGI 交给目标核处理 */
    if (vm->phys_cpu != smp_processor_id()) {
        vm->virt.vgic_ipi_vintid = vintid;
        vm->virt.vgic_ipi_pending = 1;
        asm volatile("dmb ish" ::: "memory");
        smp_send_ipi(vm->phys_cpu, SGI_VGIC_UPDATE);
        return 0;
    }

    /* 情况 2：同核但目标未运行 → 写 saved state */
    return vgic_inject_saved(&vm->virt, vintid);
}
```

### 5.3 IPI 处理函数：`vgic_ipi_update_handle`

目标核收到 `SGI_VGIC_UPDATE` 后，在 IRQ 上下文（IRQ 屏蔽）中遍历本核任务表，找到带 pending 标志的目标 vCPU 并完成注入：

```c
void vgic_ipi_update_handle(void)
{
    int cpu = smp_processor_id();

    for (int i = 0; i < cpu_state[cpu].nr_tasks; i++) {
        struct task_struct *vm = cpu_state[cpu].task[i];
        if (!vm || vm->pid == 0) continue;
        if (!vm->virt.vgic_ipi_pending) continue;

        uint32_t vintid = vm->virt.vgic_ipi_vintid;
        vm->virt.vgic_ipi_pending = 0;

        if (cpu_state[cpu].curr == vm) {
            vgic_inject_virq(vintid);              /* 运行中 → 硬件 LR */
        } else {
            vgic_inject_saved(&vm->virt, vintid);  /* 未运行 → saved state */
        }
    }
}
```

---

## 6. 跨核注入竞态分析与修复

### 6.1 最初的（有缺陷的）实现

最初实现通过读 `cpu_state[vm->phys_cpu].curr` 判断目标是否在运行：

```c
if (vm->phys_cpu != smp_processor_id() &&
    cpu_state[vm->phys_cpu].curr == vm) {
    /* 只置 mailbox + 发 IPI */
}
/* 否则写 saved state */
```

该判断存在**双重竞态**：

1. **读 curr 本身是竞态**：`cpu_state[1].curr` 在次核 `hvc_call(HVC_SCHED_YIELD)` → `_schedule` → `switch_to` 的过渡中不断翻转（vcpu1 ↔ idle），发送方读到的值可能是瞬时的中间态。
2. **IPI 送达时机竞态**：即便读到 `curr == vcpu1`，在置标志与 IPI 真正送达之间，vcpu1 可能已 `yield` 切出到 idle。此时 IPI 处理器里 `current` 是 idle（`pid == 0`），直接 `return` 丢弃了注入——SGI 丢失，且 `vgic_ipi_pending` 未清除。

这导致 vSGI "多次发送" 测试出现 `round 0 timeout, count=0`（Test 1 单次能过，Test 2 第一个 SGI 就丢）。

### 6.2 修复思路

**核心原则**：发送方核**不直接触碰**目标核的 saved state 或硬件 LR，只做两件事——写 mailbox 标志、发物理 SGI。实际的注入动作由**目标核**在 IPI 中断上下文完成，因为：

- IPI 处理器运行在目标核，IRQ 屏蔽（PSTATE.I=1）；
- 目标核的世界切换（`switch_to` 中的 `leaving_vm`/`entering_vm`）同样在 EL2 且 IRQ 屏蔽下执行；
- 二者互斥，`current` 在 IPI 处理器内是稳定的，注入动作是原子的。

因此：

- **运行中**（`curr == vm`）：直接写硬件 LR，立即送达；
- **未运行**（`curr != vm`，如 idle）：写 saved state，下次调度恢复时送达。

两种情况下都不会丢中断，也不与目标核的世界切换竞态。

### 6.3 硬件 LR 去重

`vgic_inject_virq()` 中补充了对硬件 LR 的去重：若 LR1 已经 pending 同一个 vINTID，则直接返回 0（边沿触发中断可合并），避免重复写入。

`saved state` 侧的去重由 `vgic_inject_saved()` 完成（固定 LR1 槽位，同一 vINTID 已 pending 时合并）。

---

## 7. Guest 侧改动

### 7.1 跨核启动同步（boot_done）

次核在开启自己的 MMU 前，必须等主核完成 BSS 清零 + 页表建立。否则次核的 `mmu_init()` 写 `guest_pg_dir`（BSS）会与主核的 BSS 清零/页表建立发生竞态。

新增共享标志 `SMP_BOOT_DONE_OFF`（`0x38`）：

- 主核在 `mmu_init()` 后置 `boot_done = 1`
- 次核在 `mmu_init()` 前忙等 `boot_done == 1`

```c
/* 主核 */
mmu_init();
*boot_done = 1;  asm volatile("dmb ish");

/* 次核 */
while (*boot_done != 1) hvc_call(HVC_SCHED_YIELD, 0);
asm volatile("dmb ish");
mmu_init();
```

### 7.2 vSGI 多次发送改为握手模式

SGI 是**边沿触发**的：若连续快发多次同一 INTID，硬件/vGIC 会把多次边沿合并成一次 pending，导致 "发 5 收 5" 语义不可靠。

改为**逐次握手**：每发一个 SGI，等次核收到（count 递增）后再发下一个：

```c
for (int i = 0; i < 5; i++) {
    ret = vsgi_send(1, VSGI_TEST_SGI_ID);
    while (*vsgi_count < (u64)(i + 1))
        hvc_call(HVC_SCHED_YIELD, 0);
}
```

### 7.3 中断处理加内存屏障

次核的 SGI 中断处理中，计数自增后加 `dmb ish`，确保共享内存的更新对主核可见：

```c
*vsgi_received |= (1ULL << intid);
(*vsgi_count)++;
asm volatile("dmb ish" ::: "memory");
```

---

## 8. 测试结果

### 8.1 测试环境

- **命令**：`make -f Makefile.virt run`（`timeout 40`，退出码 124 = VM 稳定运行至超时）
- **配置**：VM1 双 vCPU（vcpu0→CPU0，vcpu1→CPU1），VM2 单 vCPU（CPU0）

### 8.2 测试矩阵

| 测试项 | VM1 | VM2 | 结果 |
|--------|-----|-----|------|
| virtio-net loopback | ✅ | ✅ | 通过 |
| ivshmem ping-pong | ✅ | ✅ | 通过 |
| edu MSI interrupt | ✅ | - | 通过 |
| vITS vLPI 注入 | ✅ | - | 通过 |
| Phase 10 综合（sched/mem/shm） | ✅ | ✅ | 通过 |
| Phase 11 vSMP（counter=20000） | ✅ | - | 通过 |
| Phase 12 vSGI（单次） | ✅ | - | 通过 |
| **Phase 13 跨核 vSGI（多次握手）** | ✅ | - | 通过 |

### 8.3 关键输出

```
[vm1] smp: counter = 20000 (expected 20000)
[vm1] smp: PASSED (spinlock + shared counter correct)
[vm1] vsgi: PASSED (master->secondary vSGI correct)
[vm1] vsgi: secondary received count = 5 (5 round-trips)
[vm1] vsgi: PASSED (multiple vSGI correct)
```

> 注：`[vm2] smp: FAILED (secondary vCPU timeout)` 为**预期**——VM2 只有 1 个 vCPU，无次核可调度。

---

## 9. 设计要点与思考

### 9.1 为什么跨核注入不能由发送方直接写状态？

跨核注入的本质困难在于：目标 vCPU 的 "pending 中断" 状态分散在两处——目标核的硬件 LR（运行中时权威）与 `vm->virt.gic_lr[]` saved state（未运行时权威）。发送方核无法原子地判定 "运行/未运行"，更无法原子地访问目标核的硬件 LR。

因此把注入动作**下放到目标核的中断上下文**（mailbox + IPI）是最简洁可靠的方案：IPI 处理器与目标核世界切换天然互斥，`current` 稳定，注入原子。

### 9.2 邮箱的简化假设

当前邮箱是单槽（`vgic_ipi_vintid` + `vgic_ipi_pending`），因为物理 SGI（边沿触发）在多次触发未处理时会合并成一次 pending，与单槽邮箱语义一致。跨核 vSGI 采用逐次握手，天然保证同一时刻只有一个在途注入。

若未来需要批量/多中断并发跨核注入，可将邮箱升级为队列，并用多槽或多次 IPI 通知。

### 9.3 与真实 KVM 的对比

| 特性 | aVisor (Phase 13) | KVM/ARM |
|------|------------------|---------|
| 跨核 vCPU 调度 | vCPU 静态绑定物理核 | 动态迁移，负载均衡 |
| 跨核 vSGI 注入 | HVC + mailbox + 物理 SGI | SGI 注入 + `kvm_vcpu_kick`（IPI） |
| 目标未运行处理 | 写 saved state，恢复时生效 | 写 vGIC pending 表，`vcpu->arch.irq_lines` |
| 注入原子性 | IPI 中断上下文互斥 | 关闭中断 + 自旋锁保护 vGIC |

---

## 10. 文件改动清单

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `hypervisor/boards/virt/boot_virt.S` | 修复 | 次核等待信号改 32-bit 读（`ldr w0` + `cmp w0`） |
| `hypervisor/boards/virt/main_virt.c` | 修改 | VM1 vcpu1 放到 CPU1；修正启动打印 |
| `include/common/sched.h` | 新增 | `vm_virt_state` 增跨核邮箱字段 |
| `hypervisor/boards/virt/vgic.c` | 重构 | 跨核注入改 mailbox+IPI；新增 `vgic_inject_saved`；重写 `vgic_ipi_update_handle`；硬件 LR 去重 |
| `guests/el1_test/guest.h` | 新增 | `SMP_BOOT_DONE_OFF` 共享内存布局 |
| `guests/el1_test/main.c` | 修改 | boot_done 启动同步；vSGI 多次发送改握手；IRQ 处理加屏障 |

---

## 11. 版本信息

- **版本 tag**：`v0.13.0-phase13-crosscore`
- **前序版本**：`v0.12.0-phase12-vsgi`
- **完成日期**：2026-10
