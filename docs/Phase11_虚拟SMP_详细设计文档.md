# Phase 11：虚拟 SMP（多 vCPU）详细设计文档

## 1. 概述

### 1.1 背景

在实现了 vGIC 和 vITS 虚拟中断控制器之后，aVisor 已经具备了为单个 VM 提供虚拟中断的能力。然而，每个 VM 只有一个虚拟 CPU（vCPU），guest 操作系统无法利用多核并行计算的能力。

Phase 11 的目标是实现**虚拟 SMP（Symmetric Multi-Processing）**，让单个 VM 拥有多个 vCPU，guest 可以并行运行多个线程，充分利用物理多核资源。

### 1.2 目标

- ✅ 支持单个 VM 创建多个 vCPU
- ✅ 同物理核上的多 vCPU 分时复用调度验证
- ✅ vCPU 间共享页表与设备模型，各自拥有独立的系统寄存器与 vGIC 状态
- ✅ Guest 侧 SMP 测试（共享内存 + Ticket 自旋锁）正确性验证
- ✅ 修复 ivshmem 跨 VM 共享内存回归问题
- ⏳ 跨物理核调度（vSGI / IPI 虚拟化留待 Phase 12）

### 1.3 平台与基础

- **硬件平台**：QEMU `virt` 机器，ARMv8-A，GICv3 + ITS + vGIC
- **Hypervisor**：aVisor（Type-1，EL2）
- **Guest**：EL1 测试程序（裸金属）
- **前序版本**：`v0.10.0-phase10-advanced`

---

## 2. 整体架构设计

### 2.1 vCPU = task_struct

在 aVisor 中，每个调度实体都是一个 `task_struct`。vCPU 也不例外——**每个 vCPU 就是一个 task**。

```
物理 CPU0                    物理 CPU1
┌──────────────────────┐   ┌──────────────────┐
│ idle (HV)            │   │ idle (HV)        │
│ vm1-cpu0 (vmid=1)    │   │ (当前未使用)     │
│ vm1-cpu1 (vmid=1)    │   └──────────────────┘
│ vm2-cpu0 (vmid=2)    │
└──────────────────────┘
     per-CPU 调度器
```

**同 VM 的 vCPU 共享**：
- 页表（Stage 2 translation）
- 设备模型（virtio-net、ivshmem、vITS 等）
- vmid（VM 标识符）

**每个 vCPU 独立拥有**：
- 系统寄存器上下文（`cpu_sysregs`）
- vGIC LR（List Register）与 VMCR
- vtimer（虚拟定时器）
- 栈、寄存器上下文（`pt_regs`）
- 调度优先级、时间片

### 2.2 关键数据结构

```c
struct task_struct {
    /* 通用字段 */
    uint64_t pid;
    char name[36];
    uint32_t priority;
    uint32_t counter;    /* 时间片 */
    int      state;

    /* VM 标识 */
    uint64_t vmid;       /* 同一 VM 的 vCPU 共享同一个 vmid */
    int      vcpu_id;    /* 虚拟 CPU ID（0, 1, 2, ...）*/
    int      phys_cpu;   /* 物理 CPU ID */

    /* 虚拟化状态（per-vCPU）*/
    struct virt_state   virt;          /* vGIC LRs + vtimer */
    struct cpu_sys_regs cpu_sysregs;   /* 系统寄存器 */

    /* 设备模型（VM 级别，同 VM 的 vCPU 共享）*/
    const struct board_ops *board_ops;
    /* ... */
};
```

### 2.3 vGIC per-vCPU 状态

vGIC（虚拟 GIC）的状态是 per-vCPU 的：

- **LR[0..NR_VGIC_LRS-1]**：List Register，每个 vCPU 有自己的一组 LR
  - LR0：vtimer（HW 映射）
  - LR1/LR2：软件注入的虚拟中断
- **VMCR**：虚拟 GIC 控制寄存器，保存 vCPU 被调度出去时的状态

当调度器切换 vCPU 时，对应的 vGIC 状态也会切换。

---

## 3. vCPU 创建流程

### 3.1 主 vCPU 创建

主 vCPU 的创建与之前单核 VM 的创建完全相同：

```
create_task_on_cpu(cpu, embedded_binary_loader, &args)
    └→ embedded_binary_loader
        ├→ 创建 Stage 2 页表
        ├→ 加载 Guest 镜像
        └→ board_init（初始化设备模型）
```

### 3.2 次 vCPU 创建

次 vCPU 不重新加载镜像和建页表，而是**共享主 vCPU 的资源**：

