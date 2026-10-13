# Phase 10: 高级功能集（调度器升级 + HVC 系统调用 + 虚拟内存优化 + 中断性能优化）— 详细设计文档

## 1. 概述

Phase 10 在 Phase 9（vITS 虚拟化）的基础上，一次性实现四个高级功能子模块，全面提升 aVisor hypervisor 的功能性和性能：

| 子模块 | 功能点 | 对应文件 |
|--------|--------|----------|
| **A. 调度器升级** | 优先级调度、时间片轮转、yield、wait/wakeup | `sched.h` / `sched.c` |
| **B. HVC 系统调用扩展** | 调度类 + 内存类 + 共享内存类共 10 个 hypercall | `hvc.h` / `sync_exc.c` |
| **C. 虚拟内存优化** | 动态映射 hyp_mem_map/unmap、跨 VM 共享内存 shm | `mm.c` / `shmem.c` / `shmem.h` |
| **D. 中断性能优化** | 通用 vCPU 注入接口、LPI 延迟 profiling | `vgic.h` / `vgic.c` / `irq.c` |

**平台**：QEMU virt（ARMv8-A, GICv3 + ITS + vGIC）  
**验证方式**：Guest EL1 测试程序 + QEMU 运行验证

---

## 2. 子模块 A：调度器升级

### 2.1 设计目标

原调度器是简单的"counter-based"时间片轮转，所有任务优先级相同。Phase 10 升级为支持：
- **多级优先级**（低/普通/高）
- **优先级加权时间片**（高优先级任务获得更多时间片）
- **主动让出 CPU**（sched_yield）
- **等待/唤醒机制**（sched_wait / sched_wakeup）

### 2.2 优先级模型

```c
#define SCHED_PRIO_LOW      1
#define SCHED_PRIO_NORMAL   2
#define SCHED_PRIO_HIGH     3
#define SCHED_PRIO_MAX      SCHED_PRIO_HIGH

#define SCHED_TIMESLICE_LOW     1   /* 单位：tick */
#define SCHED_TIMESLICE_NORMAL  2
#define SCHED_TIMESLICE_HIGH    4
```

优先级到时间片的映射：

| 优先级 | 时间片（tick） | 说明 |
|--------|---------------|------|
| 1（LOW） | 1 | 低优先级，1 个 tick 就被抢占 |
| 2（NORMAL） | 2 | 默认优先级 |
| 3（HIGH） | 4 | 高优先级，4 个 tick 时间片 |

内联转换函数：
```c
static inline int sched_prio_to_timeslice(int prio)
{
    if (prio <= 0) return 1;
    switch (prio) {
    case 1: return SCHED_TIMESLICE_LOW;
    case 2: return SCHED_TIMESLICE_NORMAL;
    default: return SCHED_TIMESLICE_HIGH;
    }
}
```

### 2.3 任务状态扩展

新增 `TASK_WAITING` 状态：

```
TASK_RUNNING  ──sched_wait──▶  TASK_WAITING
     ▲                            │
     └──────sched_wakeup──────────┘
```

调度器 `_schedule()` 函数过滤掉 WAITING 状态的任务，只在 RUNNING 任务中选择下一个。

### 2.4 核心 API

| 函数 | 说明 |
|------|------|
| `sched_yield()` | 主动让出 CPU，保留剩余时间片，放到 runqueue 末尾 |
| `sched_set_priority(task, prio)` | 设置任务优先级，自动调整 counter |
| `sched_wait()` | 进入 WAITING 状态，主动调度切换 |
| `sched_wakeup(task)` | 唤醒 WAITING 任务，设为 RUNNING 并重置时间片 |

### 2.5 调度算法流程

```
timer tick → counter--
             ↓
       counter == 0 ?
             ↓ yes
       _schedule()
             ↓
       遍历所有任务：
         - 跳过 TASK_WAITING
         - 找 counter 最大的 RUNNING 任务
             ↓
       所有 counter 都为 0？
             ↓ yes
       重新分配 counter（仅 RUNNING 任务）：
         counter = sched_prio_to_timeslice(priority)
             ↓
       switch_to(next)
```

---

## 3. 子模块 B：HVC 系统调用扩展

### 3.1 调用约定

- **指令**：`hvc #0`
- **hypercall 号**：x8 寄存器
- **参数**：x0（arg0）、x1（arg1）
- **返回值**：x0
- **触发异常**：同步异常（EC=0x16，HVC from AArch64）

