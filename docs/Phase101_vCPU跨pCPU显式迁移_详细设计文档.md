# aVisor Hypervisor Phase 101 详细设计文档：vCPU 跨 pCPU 显式迁移

> 版本：Phase 101
> 平台：QEMU virt（ARMv8-A、EL2、GICv3）
> 前置阶段：Phase 100 per-CPU 运行队列并发安全
> 目标：实现 Guest 可显式请求的 vCPU CPU0/CPU1 往返迁移

## 1. 项目概述

### 1.1 背景

Phase 100 为每个 pCPU 的 runqueue 建立了独立锁和统一访问接口，但 vCPU 仍固定在创建时的物理 CPU 上。多 vCPU VM 在 CPU 负载变化或后续调度策略需要时，必须能够把当前 vCPU 从源 pCPU 安全转移到目标 pCPU。

本阶段实现显式迁移原语和最小 Guest 验证闭环：Guest 通过 HVC 指定目标 CPU，Hypervisor 以双 runqueue 有序加锁完成队列转移，并通过 SGI 通知目标 CPU 尽快重新调度。

### 1.2 目标

- 支持当前非 idle vCPU 在两个 online pCPU 之间显式迁移；
- 保持 `task_struct::phys_cpu`、runqueue 槽位和 VM registry 的一致性；
- 迁移期间不让目标 CPU 调度未完成上下文切出的 task；
- 避免 source/target runqueue 的 ABBA 死锁；
- 以 HVC 和 Guest 输出验证 CPU0 -> CPU1 -> CPU0；
- 保持 Phase 100 的队列锁边界和既有设备、内存、虚拟中断功能不变。

### 1.3 非目标

- 不实现自动负载均衡和周期性迁移；
- 不实现 CPU affinity 或远程迁移请求；
- 不在 IRQ handler 中直接执行上下文切换；
- 不修改物理 PCI IRQ affinity；
- 不改变 VM registry 中的 VM/vCPU 归属；
- 不提前实现后续阶段的迁移策略和迁移禁止区。

## 2. 架构设计

### 2.1 组件关系

```text
Guest vCPU
   |
   | HVC_SCHED_MIGRATE_CPU / HVC_SCHED_GET_CPU
   v
handle_hvc64()
   |
   v
sched_migrate_current()
   |                     \
   |                      \-- smp_request_reschedule(target)
   v                                  |
source rq + target rq                 v
有序双锁、task 转移              SGI #6 -> 目标 CPU counter=0
   |
   v
TASK_MIGRATING
   |
   | 当前 CPU schedule() / sched.S switch_to 完成
   v
 task_reap_after_switch()
   |
   v
sched_complete_migration() -> TASK_RUNNING
```

### 2.2 迁移状态机

```text
TASK_RUNNING
    |
    | 源 CPU 持有 source/target rq 锁
    v
TASK_MIGRATING
    |
    | task 已进入目标队列，但上下文尚未完成切出
    v
源 CPU 完成 schedule 与上下文保存
    |
    v
TASK_RUNNING
```

目标调度器只选择 `TASK_RUNNING` task，因此迁移中 task 虽已位于目标队列，也不会在上下文保存完成前被目标 CPU 执行。

### 2.3 双队列锁顺序

源 CPU 和目标 CPU 按 CPU 编号升序加锁：

```text
first_cpu  = min(source_cpu, target_cpu)
second_cpu = max(source_cpu, target_cpu)
```

两个方向的迁移都遵守相同顺序，从而避免 CPU0 等 CPU1、CPU1 等 CPU0 的 ABBA 死锁。持锁期间保存并屏蔽本地 IRQ，避免 timer IRQ 重入本 CPU runqueue 临界区。

### 2.4 数据一致性

迁移只改变调度归属，不改变 VM registry 归属：

| 数据 | 迁移动作 |
|---|---|
| 源 `task[source_slot]` | 清空 |
| 目标 `task[target_slot]` | 写入 task 指针 |
| `nr_tasks` | 必要时扩展或从源队尾收缩 |
| `task->pid` | 更新为目标队列槽位 |
| `task->phys_cpu` | 更新为目标 CPU |
| `task->vm`、VMID、vCPU ID | 保持不变 |
| task state | `RUNNING -> MIGRATING -> RUNNING` |

## 3. 接口和数据结构

### 3.1 task 状态

`include/common/sched.h` 增加：

```c
#define TASK_MIGRATING 3
```

该状态表示 task 已经从源队列移出并插入目标队列，但源 CPU 尚未完成上下文切出。

### 3.2 Scheduler 接口

```c
int sched_migrate_current(int target_cpu);
void sched_complete_migration(struct task_struct *tsk);
```

`sched_migrate_current()` 只允许迁移当前非 idle、处于 `TASK_RUNNING` 且属于当前 pCPU 的 task。目标 CPU 必须在线且不能与源 CPU 相同。目标队列没有可用槽位时返回失败。

### 3.3 HVC 接口

```c
#define HVC_SCHED_MIGRATE_CPU 74
#define HVC_SCHED_GET_CPU     75
```

- HVC 74：`x0` 传入目标 pCPU，返回 0 表示迁移请求成功；
- HVC 75：返回当前执行 vCPU 所在的 pCPU 编号。

### 3.4 SGI 接口

```c
#define SGI_SCHED_RESCHEDULE 6
```

迁移完成发布后向目标 CPU 发送 SGI。目标 CPU 的 SGI handler 只把当前 task 的时间片清零，不直接切换上下文，后续正常 timer/schedule 路径负责选择任务。

## 4. 核心函数设计