```
create_task_on_cpu(cpu, shared_vm_loader, &shared_args)
    └→ shared_vm_loader
        ├→ 等待主 vCPU MM 准备完成
        ├→ 挂接主 vCPU 的页表（mm.pgd）
        ├→ 初始化 per-vCPU 虚拟化状态
        └→ virt_attach_secondary（挂接共享设备模型）
```

关键代码（`guest_loader.c`）：

```c
struct shared_vm_loader_args {
    struct task_struct *master;
    uint64_t entry_point;
};

void shared_vm_loader(struct shared_vm_loader_args *args) {
    /* 等待主 vCPU 的 MM 初始化完成 */
    while (!master->mm_ready) cpu_relax();

    /* 共享页表 */
    current->mm.pgd = master->mm.pgd;

    /* 初始化 per-vCPU 虚拟化状态 */
    arch_init_virt(current);

    /* 挂接共享的设备模型 */
    virt_attach_secondary(current, master);

    /* 进入 Guest */
    enter_vm(current, args->entry_point);
}
```

### 3.3 创建后的修正

`create_task_on_cpu` 内部会初始化一些默认值（如 `vcpu_id = 0`、自动分配 `vmid`），次 vCPU 创建后需要手动修正：

```c
/* VM1 次 vCPU */
{
    struct shared_vm_loader_args shared_args = {
        .master = vm1_master,
        .entry_point = 0x40000000,
    };
    int pid = create_task_on_cpu(0, shared_vm_loader, &shared_args);

    cpu0_tasks[pid]->vmid = 1;                         /* 共享 VM1 的 vmid */
    cpu0_tasks[pid]->vcpu_id = 1;                      /* 次 vCPU ID = 1 */
    cpu0_tasks[pid]->cpu_sysregs.mpidr_el1 = 1;        /* 同步 MPIDR */
    cpu0_tasks[pid]->phys_cpu = 0;                     /* 物理核 0 */
}
```

> **重要**：`vcpu_id` 修改后必须同步修改 `cpu_sysregs.mpidr_el1`，否则 Guest 读 MPIDR_EL1 时拿到的值与实际 vcpu_id 不一致。

---

## 4. 调度器设计

### 4.1 per-CPU 调度

aVisor 采用 **per-CPU 调度器**，每个物理核有自己的 runqueue：

- vCPU 被创建时绑定到一个物理核（`phys_cpu`）
- vCPU **不跨核迁移**
- 同物理核上的多个 vCPU 按优先级 + 时间片轮转

### 4.2 同核多 vCPU 验证

Phase 11 中，VM1 的两个 vCPU 都创建在物理 CPU0 上，验证**同核分时复用**：

```
物理 CPU0 runqueue:
  [ idle, vm1-cpu0, vm1-cpu1, vm2-cpu0 ]
```

调度行为：
1. 初始：vm1-cpu0 运行，vm1-cpu1 等待
2. vm1-cpu0 时间片用完 → 调度 vm1-cpu1
3. vm1-cpu1 执行 Guest 代码（次核入口）
4. 两个 vCPU 交替运行，模拟 SMP 行为

### 4.3 sched_yield 支持

Guest 可以通过 HVC 调用主动让出 CPU：

```c
/* Guest 侧 */
hvc_call(HVC_SCHED_YIELD, 0);

/* Hypervisor 侧 */
void sched_yield(void) {
    current->counter = 0;  /* 清零时间片 */
    schedule();            /* 触发调度 */
}
```

在 SMP 测试中，次核等待主核时主动调用 `sched_yield`，避免忙等占满 CPU 时间。

---

## 5. vGIC 虚拟化（per-vCPU）

### 5.1 vGIC 状态切换

调度器在 `context_switch` 时切换 vGIC 状态：

```c
void context_switch(struct task_struct *next) {
    /* 保存当前 vCPU 的 vGIC 状态 */
    vgic_save_state(current);

    /* 切换到 next */
    current = next;

    /* 恢复 next 的 vGIC 状态 */
    vgic_restore_state(current);
}
```

实际上，vGIC 的保存/恢复在 `enter_vm` / `leave_vm` 中完成，而 `leave_vm` 在 Guest 退出到 EL2 时自动触发。

### 5.2 虚拟中断注入

向某个 vCPU 注入虚拟中断时，需要找到目标 vCPU 的 vGIC 状态：

```c
int vgic_inject_virq_to_vm(struct task_struct *target, int vintid) {
    /* 如果目标正在运行，直接写硬件 LR */
    if (target->state == TASK_RUNNING &&
        target == per_cpu(current_task, target->phys_cpu)) {
        return vgic_lr_set_hw(vintid);
    }
    /* 否则，修改保存的 LR 状态 */
    return vgic_set_pending(target, vintid);
}
```

