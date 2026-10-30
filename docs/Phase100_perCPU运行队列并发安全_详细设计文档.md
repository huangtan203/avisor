# aVisor Hypervisor Phase 100 详细设计文档：per-CPU 运行队列并发安全

> 版本：Phase 100
> 平台：QEMU virt（ARMv8-A、EL2、GICv3）
> 前置阶段：Phase 99 VM/vCPU 对象模型与生命周期分层
> 目标：让 per-CPU task 表的入队、出队、查找和调度扫描拥有统一同步边界

---

## 1. 背景与问题

Phase 99 建立了 VM registry，但 task 创建和回收仍直接修改 `cpu_state[cpu].task[]`、`nr_tasks` 和 `rr_start`。这些字段同时被以下路径访问：

- 普通上下文的 vCPU 创建和 task 回收；
- 架构定时器中断触发的调度；
- vGIC/跨核通知路径对远端 vCPU 的查找；
- VM freeze/transaction 对运行队列的扫描。

如果普通上下文修改任务表时被 timer IRQ 重入，或者远端 CPU 在任务槽位更新过程中读取，就可能出现半更新的指针、错误的 `nr_tasks` 上界和释放后继续使用的 task 指针。

本阶段只处理运行队列的一致性，不实现跨 pCPU 迁移。迁移所需的双队列有序加锁和 reschedule IPI 留给 Phase 101。

## 2. 设计目标

### 2.1 功能目标

- 每个 pCPU 拥有独立的 runqueue lock。
- 统一提供 enqueue/dequeue 接口，封装槽位复用和上界收缩。
- 调度选择、时间片重填和轮转游标更新在锁内完成。
- 普通上下文持有本核 runqueue 锁期间屏蔽 IRQ，避免 timer IRQ 自锁。
- 远端 vCPU 查找按 CPU 逐个加锁，返回稳定的 task 指针快照。
- task 回收保持“从 runqueue 摘除 → VM registry 解绑 → 释放内存”的顺序。
- 保留旧任务和 VM 接口，不改变 Guest ABI。

### 2.2 非目标

- 不实现跨 CPU 迁移；
- 不实现负载均衡；
- 不引入 reschedule IPI；
- 不把全部 task 状态转移到状态机 API；
- 不修改用户已存在的 MMIO、PCI 或 Guest 测试逻辑。

## 3. 架构设计

```text
             +-----------------------------+
             | scheduler / timer IRQ      |
             +--------------+--------------+
                            |
                     rq_irq_save/restore
                            |
       +--------------------v--------------------+
       |          cpu_state[cpu]                 |
       |  rq_lock | task[] | nr_tasks | rr_start |
       +-----------+-------------+---------------+
                   |             |
          sched_enqueue      sched_dequeue
                   |             |
       +-----------v-------------v---------------+
       |         task_struct / VM registry       |
       +-----------------------------------------+
```

锁的职责：

| 数据 | 保护者 | 访问规则 |
|---|---|---|
| `task[]` | 当前 CPU 的 `rq_lock` | 只能通过 enqueue/dequeue 或锁内调度代码访问 |
| `nr_tasks` | 当前 CPU 的 `rq_lock` | 插入和尾部收缩必须同锁完成 |
| `rr_start` | 当前 CPU 的 `rq_lock` | 选择任务和更新游标必须同锁完成 |
| `vm_table` | Phase 99 的 VM registry lock | 不由 runqueue lock 替代 |
| `task_struct` 生命周期 | runqueue + VM registry 顺序 | 摘除后才允许释放 |

## 4. 数据结构

### 4.1 `cpu_sched_state`

```c
struct cpu_sched_state {
    struct task_struct *curr;
    struct task_struct *task[NR_TASKS];
    int nr_tasks;
    int rr_start;
    volatile unsigned int rq_lock;
    struct task_struct init_task;
};
```

`rq_lock` 使用 0/1 状态：0 表示未持有，1 表示持有。其他阶段可在此结构上增加负载统计或迁移状态，但不能绕过本阶段锁规则。

### 4.2 IRQ 屏蔽