### 4.1 `sched_migrate_current`

调用关系：

```text
handle_hvc64(HVC_SCHED_MIGRATE_CPU)
  -> sched_migrate_current(target_cpu)
     -> 校验 current、CPU、状态和 online
     -> rq_irq_save()
     -> 以 CPU 编号升序锁定两个 runqueue
     -> 查找源槽位和目标空槽
     -> task->state = TASK_MIGRATING
     -> 源队列清空 task 并收缩尾部
     -> 目标队列写入 task
     -> 更新 pid / phys_cpu
     -> 解锁两个 runqueue
     -> rq_irq_restore()
     -> schedule()
```

失败路径在改变 task 状态前释放两个锁并恢复 IRQ，返回 `-1`。

### 4.2 `sched_complete_migration`

调用关系：

```text
sched.S switch_to
  -> 保存 prev 上下文
  -> task_reap_after_switch(prev)
     -> sched_complete_migration(prev)
        -> release store TASK_RUNNING
        -> smp_request_reschedule(prev->phys_cpu)
```

使用 release store 发布状态，确保目标 CPU 观察到 `TASK_RUNNING` 时，队列和 task 归属字段已经完成更新。

### 4.3 `task_reap_after_switch`

该函数在架构上下文切换完成后执行：

- `prev->state == TASK_MIGRATING`：完成迁移发布；
- `prev->state == TASK_ZOMBIE`：执行既有 task 槽位回收；
- 其他状态：不做处理。

因此迁移 task 不会在仍使用旧 CPU 上下文时提前变为可运行状态。

### 4.4 SGI 重调度路径

```text
sched_complete_migration
  -> smp_request_reschedule(target_cpu)
     -> smp_send_ipi(target_cpu, SGI_SCHED_RESCHEDULE)
        -> smp_ipi_handle(SGI_SCHED_RESCHEDULE)
           -> current->counter = 0
```

## 5. Guest 验证设计

只由 VM1/vCPU0 执行验证，避免多个 vCPU 同时发起同一迁移请求：

```text
HVC_GET_CPU == 0
  -> HVC_MIGRATE_CPU(1) == 0
  -> HVC_GET_CPU == 1
  -> HVC_MIGRATE_CPU(0) == 0
  -> HVC_GET_CPU == 0
```

成功输出：

```text
sched: vCPU migration CPU0->CPU1->CPU0 PASSED
```

任一步骤失败输出 `sched: FAILED (vCPU migration)`。

## 6. 文件与代码映射

| 文件 | 实现内容 |
|---|---|
| `include/common/sched.h` | `TASK_MIGRATING` 和迁移接口声明 |
| `hypervisor/common/sched.c` | 双队列有序加锁、队列转移、迁移完成发布 |
| `hypervisor/arch/aarch64/task.c` | 上下文切出后的迁移 task 回收钩子 |
| `hypervisor/arch/aarch64/sched.S` | 切换后调用 `task_reap_after_switch` |
| `include/common/hvc.h` | HVC 74/75 ABI 常量 |
| `hypervisor/arch/aarch64/sync_exc.c` | HVC 分发 |
| `include/boards/virt/gicv3.h` | SGI #6 定义 |
| `include/common/smp.h` | 重调度请求声明 |
| `hypervisor/common/smp.c` | SGI 发送和处理 |
| `guests/el1_test/guest.h` | Guest HVC 常量 |
| `guests/el1_test/main.c` | CPU0 -> CPU1 -> CPU0 验证 |

## 7. 错误处理和并发约束

- 目标 CPU 越界、离线、等于源 CPU、当前 task 为 idle、task 不属于当前 CPU 或状态不是 `TASK_RUNNING` 时拒绝迁移；
- 目标队列满或源槽位不存在时不修改 task，释放锁后返回失败；
- 所有 source/target 队列访问均在对应 `rq_lock` 保护下完成；
- 不在 runqueue 锁内调用 `schedule()` 或 `switch_to()`；
- 不在 SGI handler 中直接切换 Guest 世界；
- VM registry 不需要 detach/attach，迁移不改变 VM/vCPU 对象关系。

## 8. 验证过程和结果

### 8.1 构建

```text
make -f Makefile.virt -j$(nproc)
结果：成功，生成 bin_virt/kernel8.img
```

### 8.2 QEMU 回归

```text
timeout 30 make -f Makefile.virt run-log \
  QEMU_SERIAL_LOG=/tmp/avisor-phase101.log
结果：QEMU 被 timeout 正常终止（返回 124），串口日志生成。
```

### 8.3 关键结果

- Guest 输出：`sched: vCPU migration CPU0->CPU1->CPU0 PASSED`；
- VM1/VM2 virtio-net loopback 测试通过；
- VM1/VM2 ivshmem ping-pong 测试通过；
- vITS MSI/LPI 测试通过；
- VM2 -> VM3 双 vCPU Live Migration 测试通过；
- 未发现 `PANIC` 或 `ASSERT`；Phase 101 迁移和上述既有回归均通过；
- 本次运行还输出 Phase 44 task-slot reuse 自测失败（`first-pid`/`second-pid` 均为无效值）。该路径不属于 Phase 101，本阶段未修改；因此本次全量日志不能记为无失败。应在对应阶段处理该遗留失败。

验证记录摘要：`docs/phase-logs/phase101.log`。

## 9. 后续阶段接口

Phase 101 只提供显式迁移原语。自动负载统计、迁移决策、CPU affinity、设备 IRQ affinity 联动和迁移策略应在后续阶段独立设计，不能由本阶段的 Guest HVC 直接替代。
