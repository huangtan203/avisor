# aVisor Hypervisor Phase 99 详细设计文档：VM/vCPU 对象模型与生命周期分层

> 版本：Phase 99
> 平台：QEMU virt（ARMv8-A、EL2、GICv3）
> 目标：建立独立于调度器和板级设备模型的 VM/vCPU 归属注册表，使创建、查找、迁移和销毁使用一致的身份模型

---

## 1. 项目背景与问题

Phase 99 之前，`task_struct` 同时承担调度实体、vCPU 身份和部分 VM 资源容器三种职责。调用者先通过 `create_task_on_cpu()` 创建任务，再直接覆盖 `vmid`、`vcpu_id`、`phys_cpu` 和 `MPIDR_EL1`。板级代码另外维护 `(vmid, vcpu_id) -> task_struct` 数组。

该模型存在以下问题：

1. vCPU 创建不是原子操作。任务加入调度表后，身份字段才由调用者逐项覆盖。
2. `vmid` 与虚拟 MPIDR 可能短暂或永久不一致。
3. 通用代码无法按 VMID 查找 vCPU，只能依赖 virt 板级私有表。
4. task 回收后若板级槽位未同步清除，会留下悬挂指针。
5. Live Migration 直接修改 `task_struct::vmid`，归属更新和回滚缺少统一事务边界。

本阶段引入最小 VM registry，将“在哪个 pCPU 上运行”继续交给调度器，将“属于哪个 VM、是哪个 vCPU”交给通用 VM 层。

## 2. 设计目标

### 2.1 功能目标

- 建立固定容量的 VM 注册表。
- 使用 `(vmid, vcpu_id)` 唯一标识一个 vCPU。
- 在创建 vCPU 时一次性写入 VMID、vCPU ID、物理 CPU 和虚拟 MPIDR。
- 提供 attach、detach、rebind 和 lookup 接口。
- task 回收前从注册表解绑。
- Live Migration 身份切换和失败回滚通过同一个接口完成。
- 保留旧 `create_task_on_cpu()`，避免一次性破坏旧平台和自测调用点。

### 2.2 非目标

本阶段不实现：

- per-CPU runqueue 锁和跨核任务队列同步；
- vCPU 跨 pCPU 迁移；
- 动态 VMID 分配和 generation；
- VM 级 Stage-2、设备和 IRQ 资源全部迁入 `struct vm`；
- 删除 virt 板级旧 vCPU 表。

## 3. 总体架构

```text
                    +----------------------+
                    |     VM registry      |
                    | vm_table[vmid - 1]   |
                    +----------+-----------+
                               |
                 (vmid, vcpu_id) -> task_struct
                               |
              +----------------+----------------+
              |                                 |
     +--------v---------+             +---------v--------+
     | vCPU / task      |             | Scheduler        |
     | identity + state |             | pCPU task table  |
     +--------+---------+             +---------+--------+
              |                                 |
              +-------------+-------------------+
                            |
                    +-------v--------+
                    | virt board     |
                    | device/vGIC    |
                    +----------------+
```

职责分层：

| 层 | 负责内容 | 不负责内容 |
|---|---|---|
| VM registry | VMID、vCPU ID、归属、查找、重绑定 | 时间片、pCPU 队列、设备模拟 |
| Scheduler | task 队列、current、时间片、上下文切换 | VM 身份分配 |
| task/vCPU | CPU 上下文、系统寄存器、运行状态 | 全局 VM 查找策略 |
| virt board | vGIC、virtio、ivshmem 等板级状态 | 通用 VM 生命周期 |

## 4. 数据结构设计

### 4.1 `struct vm`

```c
#define VM_MAX_COUNT 4
#define VM_MAX_VCPUS 4

struct vm {
    unsigned long vmid;
    unsigned int nr_vcpus;
    struct task_struct *vcpus[VM_MAX_VCPUS];
};
```

- `vmid`：当前槽位所属 VM；0 表示空槽。
- `nr_vcpus`：非空 vCPU 指针数量，用于一致性检查和空 VM 回收。
- `vcpus[]`：以 vCPU ID 为索引的反向查找表。
- VMID 直接映射 `vm_table[vmid - 1]`，避免动态内存和哈希依赖。