---

## 6. ivshmem 设备模型的多 vCPU 适配

### 6.1 问题分析

ivshmem 设备模型使用 `ivshmem_peers[]` 全局数组维护 peer 与 VM 的映射：

```c
struct ivshmem_dev *ivshmem_peers[IVSHMEM_MAX_PEERS];
```

peer_id 由 `vmid - 1` 计算。但多 vCPU 场景下，`create_task_on_cpu` 时 vmid 是临时分配的（`next_vmid` 递增），调用方随后才修改为正确的 vmid。

这导致：
1. 次 vCPU 创建时，board 初始化用临时 vmid 计算 peer_id，可能越界或覆盖
2. `virt_attach_secondary` 中，次 vCPU 的 board 设备会再次初始化，覆盖 `ivshmem_peers`

### 6.2 解决方案

**方案一**：新增 `ivshmem_set_peer_id()` 函数，允许在 vmid 修正后重新设置 peer 映射。

```c
void ivshmem_set_peer_id(struct ivshmem_dev *dev, int peer_id) {
    /* 如果旧的 peer 位置上是自己，清空 */
    if (dev->peer_id >= 0 && dev->peer_id < IVSHMEM_MAX_PEERS &&
        ivshmem_peers[dev->peer_id] == dev) {
        ivshmem_peers[dev->peer_id] = NULL;
    }
    dev->peer_id = peer_id;
    if (peer_id >= 0 && peer_id < IVSHMEM_MAX_PEERS) {
        ivshmem_peers[peer_id] = dev;
    }
}
```

**方案二**：`virt_attach_secondary` 中恢复 `ivshmem_peers`。

次 vCPU 的 board 初始化会覆盖 `ivshmem_peers` 的某些位置。`virt_attach_secondary` 执行时，重新将主 vCPU 的 ivshmem_dev 放回正确的 peer 位置。

**方案三**：在 `main_virt.c` 中，所有 vCPU 创建完成后，手动调用一次修正函数，确保 peer 映射正确。

最终采用了**组合方案**：既有 `ivshmem_set_peer_id` 灵活修正，又在最后统一修正一次，确保正确性。

---

## 7. Guest 侧 SMP 实现

### 7.1 MPIDR 虚拟化

Guest 通过读 MPIDR_EL1 知道自己是哪个 CPU。aVisor 通过 **TID1 trap** 捕获 MPIDR_EL1 的读取，返回虚拟 MPIDR 值：

```
HCR_EL2.TID1 = 1  →  读取 MPIDR_EL1 触发异常
                    →  注入返回值为 task_struct.cpu_sysregs.mpidr_el1
```

### 7.2 Guest 启动流程

```asm
/* start.S */
mrs x0, mpidr_el1
and x0, x0, #0xFF      /* aff0 */
cbz x0, master         /* aff0 == 0 → 主核 */
b   secondary          /* aff0 != 0 → 次核 */

master:
    bl guest_main

secondary:
    bl guest_secondary_main
```

> **注意**：由于某些原因（可能是编译器优化或异常返回时的寄存器状态差异），start.S 中读到的 MPIDR 和 C 代码中读到的 MPIDR 可能不一致。
> 为了稳健，Guest C 代码中使用**主核竞选机制**（见 7.3 节）。

### 7.3 主核竞选机制

```c
#define SMP_TEST_ADDR   0x40300000

int try_become_master(void) {
    volatile uint64_t *flag = (void *)SMP_TEST_ADDR;
    /* 原子地尝试将 flag 从 0 改为 1 */
    uint64_t old = 0;
    uint64_t new = 1;
    int success;

    __asm__ volatile(
        "mov %0, #0\n"
        "ldxr x1, [%2]\n"
        "cmp x1, #0\n"
        "b.ne 1f\n"
        "stxr w1, %3, [%2]\n"
        "cbnz w1, 1b\n"
        "mov %0, #1\n"
    "1:\n"
        : "=&r"(success)
        : "r"(flag), "r"(new)
        : "x1", "memory"
    );

    return success;
}
```

第一个进入 `guest_main` 的 vCPU 竞选成功（成为 master），第二个失败（转入 secondary 逻辑）。

### 7.4 Ticket 自旋锁

Guest 实现了 Ticket 自旋锁（FIFO 公平锁），用于多 vCPU 间的互斥：