```text
rq_irq_save
  -> mrs daif
  -> msr daifset, #2
  -> 返回旧 DAIF

rq_irq_restore
  -> msr daif, old_daif
```

只屏蔽 IRQ，不改变 FIQ、SError 和 Debug mask。这样 timer IRQ 不会在同一 CPU 上重入 runqueue 临界区，临界区结束后恢复调用前状态。

## 5. 锁实现与内存顺序

`rq_lock()` 使用 AArch64 exclusive load/store：

```text
LDAXR 读取锁
若非 0 则自旋
写入 1
STXR 尝试发布
失败则重试
```

`rq_unlock()` 使用 `STLR wzr` 发布解锁。其作用是：

1. 获取锁后读取到前一持有者对 task 表的完整更新；
2. 解锁前对 `task[]`、`nr_tasks` 和 `rr_start` 的写入不会被重排到解锁之后；
3. 远端 CPU 按同一锁读取时不会观察到插入过程中的半状态。

注意：对远端 CPU 加锁时，本 CPU 的 IRQ 也必须保持屏蔽，否则本 CPU timer IRQ 可能在等待远端锁时造成锁顺序问题。`sched_find_vcpu()` 因此在遍历所有 CPU 前保存并屏蔽本地 IRQ。

## 6. 核心接口设计

### 6.1 `sched_enqueue_task(int cpu, struct task_struct *tsk)`

调用关系：

```text
sched_enqueue_task
  -> 参数校验
  -> rq_irq_save
  -> rq_lock(cpu)
  -> 扫描 1..nr_tasks-1 的空槽
  -> 无空槽且未达到 NR_TASKS 时扩展 nr_tasks
  -> 写入 task[slot]
  -> 更新 pid / phys_cpu
  -> rq_unlock
  -> rq_irq_restore
```

特性：

- task 已存在于同一队列时返回原槽位，保持幂等；
- 队列满时返回 `-1`；
- idle task 固定在 slot 0，不会被覆盖。

### 6.2 `sched_dequeue_task(struct task_struct *tsk)`

调用关系：

```text
sched_dequeue_task
  -> 由 tsk->phys_cpu 定位队列
  -> rq_irq_save / rq_lock
  -> 找到指针并写 NULL
  -> 从尾部收缩 nr_tasks
  -> 解锁并恢复 IRQ
```

返回值：

- `1`：成功摘除；
- `0`：task 不在队列中；
- `-1`：task 或 CPU 参数无效。

它只摘除指针，不释放内存。释放由 task 回收路径在完成 VM registry detach 后执行。

### 6.3 `sched_find_vcpu(unsigned long vmid, int vcpu_id)`

按 CPU 顺序扫描每个队列，每次只持有一个 runqueue lock。匹配条件为：

```text
vmid 相等 && vcpu_id 相等 && state != TASK_ZOMBIE
```

找到后立即释放队列锁并返回指针。调用者必须保证返回对象仍由 VM registry 或其他生命周期机制持有；本阶段不引入引用计数。

### 6.4 `_schedule()`

调度流程：

```text
_schedule
  -> rq_lock(local_cpu)
  -> 读取 rr_start
  -> 扫描 RUNNING task 和最高 counter
  -> 找到候选：更新 rr_start，保存 selected，解锁
  -> 未找到：在锁内重填所有 RUNNING task 的 counter，解锁并重试
  -> switch_to(selected)
```

关键规则：`switch_to()` 不在 runqueue 锁内调用。世界切换可能访问 VM 虚拟化状态，不能把它和队列锁绑定在一起。

## 7. task 生命周期改造

### 7.1 创建

Phase 99 的 task 创建流程改为：

```text
allocate task / freeze_context / FIFO
  -> 初始化 vCPU 和 VM 字段
  -> sched_enqueue_task(cpu, task)
  -> vm_attach_vcpu(task, vmid, vcpu_id)
```

如果 VM attach 失败：

```text
sched_dequeue_task
  -> destroy_task_aux
  -> deallocate_page(task)
```

因此不会留下半初始化的调度槽位。

### 7.2 回收