### 3.2 新增 hypercall 列表

| 编号 | 名称 | 参数 | 返回值 | 分类 |
|------|------|------|--------|------|
| 10 | `HVC_SCHED_YIELD` | - | 0 | 调度 |
| 11 | `HVC_SCHED_GET_PRIO` | - | 当前优先级 | 调度 |
| 12 | `HVC_SCHED_SET_PRIO` | w0=新优先级 | 0=成功/-1=失败 | 调度 |
| 20 | `HVC_MEM_MAP` | w0=IPA, w1=flags | 0=成功/-1=失败 | 内存 |
| 21 | `HVC_MEM_UNMAP` | w0=IPA | 0=成功/-1=失败 | 内存 |
| 30 | `HVC_SHM_CREATE` | w0=key, w1=size | shmid（>=0）或 -1 | 共享内存 |
| 31 | `HVC_SHM_ATTACH` | w0=shmid, w1=ipa | 0=成功/-1=失败 | 共享内存 |
| 32 | `HVC_SHM_DETACH` | w0=shmid | 0 | 共享内存 |
| 33 | `HVC_SHM_NOTIFY` | w0=shmid | notified 数量 | 共享内存 |

### 3.3 实现位置

Hypervisor 侧在 `hypervisor/arch/aarch64/sync_exc.c` 的 `handle_hvc64()` 函数中，以 `switch-case` 分发到具体处理逻辑。

---

## 4. 子模块 C：虚拟内存优化

### 4.1 动态内存映射（hyp_mem_map / hyp_mem_unmap）

#### 4.1.1 设计目标

允许 Guest 通过 hypercall 动态申请和释放 Stage-2 页表映射，用于：
- 按需分配物理内存
- 内存热插拔
- 共享内存 attach（见 4.2 节）

#### 4.1.2 hyp_mem_map 流程

```
Guest HVC_MEM_MAP(ipa, flags)
    ↓
检查 ipa 是否已映射（ipa_to_pa 查询）
    ↓ 未映射
从页池分配一个物理页（get_free_page）
    ↓
清零（memset 0）
    ↓
map_stage2_page(current, ipa, page_pa, flags)
    ↓
return 0
```

#### 4.1.3 hyp_mem_unmap 流程

```
Guest HVC_MEM_UNMAP(ipa)
    ↓
ipa_to_pa 查找对应的物理页 PA
    ↓
清除 Stage-2 L3 entry
    ↓
TLB flush（tlbi vmalle1is）
    ↓
释放物理页回页池（deallocate_page）
    ↓
return 0
```

#### 4.1.4 重要修正：已映射检查

使用 `ipa_to_pa()` 而非 `check_task_page_mapped()` 来判断地址是否已映射：

| 方式 | 精确性 | 副作用 |
|------|--------|--------|
| `check_task_page_mapped` | 只检查到 L2 table（2MB 粒度） | 会创建中间页表 |
| `ipa_to_pa` | 精确到 L3 page（4KB 粒度） | 纯查询，无副作用 |

**为什么不用 check_task_page_mapped**：它内部调用 `map_stage2_table()` 遍历时会分配中间表（有副作用），且只检查 L2 table 是否存在，不检查具体 L3 entry。导致同一 2MB 范围内有任何页被映射，所有地址都被误判为 "已映射"。

### 4.2 跨 VM 共享内存（shm）

#### 4.2.1 设计模型

借鉴 POSIX shm 的思路，提供 key-based 的共享内存机制：

```
         ┌──────────────────────┐
         │    全局 shm_table    │
         │  ┌────────────────┐  │
         │  │ shm[0]         │  │
         │  │  - key         │  │
         │  │  - page_pa     │  │──┐
         │  │  - refcount    │  │  │  共享物理页
         │  │  - attach[4]   │  │  │   ┌──────┐
         │  │    vm1: ipa1   │──┼──┼──▶│ Page │
         │  │    vm2: ipa2   │──┼──┘   └──────┘
         │  └────────────────┘  │
         │  ... (max 8)         │
         └──────────────────────┘
```

#### 4.2.2 数据结构

```c
#define SHM_MAX_COUNT       8   /* 最多 8 个共享内存段 */
#define SHM_MAX_ATTACH      4   /* 每个 shm 最多 4 个 VM  attach */
#define SHM_NOTIFY_VIRQ     60  /* 通知中断号（vSPI） */

struct shm_attach {
    struct task_struct *vm;
    paddr_t ipa;
};

struct shm_entry {
    int used;
    uint32_t key;
    paddr_t page_pa;
    size_t size;
    int refcount;
    struct shm_attach attach[SHM_MAX_ATTACH];
};
```

