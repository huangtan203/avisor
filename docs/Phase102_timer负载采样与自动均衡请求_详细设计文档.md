# aVisor Hypervisor Phase 102 详细设计文档：timer 负载采样与自动均衡请求

> 版本：Phase 102
> 平台：QEMU virt（ARMv8-A、EL2、GICv3）
> 前置阶段：Phase 101 vCPU 跨 pCPU 显式迁移
> 目标：基于 pCPU 负载差异生成自动迁移请求，并在 Guest HVC 安全点复用 Phase 101 迁移协议

## 1. 项目概述

### 1.1 背景

Phase 101 已经实现当前 vCPU 显式迁移，但迁移必须由 Guest 主动发起 HVC。若两个 pCPU 的运行负载长期不均衡，Hypervisor 需要能从 timer tick 中观察 busy/idle 比例，并在负载差异达到阈值时产生自动均衡请求。

本阶段采用协作式安全点模型：timer IRQ 只采样负载并设置迁移目标，不在 IRQ handler 内直接迁移；当前 vCPU 下一次执行 HVC 时，EL2 在同步异常返回前检查请求，并调用 Phase 101 的迁移函数。这样避免在 timer IRQ 中切换 Guest 上下文导致异常返回、GIC EOI/DIR 和源 pCPU 状态不一致。

### 1.2 设计目标

- 每个 pCPU 按 100 个 EL2 timer tick 形成一个负载窗口；
- 统计 busy/idle tick，并发布 0–100 的 `load_percent`；
- 当前 pCPU 负载不低于 75%，且比另一在线 pCPU 高至少 25 个百分点时生成迁移请求；
- 通过 cooldown 限制连续窗口重复请求，避免迁移抖动；
- 为每个 vCPU 累计 `runtime_ticks`；
- 在 HVC 同步安全点复用 Phase 101 的双队列锁、`TASK_MIGRATING` 和上下文切出流程。

### 1.3 非目标

- 不在 timer IRQ 中直接执行跨 pCPU 上下文切换；
- 不实现远程强制迁移或 CPU affinity；
- 不按运行时间选择最优候选 vCPU；
- 不实现多于两个 pCPU 的拓扑负载选择；
- 不新增 Guest 负载查询 ABI；
- 不修改 PCI 设备 IRQ affinity 或 DMA 路由。

## 2. 架构设计

```text
EL2 timer IRQ(cpu N)
        |
        v
sched_balance_tick(N)
  |-- busy_ticks / idle_ticks
  |-- current->runtime_ticks++
  |-- 100 tick -> load_percent
  |-- threshold -> balance_target[N] = other CPU
        |
        | 仅记录请求，不切换上下文
        v
Guest 下一次 HVC -> handle_sync_exception
        |
        v
sched_balance_safe_point()
  |-- 读取并清除 balance_target
  |-- 校验 current 和 TASK_RUNNING
  `-- sched_migrate_current(target)
             |
             `-- Phase 101 双 runqueue 锁 + 上下文切出
```

### 2.1 负载窗口

每个 pCPU 独立维护：

- `busy_ticks`：timer tick 到达时当前 task 非 idle 的次数；
- `idle_ticks`：当前 task 为 idle task 的次数；
- `load_percent = busy * 100 / (busy + idle)`；
- `load_percent` 通过 acquire/release 原子访问供另一 pCPU 读取。

窗口达到 100 tick 后发布负载并清零窗口计数。当前 task 非 idle 时同时递增 `runtime_ticks`，该字段用于后续更精细的候选选择，但本阶段不向 Guest 暴露。

### 2.2 触发策略

```text
local_load >= 75
&& remote_load + 25 <= local_load
&& remote_cpu online
&& current 非 idle
&& current == TASK_RUNNING
```

满足条件时设置 `balance_target[local_cpu] = remote_cpu`，并设置 5 个窗口的 cooldown。cooldown 每个完成的负载窗口递减，避免在短时间内重复产生迁移请求。

### 2.3 安全点

`balance_target` 只表示请求，不表示迁移已经完成。HVC 路径调用 `sched_balance_safe_point()`：

1. 原子读取本 CPU 的目标 CPU；
2. 立即清除请求，避免同一请求重复执行；
3. 校验当前 task 仍为非 idle、`TASK_RUNNING` 且属于当前 CPU；
4. 调用 `sched_migrate_current(target)`；
5. 失败时设置短 cooldown，等待后续窗口再次尝试。

迁移实际状态机、source/target runqueue 有序加锁、`TASK_MIGRATING` 和源上下文切出回调均由 Phase 101 提供，本阶段不复制实现。

## 3. 数据结构设计

### 3.1 `task_struct`

```c
unsigned long runtime_ticks;
```