```c
typedef struct {
    volatile uint32_t owner;   /* 当前服务的票号 */
    volatile uint32_t next;    /* 下一张发出的票号 */
} spinlock_t;

void spin_lock(spinlock_t *lock) {
    uint32_t my_ticket = atomic_inc_return(&lock->next);
    while (lock->owner != my_ticket) {
        /* 让出 CPU，避免忙等 */
        hvc_call(HVC_SCHED_YIELD, 0);
    }
}

void spin_unlock(spinlock_t *lock) {
    lock->owner++;
}
```

> **为什么用 `sched_yield` 而不是 `wfe`？**
> 同物理核上的多 vCPU 是分时复用的。如果次核用 `wfe` 忙等，会占满时间片，主核得不到执行，
> 导致死锁（次核等主核释放锁，主核等次核让出 CPU）。
> 用 `sched_yield` 主动让出，让调度器切到其他任务。

### 7.5 SMP 测试

测试共享计数器的并发累加：

```
物理 CPU0
├─ vm1-cpu0 (主核): 累加 10000 次
└─ vm1-cpu1 (次核): 累加 10000 次

共享内存布局 (SMP_TEST_ADDR = 0x40300000):
  offset 0x00: ready_flag  (主→次握手)
  offset 0x08: done_flag   (次→主握手)
  offset 0x10: counter      (共享计数器)
  offset 0x18: lock         (自旋锁)
```

预期结果：`counter == 20000`

实际运行结果：
```
[vm1] smp: counter = 20000 (expected 20000)
[vm1] smp: PASSED (spinlock + shared counter correct)
```

---

## 8. 问题与解决方案

### 8.1 问题一：次 vCPU MPIDR 与 vcpu_id 不同步

**现象**：Guest 次核读 MPIDR_EL1 返回 0（应该返回 1）。

**根因**：`create_task_on_cpu` 内部用 `vcpu_id = 0` 初始化 `cpu_sysregs.mpidr_el1`，
调用方随后修改了 `vcpu_id = 1`，但没有同步修改 `mpidr_el1`。

**修复**：修改 `vcpu_id` 时同步修改 `mpidr_el1`：
```c
cpu0_tasks[pid]->vcpu_id = 1;
cpu0_tasks[pid]->cpu_sysregs.mpidr_el1 = 1;  /* 必须同步！*/
```

### 8.2 问题二：ivshmem 回归——VM2 回复中断 VM1 收不到

**现象**：VM1 发 ping → VM2 能收到 ✅；VM2 发回复 → VM1 收不到 ❌。

**定位过程**：
1. 在 `virt_mmio_write` 加调试打印，发现 VM2 写 doorbell 寄存器的值是 `0x41117000`，
   而不是预期的 `0`（目标 peer 0）。
2. `target_peer = val & 0xFFFF = 0x7000`，远大于 `IVSHMEM_MAX_PEERS=2`，
   直接 return，不注入中断。
3. 实验验证：
   - `ivshmem_doorbell(0xDEAD)` → 写出去的值是 0xDEAD ✅（正确）
   - `ivshmem_doorbell(0)` → 写出去的值是 0x41117000 ❌（错误）
4. 只有参数为 0 时出错 → 推测编译器优化了零值写入，使用 WZR（零寄存器）。

**根因**：MMIO 写操作处理时，从 ESR_EL2.ISS 提取源寄存器编号 `srt`，
然后从 `pt_regs->regs[srt]` 读取值。但 pt_regs 只有 X0-X30 共 31 个寄存器，
`srt = 31` 对应 WZR/XZR，此时 `regs[31]` 是越界访问，读到垃圾值。

**修复**：在 `handle_mem_abort` 中判断 srt == 31 时，val = 0。

```c
if (srt == 31) {
    val = 0;  /* WZR/XZR */
} else {
    val = regs->regs[srt];
    if (sf == 0) val &= 0xFFFFFFFFULL;  /* 32-bit */
}
```

> 同时修复了另一个小问题：32 位写操作之前直接读完整的 64 位寄存器，
> 现在根据 SF 位正确截断为 32 位。

### 8.3 问题三：次核忙等占满 CPU

**现象**：次核用 `while (flag == 0) wfe();` 等待主核，但主核迟迟得不到执行。

**根因**：同核调度下，次核占满时间片。`wfe` 会 trap 到 EL2，但 trap handler 可能立刻返回，
没有触发调度。结果就是次核不断 trap → 返回 → 再 trap → 再返回，占满 CPU。

**修复**：Guest 等待循环中用 `hvc_call(HVC_SCHED_YIELD, 0)` 主动让出 CPU，
让调度器切换到其他任务。