```text
remove_task_slot
  -> 校验 task 位于当前 CPU
  -> sched_dequeue_task
  -> vm_detach_vcpu
  -> destroy_task_aux
  -> deallocate_page
```

这个顺序保证其他调度扫描者不会再拿到即将释放的 task 指针。

## 8. 函数调用关系

### 8.1 创建路径

```text
create_vcpu_on_cpu
  -> create_vcpu_task_on_cpu
     -> sched_enqueue_task
        -> rq_irq_save
        -> rq_lock
        -> task slot update
        -> rq_unlock
        -> rq_irq_restore
     -> vm_attach_vcpu
```

### 8.2 调度路径

```text
arch_timer_irq
  -> timer_tick
     -> counter decrement
     -> _schedule
        -> rq_lock
        -> select / refill
        -> rq_unlock
        -> switch_to
```

### 8.3 回收路径

```text
reap_zombie_tasks
  -> remove_task_slot
     -> sched_dequeue_task
     -> vm_detach_vcpu
     -> free task resources
```

### 8.4 远端查找路径

```text
virt_find_vcpu / irq route lookup
  -> sched_find_vcpu
     -> rq_irq_save
     -> for each CPU: rq_lock -> scan -> rq_unlock
     -> rq_irq_restore
```

## 9. 自检设计

`sched_runqueue_selftest(0)` 使用栈上临时 task，不接入 VM registry：

1. 设置 `TASK_WAITING`，避免被调度；
2. enqueue，验证返回有效 slot；
3. 重复 enqueue，验证返回同一 slot；
4. dequeue，验证返回 1；
5. 再次 dequeue，验证返回 0；
6. 临时 task 离开队列，原 runqueue 状态恢复。

该自检覆盖本阶段最容易回归的槽位复用、重复入队和重复出队行为。

## 10. 代码映射

| 文件 | 改动 |
|---|---|
| `include/common/sched.h` | 增加 `rq_lock` 和 runqueue API 声明 |
| `hypervisor/common/sched.c` | 锁、IRQ 保存恢复、enqueue/dequeue/find、自检、调度锁保护 |
| `hypervisor/arch/aarch64/task.c` | 创建和回收统一调用 enqueue/dequeue |
| `hypervisor/boards/virt/main_virt.c` | 启动阶段执行 Phase 100 runqueue 自检 |
| `docs/phase-logs/phase100.log` | 构建和 QEMU 回归记录 |

## 11. 兼容性与限制

- `cpu_state[].task[]` 的布局不变，已有调度算法不变。
- Phase 99 的 VM registry 锁和 runqueue 锁职责分离。
- 现有 virtio、ivshmem、vGIC、MSI 和迁移代码接口不变。
- `smp.c`、`vgic.c` 等旧的裸扫描路径尚未全部迁移到 `sched_find_vcpu()`；本阶段只提供安全 API，调用方迁移在相关后续阶段完成。
- 跨 CPU 迁移不能在本阶段调用；Phase 101 需要双队列排序加锁以及 IPI 协调。

## 12. 验证方案与结果

### 构建

```sh
make -f Makefile.virt -j$(nproc)
```

结果：通过，生成 `bin_virt/kernel8.img`。

### QEMU

```sh
timeout 30 make -f Makefile.virt run-log \
    QEMU_SERIAL_LOG=/tmp/avisor-phase100.log
```

结果：QEMU 持续运行，30 秒后由 timeout 正常结束；退出码 124 属于预期行为。

通过项目：

- Phase 100 runqueue selftest；
- VM1/VM2 ivshmem ping-pong；
- VM2 → VM3 双 vCPU Live Migration；
- 无 `PANIC`、`ASSERT`、`FAILED` 或 `SOME tests FAILED`。

基线 warning：`INIT_TASK` 聚合初始化和既有 MM/Guest 警告仍存在，不属于本阶段新增错误。

## 13. 下一阶段

Phase 101 将在本阶段 runqueue API 之上实现协作式跨 pCPU vCPU 迁移：

- source/target runqueue 有序双锁；
- `TASK_MIGRATING` 状态；
- 目标 CPU reschedule IPI；
- 中断路由 affinity 与 vCPU 物理 CPU 的同步。