### 4.2 `task_struct::vm`

`task_struct` 新增 `struct vm *vm`。它与兼容字段 `vmid` 形成双向关系：

```text
vcpu->vm == &vm_table[vcpu->vmid - 1]
vcpu->vm->vcpus[vcpu->vcpu_id] == vcpu
```

`vm_registry_selftest()` 检查上述不变量以及 `nr_vcpus` 计数。

## 5. 并发与内存顺序

registry 由 `vm_table_lock` 保护。锁使用 AArch64 exclusive 指令：

```text
获取：LDAXR + STXR
释放：STLR
```

- `LDAXR` 提供 acquire 语义，确保获得锁后看到前一个持有者发布的数据。
- `STLR` 提供 release 语义，确保槽位和 vCPU 字段先于解锁可见。
- attach、detach、rebind 和 lookup 都在同一锁域内访问 registry。

锁只保护归属数据，不替代 runqueue 锁。调用者若移动正在运行的 vCPU，仍需先停止 vCPU；跨 pCPU 调度同步属于后续阶段。

## 6. 核心接口

### 6.1 `vm_attach_vcpu()`

输入：vCPU 指针、VMID、vCPU ID。

处理流程：

```text
vm_attach_vcpu
  -> 校验 VMID/vCPU ID
  -> vm_lock
  -> 检查目标槽位冲突
  -> 检查 vCPU 是否已属于其他 VM
  -> 建立 vm->vcpus[vcpu_id]
  -> 更新 vm->nr_vcpus
  -> 写入 vcpu->vm/vmid/vcpu_id
  -> 同步 MPIDR_EL1.Aff0
  -> vm_unlock
```

重复 attach 同一个 vCPU 是幂等操作；目标槽位已有其他 vCPU 时失败。

### 6.2 `vm_detach_vcpu()`

从当前 VM 槽位移除 vCPU，递减计数并清空 `vcpu->vm`。最后一个 vCPU 解绑时将槽位 VMID 复位为 0。

### 6.3 `vm_rebind_vcpu()`

在一次锁持有期间完成旧 VM 解绑和新 VM 绑定。先检查目标冲突，再修改旧槽位，因此目标冲突不会破坏原归属。该接口用于迁移提交和失败回滚。

### 6.4 `vm_find_vcpu()`

按 `(vmid, vcpu_id)` 返回 vCPU。virt 板级查找首先调用通用 registry，旧板级数组仅作为兼容回退。

### 6.5 `create_vcpu_on_cpu()`

```c
int create_vcpu_on_cpu(unsigned long vmid, int vcpu_id, int cpu,
                       loader_func_t loader, void *arg);
```

它通过共享内部函数完成 task 分配、设备模型初始化、系统寄存器初始化、调度槽位插入和 registry attach。任何一步失败都会撤销已经完成的资源分配。

## 7. 函数调用关系

### 7.1 启动创建路径

```text
hypervisor_main
  -> create_vcpu_on_cpu(vmid, vcpu_id, pcpu, loader, arg)
     -> create_vcpu_task_on_cpu
        -> allocate_page(task)
        -> allocate_page(freeze_context)
        -> init_task_console
        -> board_ops->initialize
        -> prepare_initial_sysregs
        -> task_stack_init
        -> cpu_state[cpu].task[pid] = task
        -> vm_attach_vcpu
           -> vm_slot
           -> vm_lock
           -> 建立双向归属
           -> vm_unlock
```

### 7.2 vCPU 查找路径

```text
virt_inject_virq_to_vcpu
  -> virt_find_vcpu
     -> vm_find_vcpu
        -> vm_slot
        -> vm_lock / lookup / vm_unlock
     -> vgic_inject_virq_to_vm
```

### 7.3 task 回收路径

```text
task_reap_after_switch / reap_zombie_tasks
  -> remove_task_slot
     -> 从 cpu_state.task[] 摘除
     -> vm_detach_vcpu
     -> destroy_task_aux
     -> deallocate_page(task)
```