该字段随 task/vCPU 一起存在，task 迁移或 VM Live Migration 时不改变其含义。

### 3.2 `cpu_sched_state`

```c
unsigned long busy_ticks;
unsigned long idle_ticks;
unsigned int load_percent;
```

这些字段属于 pCPU 调度统计，不属于 Guest 可见状态。`balance_target[MAX_CPUS]` 和 `balance_cooldown[MAX_CPUS]` 为 scheduler 内部状态。

## 4. 函数调用关系

### 4.1 Timer 采样路径

```text
handle_timer_irq()
  -> hyp_tick_count[cpu]++
  -> sched_balance_tick(cpu)
     -> 统计 current pid
     -> current->runtime_ticks++（非 idle）
     -> 100 tick 窗口完成
     -> sched_cpu_load(other)
     -> 设置 balance_target / cooldown
  -> timer_tick()
```

### 4.2 HVC 安全点路径

```text
handle_sync_exception(ESR_EL2_EC_HVC64)
  -> handle_hvc64(hvc_nr)
  -> sched_balance_safe_point()
     -> balance_target[cpu]
     -> sched_migrate_current(target)
        -> Phase 101 ordered dual-rq lock
        -> TASK_MIGRATING
        -> schedule()
        -> task_reap_after_switch()
        -> sched_complete_migration()
```

安全点放在 `handle_hvc64()` 返回后，保证 Guest HVC 本身已经完成处理；不会在 timer IRQ 中途改变当前异常上下文。

## 5. 错误处理和并发约束

- CPU 参数越界时负载读取返回 0；
- 当前 task 为空时不采样；
- 目标 CPU 离线时不设置迁移请求；
- idle task 不产生自动迁移请求；
- 请求只在 `TASK_RUNNING` 且 `phys_cpu` 与当前 CPU 一致时执行；
- `balance_target` 使用 acquire/release 原子访问；
- 实际队列转移继续由 Phase 101 的 runqueue 锁保护；
- timer IRQ 不调用 `schedule()`、不获取双队列锁、不发送迁移上下文切换；
- workload 长时间不执行 HVC 时，请求保持为待处理状态，直到下一次 HVC 安全点。

## 6. 文件与代码映射

| 文件 | 实现内容 |
|---|---|
| `include/common/sched.h` | `runtime_ticks`、pCPU 统计字段和 Phase 102 接口 |
| `hypervisor/common/sched.c` | 负载窗口、阈值判断、cooldown 和安全点迁移请求 |
| `hypervisor/boards/virt/arch_timer.c` | 每个 timer tick 调用负载采样 |
| `hypervisor/arch/aarch64/sync_exc.c` | HVC 返回后的安全点检查 |
| `hypervisor/common/smp.c` | Phase 101 迁移完成后的目标核重调度机制，保持不变 |
| `hypervisor/arch/aarch64/task.c` | Phase 101 上下文切出后的迁移完成回调，保持不变 |

## 7. 验证过程和结果

### 7.1 构建

```text
make -f Makefile.virt -j$(nproc)
结果：PASS；生成 bin_virt/kernel8.img，大小 251568 字节。
```

构建期间保留了基线 warning；没有新增链接错误或编译错误。

### 7.2 QEMU 回归

```text
timeout 70 make -f Makefile.virt run-log \
  QEMU_SERIAL_LOG=/tmp/avisor-phase102.log
结果：返回 124，QEMU 被 timeout 正常终止。
```

观察结果：

- Phase 100 runqueue selftest 通过；
- Phase 101 显式往返迁移 `CPU0 -> CPU1 -> CPU0` 通过；
- 两个 pCPU 均持续输出 100、200、300、400、500、600、700 tick 心跳；
- HVC 返回后的安全点检查路径已接入；本轮标准 workload 未提供可独立观测的负载百分比或自动迁移标记，因此不把“自动触发迁移”列为实测通过项；
- VM1/VM2 virtio-net、ivshmem 和 vITS 回归通过；
- VM2 -> VM3 双 vCPU Live Migration 通过；
- 未发现 `PANIC` 或 `ASSERT`。

### 7.3 已知基线问题

全量 Guest 回归仍输出既有 Phase 44 task-slot reuse 测试失败：

```text
FAIL: task 槽位未被实际复用
```

该路径不属于 Phase 102，本阶段没有修改其 HVC 或 task-slot reuse 实现，故单独记录，不将其伪装为本阶段通过。

## 8. 限制与后续

当前平台只有两个 pCPU，自动均衡只能选择另一个 CPU；`runtime_ticks` 仅用于累计，尚未参与候选排序，也没有 Guest 查询 ABI。若 Guest 长时间不执行 HVC，自动请求会延迟到下一次同步安全点。后续阶段可增加可中断 EL2 reschedule safe-point 协议和更细粒度的负载决策。