#### 4.2.3 核心操作

| 操作 | 说明 |
|------|------|
| `shm_create(key, size)` | 按 key 查找，存在则直接返回 shmid；否则分配新物理页 |
| `shm_attach(shmid, ipa)` | 将共享物理页映射到当前 VM 的指定 IPA |
| `shm_detach(shmid)` | 解除当前 VM 的映射，减少引用计数 |
| `shm_notify(shmid)` | 向所有其他 attach 的 VM 注入 vSPI 60 中断 |

#### 4.2.4 生命周期管理

Phase 10 采用**全局持久化**策略：shm 创建后一直存在，detach 只解除映射不销毁。

> **设计决策说明**：原设计为"最后一个 detach 自动销毁"，但在两个 VM 启动时序不一致的场景下会导致问题：
> - VM1 先 create → attach → 用完 detach → shm 被销毁
> - VM2 后 create → 拿到同名 key 但全新的物理页
> - 两边数据不互通
>
> 8 个 slot 对测试场景足够，简化生命周期管理。

#### 4.2.5 通知机制

`shm_notify(shmid)` 遍历 attach 列表，对每个非当前 VM：
1. 如果目标 VM 是 `current`（正在运行），走 `vgic_inject_virq()` 直接写 LR
2. 否则写入 `vm->virt.gic_lr[]` 保存状态，等下次调度到该 VM 时生效
3. 如果目标 VM 处于 `TASK_WAITING` 状态，调用 `sched_wakeup()` 唤醒

---

## 5. 子模块 D：中断性能优化

### 5.1 通用 vIRQ 注入接口

#### 5.1.1 背景

原 `vgic_inject_virq(vintid)` 只能向当前正在运行的 VM（current）注入中断。但在共享内存通知、跨 VM 信号等场景下，需要向**任意** VM 注入中断。

#### 5.1.2 vgic_inject_virq_to_vm

```c
int vgic_inject_virq_to_vm(struct task_struct *vm, int vintid);
```

逻辑：
- 如果 `vm == current`：直接调用 `vgic_inject_virq(vintid)` 写 GIC LR 寄存器
- 如果 `vm != current`：写入 `vm->virt.gic_lr[]` 数组，找一个空的 LR slot，设置 Pending 状态。下次调度到该 VM 时，`vgic_save/restore` 机制会把这些 LR 写入硬件。

### 5.2 LPI 延迟 Profiling

#### 5.2.1 测量点

在 `hypervisor/boards/virt/irq.c` 的 LPI 处理路径中：

```
LPI 触发
    ↓
t_start = read_cntpct()   // 读 CNTPCT_EL0
    ↓
查找目标 VM + vITS 翻译
    ↓
vgic_inject_virq_to_vm()
    ↓
t_end = read_cntpct()
    ↓
irq_stats_record_lpi(t_end - t_start)
```

#### 5.2.2 统计结构

```c
struct irq_stats {
    uint64_t count;
    uint64_t total_cycles;
    uint64_t max_cycles;
    uint64_t min_cycles;
};
```

记录总次数、总 cycles、最大/最小延迟。可通过 `irq_stats_dump()` 打印统计结果。

---

## 6. 验证结果

### 6.1 测试环境

- **平台**：QEMU 6.2+ virt machine
- **CPU**：ARMv8-A Cortex-A57 模拟，单核
- **GIC**：GICv3 + ITS
- **Guest**：2 个 VM（vm1 + vm2），各 256MB 内存

### 6.2 测试结果

```
[vm1] virtio-net: ALL loopback tests PASSED!
[vm1] edu: MSI interrupt PASSED
[vm1] vITS: PASSED! Received vSPI 72 (vLPI 8200)
[vm1] sched: PASSED
[vm1] mem: PASSED
[vm1] shm: PASSED
[vm1] Phase 10: ALL tests done

[vm2] virtio-net: ALL loopback tests PASSED!
[vm2] ivshmem: ping-pong test PASSED
[vm2] sched: PASSED
[vm2] mem: PASSED
[vm2] shm: PASSED
[vm2] Phase 10: ALL tests done
```

### 6.3 各子测试说明