### 7.4 Live Migration 身份切换

```text
migration_finish
  -> smp_vm_txn_begin
  -> vm_rebind_vcpu(vcpu1, destination_vmid, vcpu_id)
  -> 更新 mm / board_data
  -> 成功：提交迁移事务
  -> 失败：vm_rebind_vcpu(vcpu1, source_vmid, vcpu_id)
           -> 恢复 mm / board_data / name
```

## 8. 错误处理与回滚

| 失败位置 | 处理 |
|---|---|
| VMID 或 vCPU ID 越界 | 返回 `-1`，不修改状态 |
| 目标 vCPU 槽冲突 | 返回 `-1`，保留原归属 |
| task/freeze context/FIFO 分配失败 | 释放此前资源 |
| registry attach 失败 | 从调度槽摘除 task，释放辅助页和 task 页 |
| migration rebind 失败 | 取消冻结事务，不修改 VM 资源 |
| migration 后续步骤失败 | rebind 回源 VM，并恢复资源指针和名称 |

## 9. 代码映射

| 文件 | Phase 99 改动 |
|---|---|
| `include/common/vm.h` | VM 数据结构与生命周期接口 |
| `hypervisor/common/vm.c` | registry、锁、attach/detach/rebind/lookup/selftest |
| `include/common/sched.h` | `task_struct::vm` 前向关联 |
| `include/common/task.h` | `create_vcpu_on_cpu()` 声明 |
| `hypervisor/arch/aarch64/task.c` | 显式 vCPU 创建、attach 失败回滚、回收 detach |
| `hypervisor/boards/virt/main_virt.c` | VM1/VM2/VM3 使用显式身份创建 |
| `hypervisor/boards/virt/virt_board.c` | vCPU 查找优先使用通用 registry |
| `hypervisor/common/migration.c` | 迁移身份切换和回滚使用 `vm_rebind_vcpu()` |

## 10. 兼容性

- `create_task()` 与 `create_task_on_cpu()` 保留，使用递增 VMID 创建单 vCPU task。
- virt 板级 `vm_vcpus` 数组保留，现有注册/注销调用无需一次性删除。
- `task_struct::vmid` 保留供 VTTBR、日志和既有代码使用。
- 不改变 Guest ABI、HVC 编号、Stage-2 布局和设备地址。

## 11. 验证方案

### 11.1 构建验证

```sh
make -f Makefile.virt clean
make -f Makefile.virt -j$(nproc)
```

验收标准：Hypervisor 和 EL1 Guest 编译、链接并生成 `bin_virt/kernel8.img`。

### 11.2 运行验证

```sh
timeout 30 make -f Makefile.virt run-log
```

检查项：

- VM1/VM2 两个 vCPU 均启动；
- ivshmem 与 virtio 既有测试不回退；
- Live Migration 完成或保持基线行为；
- 日志无 `PANIC`、`ASSERT`；
- 不出现 vCPU 查找失败或重复槽位。

### 11.3 一致性验证

`vm_registry_selftest()` 可遍历 registry，验证 VMID 槽位、双向指针和计数。后续阶段可将其接入启动自测或 HVC 诊断。

## 12. 验证记录

- 构建：通过，生成 `bin_virt/kernel8.img`。
- 本阶段首次构建发现 `vm.c` 缺少 `NULL` 定义，补充通用工具头后通过。
- 基线已有的 Guest 与 `mm.c` 编译警告仍存在，本阶段未扩大这些警告。
- QEMU 回归结果在本阶段提交前执行并记录于 Git commit/tag。

## 13. 限制与下一阶段

- 固定支持最多 4 个 VM、每 VM 4 个 vCPU。
- registry 锁不保护 per-CPU task 表。
- VM 级资源仍分散在 vCPU 和 `board_data` 中。
- 旧板级 vCPU 表尚未删除。

Phase 100 将为 per-CPU runqueue 增加并发安全的入队、出队与快照接口，使 registry 生命周期和调度队列生命周期拥有明确的锁顺序。