### 8.4 问题四：ivshmem peer_id 不匹配

**现象**：次 vCPU 创建后，ivshmem 的 peer 映射被打乱。

**根因**：
1. `create_task_on_cpu` 时 vmid 是临时分配的（`next_vmid` 递增）
2. 次 vCPU 的 board 初始化会调用 `ivshmem_init`，修改全局 `ivshmem_peers[]`
3. VM1 次 vCPU 的临时 vmid 是 2，导致 peer[1] 被错误设置

**修复**：
1. 新增 `ivshmem_set_peer_id()` 函数，支持动态修正 peer 映射
2. 次 vCPU 通过 `virt_attach_secondary` 挂接主 vCPU 的设备
3. 所有 vCPU 创建完成后，统一调用一次修正函数

---

## 9. 测试结果

### 9.1 测试环境

- **物理平台**：QEMU virt，4 CPU，GICv3 + ITS
- **命令**：`make -f Makefile.virt run`
- **配置**：VM1 双 vCPU（都在 CPU0），VM2 单 vCPU（CPU0）

### 9.2 测试矩阵

| 测试项 | VM1 | VM2 | 结果 |
|--------|-----|-----|------|
| virtio-net loopback | ✅ | ✅ | 通过 |
| ivshmem ping-pong | ✅ | ✅ | 通过 |
| edu MSI interrupt | ✅ | - | 通过 |
| vITS (vLPI 注入) | ✅ | - | 通过 |
| Phase 10 综合 (sched/mem/shm) | ✅ | ✅ | 通过 |
| Phase 11 vSMP (spinlock + counter) | ✅ | - | 通过 |

### 9.3 vSMP 测试详情

```
[vm1] smp: counter = 20000 (expected 20000)
[vm1] smp: PASSED (spinlock + shared counter correct)
```

两个 vCPU 各累加 10000 次，最终结果 20000，Ticket 自旋锁正确工作，无数据竞争。

---

## 10. 遗留问题与后续规划

### 10.1 遗留问题

1. **start.S 与 C 代码 MPIDR 不一致**：原因未明，暂时用主核竞选机制绕开
2. **CPU1 启动问题**：PSCI CPU_ON 返回成功，但次核没有执行
   - 可能需要 QEMU `-machine virt,secure=on`
   - 当前所有 vCPU 都在 CPU0 上调度，不影响功能验证
3. **vGIC LR 扫描 bug**：`vgic_inject_virq()` 中 LR 扫描范围是 `NR_VGIC_LRS - 1`
   - 只扫 LR1，LR2 被浪费
   -  `ivshmem_mmio_write` 中同样的 bug 已修复
   -  当前场景不触发（因为走保存状态路径）

### 10.2 Phase 12 规划

- **vSGI 虚拟化**：Guest 写 ICC_SGI1R 触发 SGI，Hypervisor 捕获并路由到目标 vCPU
- **跨物理核 vCPU**：修复 CPU1 启动，让 vCPU 可以在不同物理核上运行
- **IPI 注入**：跨核 vCPU 之间的虚拟中断注入（需要硬件 IPI 支持）

---

## 11. 文件改动清单

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `hypervisor/common/mm.c` | 修复 | MMIO 写操作处理 WZR/XZR 寄存器；32/64位 操作正确截断 |
| `hypervisor/boards/virt/main_virt.c` | 新增 | VM1 次 vCPU 创建；ivshmem peer 修正 |
| `hypervisor/boards/virt/guest_loader.c` | 新增 | `shared_vm_loader` 次 vCPU 加载器 |
| `hypervisor/boards/virt/virt_board.c` | 修改 | `virt_attach_secondary`；ivshmem peer 恢复 |
| `hypervisor/boards/virt/ivshmem.c` | 新增 | `ivshmem_set_peer_id` 动态修正 peer |
| `include/boards/virt/ivshmem.h` | 新增 | 接口声明 |
| `include/boards/virt/virt_board.h` | 新增 | 接口声明 |
| `guests/el1_test/guest.h` | 新增 | Ticket 自旋锁；SMP 共享内存布局；HVC_SCHED_YIELD |
| `guests/el1_test/main.c` | 新增 | `phase11_smp_test`；主核竞选；次核测试逻辑 |
| `guests/el1_test/start.S` | 修改 | 次核入口 |

---

## 12. 版本信息

- **版本 tag**：`v0.11.0-phase11-vsmp`
- **前序版本**：`v0.10.0-phase10-advanced`
- **完成日期**：2025-07