| 测试 | 验证内容 |
|------|----------|
| sched | get_prio → set_prio(3) → verify → yield → 恢复 prio |
| mem | map 0x48000000 → 读写验证 → unmap |
| shm | VM1 创建 + attach + 写 PING + 等 PONG；VM2 attach + 等 PING + 回 PONG |

---

## 7. 关键 Bug 与解决方案

### Bug 1：shm attach 报 "already mapped"

**现象**：`hyp_mem_map` 成功映射并 unmap 后，`shm_attach` 到同一 2MB 范围内的另一地址时报 "already mapped"。

**根因**：`check_task_page_mapped()` 函数实现有缺陷：
1. 内部调用 `map_stage2_table()`，遍历时会分配中间页表（有副作用）
2. 只检查到 L2 table 是否存在，不检查 L3 的具体 page entry
3. 因此只要同一 2MB 范围内有任何页被映射过（导致 L2 table 被创建），所有同 2MB 内的地址都会返回 "已映射"

**修复**：用 `ipa_to_pa()` 替代 `check_task_page_mapped()` 做检查。`ipa_to_pa()` 是只读遍历且精确到 L3 page，返回 0 表示已映射，-1 表示未映射。

### Bug 2：共享内存 Ping-Pong 不通

**现象**：VM1 创建 shm 并 notify VM2，但 notified=0，VM2 收不到通知。

**根因**：
1. VM1 先跑完所有前置测试进入 Phase 10，VM2 还在跑 virtio-net / ivshmem 等
2. VM1 notify 时 VM2 还没 attach 到 shm
3. VM1 detach 后 shm 被销毁（原设计：最后一个 detach 自动销毁）
4. VM2 之后 create 拿到了一个全新的 shm（同 key 但不同物理页）

**修复**：
1. shm 生命周期改为全局持久化（detach 不销毁）
2. Guest 测试改为轮询模式（不用中断通知），忙等循环中调用 `sched_yield` 主动让出 CPU
3. 这样 VM1 写 PING 后反复 yield，VM2 有机会跑到 shm 测试并回复 PONG

---

## 8. 后续方向

1. **调度器**：引入多级 runqueue、实时优先级、CFS 完全公平调度
2. **HVC 接口**：增加参数校验、权限检查、返回值标准化
3. **共享内存**：
   - 支持多页共享内存（当前限制 1 页）
   - 支持权限控制（只读/读写）
   - 显式销毁接口（shm_destroy / shm_unlink）
   - 支持事件通知（类似 eventfd）
4. **虚拟内存**：
   - 页表回收（unmap 时回收空的中间页表）
   - 按需分页（demand paging）
   - 写时复制（CoW）
5. **中断性能**：
   - 批量注入（一次注入多个 vIRQ）
   - 中断聚合（interrupt coalescing）
   - 更细粒度的性能统计（分阶段耗时）

---

## 9. 文件变更清单

| 文件 | 类型 | 说明 |
|------|------|------|
| `include/common/sched.h` | 修改 | 新增优先级宏、TASK_WAITING、新 API 声明 |
| `hypervisor/common/sched.c` | 修改 | 调度逻辑升级（优先级、WAITING、yield、wait/wakeup） |
| `include/common/hvc.h` | 修改 | 新增 10 个 hypercall 编号 |
| `hypervisor/arch/aarch64/sync_exc.c` | 修改 | HVC handler 扩展（9 个新 case） |
| `include/boards/virt/virt_mm.h` | 修改 | 新增 hyp_mem_map/unmap 声明 |
| `hypervisor/common/mm.c` | 修改 | 新增 hyp_mem_map/unmap 实现 |
| `include/boards/virt/shmem.h` | 新增 | 共享内存接口头文件 |
| `hypervisor/boards/virt/shmem.c` | 新增 | 共享内存实现（create/attach/detach/notify） |
| `include/boards/virt/vgic.h` | 修改 | 新增 vgic_inject_virq_to_vm + 统计接口 |
| `hypervisor/boards/virt/vgic.c` | 修改 | 通用注入接口 + 延迟统计 |
| `hypervisor/boards/virt/irq.c` | 修改 | LPI 延迟测量 + 目标 VM 查找注入 |
| `hypervisor/boards/virt/main_virt.c` | 修改 | 增加 shmem_init 初始化调用 |
| `guests/el1_test/guest.h` | 修改 | 新增 HVC 号、hvc_call2 两参数版本 |
| `guests/el1_test/main.c` | 修改 | Phase 10 测试代码（sched/mem/shm） |
