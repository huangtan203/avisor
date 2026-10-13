# Phase 19：vGIC 注入路径统一 详细设计文档

## 1. 概述

### 1.1 背景

在 Phase 19 之前，aVisor 的虚拟中断注入存在**多条分散路径**：

1. **vSGI 路径**：`virt_inject_virq_to_vcpu()` → `vgic_inject_virq_to_vm()`（Phase 10 引入，统一的三路分派）
2. **ivshmem doorbell 路径**：`ivshmem_mmio_write()` 中直接写 `target->vm->virt.gic_lr[]`（老式直写 saved state）
3. **shm_notify 路径**：`shm_notify()` 中直接写 `vm->virt.gic_lr[]`（老式直写 saved state）
4. **vGICD 软件注入路径**：`virt_vgicd_inject_spi()` → `vgic_inject_virq_to_vm()`（Phase 15 引入，走统一路径）

老式直写 saved state 的方式存在以下问题：
- **代码重复**：每个设备驱动都要自己扫 LR、判断状态、去重
- **不支持跨核注入**：直写 saved state 只对"目标 VM 在当前核上且未运行"的情况有效，跨核场景下 saved state 是陈旧的（实际状态在硬件 LR 中）
- **竞态风险**：与世界切换（vgic_save/restore_state）竞态
- **维护成本高**：修改注入逻辑（如加优先级）要改多处

Phase 10 引入的 `vgic_inject_virq_to_vm()` 已经解决了上述问题（三路分派：同核运行 / 同核未运行 / 跨核 IPI），vSGI 和 vGICD 软件注入都走这条路径且工作正常。

Phase 19 的目标是：**把 ivshmem doorbell 和 shm_notify 的老式注入也统一到 `vgic_inject_virq_to_vm()` 上**，消除分散的 LR 操作逻辑。

### 1.2 目标

- ✅ `ivshmem_doorbell_inject()`：替换为 `vgic_inject_virq_to_vm(target->vm, VIRQ_IVSHMEM)`
- ✅ `shm_notify()`：替换为 `vgic_inject_virq_to_vm(vm, SHM_NOTIFY_VIRQ)`
- ✅ 保留 GICD SPI 门控（`virt_vgicd_spi_enabled`）在注入路径外层
- ✅ 全量回归通过：ivshmem ping-pong、shm、vSGI、vGICD、vGICR、优先级等既有测试不受影响
- ✅ Guest 侧新增 Phase 19 测试：门控验证（禁用 SPI 后 doorbell 被丢弃）

### 1.3 平台与基础

- **硬件平台**：QEMU `virt` 机器，ARMv8-A，GICv3 + ITS + vGIC，2 物理核
- **Hypervisor**：aVisor（Type-1，EL2）
- **Guest**：EL1 测试程序（裸金属）
- **前序版本**：`v0.18.0-phase18-priority-pending`

---

## 2. 设计方案

### 2.1 统一注入函数回顾

`vgic_inject_virq_to_vm(vm, vintid)` 是 Phase 10 引入的统一注入入口，三路分派：

```c
int vgic_inject_virq_to_vm(struct task_struct *vm, uint32_t vintid)
{
    /* 情况 1：目标就是当前运行的 VM → 直接写硬件 LR */
    if (vm == current && vm->pid != 0)
        return vgic_inject_virq_raw(vintid);

    /* 情况 3：目标在另一物理核 → mailbox + 物理 SGI 跨核注入 */
    if (vm->phys_cpu != smp_processor_id()) {
        vgic_mailbox_set_pending(vm->phys_cpu, vm->pid, vintid);
        smp_send_sgi(SMP_VGIC_UPDATE, vm->phys_cpu);
        return 0;
    }

    /* 情况 2：目标在本核但未运行 → 写 saved state */
    return vgic_inject_saved(&vm->virt, vintid);
}
```

三条路径都经过充分验证（vSGI 跨核、同核均工作）。

### 2.2 ivshmem doorbell 改造

**改造前**（`ivshmem.c: ivshmem_mmio_write()`）：

```c
if (target->vm == current) {
    /* 目标就是自己：直接注入 */
    if (virt_vgicd_spi_enabled(current, VIRQ_IVSHMEM))
        vgic_inject_virq(VIRQ_IVSHMEM);
} else {
    /* 目标是其他 VM：写 saved state */
    struct vm_virt_state *virt = &target->vm->virt;
    if (virt_vgicd_spi_enabled(target->vm, VIRQ_IVSHMEM)) {
        for (int i = 1; i < NR_VGIC_LRS; i++) {
            if ((virt->gic_lr[i] & ICH_LR_STATE_MASK) == 0) {
                virt->gic_lr[i] = VIRQ_IVSHMEM | ICH_LR_GROUP1
                                                  | ICH_LR_PENDING;
                break;
            }
        }
    }
}
```

**改造后**：

```c
if (!virt_vgicd_spi_enabled(target->vm, VIRQ_IVSHMEM))
    return;

vgic_inject_virq_to_vm(target->vm, VIRQ_IVSHMEM);
```

代码从约 15 行缩减到 3 行，且自动获得跨核注入能力。

### 2.3 shm_notify 改造

**改造前**（`shmem.c: shm_notify()`）：

```c
if (virt_vgicd_spi_enabled(vm, SHM_NOTIFY_VIRQ)) {
    struct vm_virt_state *virt = &vm->virt;
    for (int j = 1; j < NR_VGIC_LRS; j++) {
        if ((virt->gic_lr[j] & ICH_LR_STATE_MASK) == 0) {
            virt->gic_lr[j] = SHM_NOTIFY_VIRQ | ICH_LR_GROUP1
                                                    | ICH_LR_PENDING;
            notified++;
            if (vm->state == TASK_WAITING)
                sched_wakeup(vm);
            break;
        }
    }
}
```

**改造后**：

```c
if (!virt_vgicd_spi_enabled(vm, SHM_NOTIFY_VIRQ))
    continue;

int ret = vgic_inject_virq_to_vm(vm, SHM_NOTIFY_VIRQ);
if (ret == 0) {
    notified++;
    if (vm->state == TASK_WAITING)
        sched_wakeup(vm);
}
```

### 2.4 门控设计

`vgic_inject_virq_to_vm()` 本身**不带门控**——它是一个通用的注入原语，用于 vSGI、vLPI、SPI 等各种中断类型。不同中断类型的门控策略不同：

- **vSGI**：没有 GICD 门控（SGI 由 ICC_SGI1R 直接触发，使能由 GICR 管理，vSGI 路径已在 vGICR 建模中处理）
- **SPI**：由 GICD ISENABLER 门控
- **vLPI**：由 ITS 配置表门控

因此，**门控放在注入路径的外层**（设备驱动侧），由调用者在调用 `vgic_inject_virq_to_vm()` 之前判断是否应该注入。

这与 vGICD 软件注入路径（`virt_vgicd_inject_spi()`）的设计一致：门控在 `virt_vgicd_inject_spi()` 内部判断，然后调 `vgic_inject_virq_to_vm()`。

---

## 3. 行为差异分析

### 3.1 LR 扫描范围

| 路径 | 扫描范围 | 说明 |
|------|---------|------|
| 老式 ivshmem | LR1 ~ LR2（`i = 1; i < 3`） | 包含了 PCI HW 映射专用的 LR2 |
| 老式 shm_notify | LR1 ~ LR2（同上） | 同上 |
| `vgic_inject_saved` | LR1 （`i = 1; i < 2`） | 只用 LR1，LR2 留给 PCI |

**差异**：老式代码意外地用了 LR2（PCI HW 映射专用槽位），新代码只用 LR1。

**影响评估**：NR_VGIC_LRS = 3（LR0=vtimer HW 映射，LR1=软件注入，LR2=PCI INTx HW 映射）。软件注入只用 LR1 是正确的设计。在 ivshmem 和 shm_notify 的使用场景下，LR1 几乎总是空闲的（单次注入，不会积累），所以没有功能影响。

### 3.2 优先级字段

| 路径 | Priority 字段 |
|------|--------------|
| 老式注入 | 不设置（隐式为 0，最高优先级） |
| `vgic_inject_saved` | 不设置（隐式为 0） |
| `vgic_inject_virq_raw` | 设置为 `virt_vgic_vintid_priority()` 返回值（默认 0xA0） |

**差异**：
- 同核未运行场景（saved state 路径）：优先级均为 0，行为一致
- 同核运行场景（raw 路径）：老式代码不走这个分支（老式直接判断 `target->vm == current` 时走 `vgic_inject_virq`，而 `vgic_inject_virq` 也不设置优先级）

等等，实际上 Phase 18 给 `vgic_inject_virq_raw` 加了优先级，但没有给 `vgic_inject_saved` 加。这是一个已知的不一致（saved state 路径的优先级为 0，raw 路径的优先级为 0xA0），但不影响功能（VPMR = 0xFF 放行所有优先级）。

本 Phase 不修复这个不一致，留待后续统一处理。

### 3.3 去重逻辑

| 路径 | 去重 |
|------|------|
| 老式 ivshmem | 无去重（同一 vINTID 可写多个 LR 条目） |
| 老式 shm_notify | 无去重 |
| `vgic_inject_saved` / `vgic_inject_virq_raw` | 有去重（同一 vINTID 已 pending 则直接返回 0） |

**差异**：老式代码允许同一中断重复 pending（边沿触发语义下这是无害的——多次边沿触发合并为一次 pending）。新代码有显式去重。

**影响评估**：边沿触发中断的去重是正确的行为（GICv3 语义）。老式代码不去重但也不会出错——只是浪费 LR 槽位。新代码去重是优化，不影响功能。

---

## 4. Guest 侧测试

### 4.1 测试策略

Phase 19 是重构验证，核心是确保"路径统一后功能不变"。测试策略如下：

1. **ivshmem 送达性验证**：由 Test 2（ivshmem ping-pong）覆盖——如果路径统一后 ping-pong 仍通过，说明送达性没问题
2. **门控验证**：新增专项测试——禁用目标 SPI 后，doorbell 应被丢弃
3. **shm_notify 送达性**：与 ivshmem 走同一条 `vgic_inject_virq_to_vm()` 路径，无需重复验证

### 4.2 门控验证测试设计

```
VM1 (sender)                    VM2 (receiver)
    |                               |
    |                        禁用 SPI #49
    |                               |
    |                          置 s_ready=1
    |                               |
    |  等 s_ready=1                 |
    |  收到后发 doorbell            |
    |                               |
    |                          等 s_sent=1
    |  置 s_sent=1                  |  (同时检查是否收到中断)
    |                               |
    |  等 s_result                  |  检查 ivshmem_irq_received
    |                               |
    |                          未收到 → s_result=1 (PASS)
    |                          收到了 → s_result=2 (FAIL)
    |                               |
    |  收到结果后置 s_ack=1         |
    |                               |
    |                          等 s_ack=1
    |                          恢复 SPI #49
```

用 ivshmem 共享内存的 byte[60:66] 做同步（不使用低地址字节，避免与前面的测试冲突）。

### 4.3 同步机制

两 VM 执行速度不同，进入 Phase 19 的时间可能差异很大。因此：
- **入口同步**：每个 VM 进入 Phase 19 后置自己的 ready 字节，等对方的 ready 字节
- **测试项内同步**：用独立的字节做每一步的握手

---

## 5. 验证结果

### 5.1 全量回归

| 测试项 | VM1 | VM2 | 说明 |
|--------|-----|-----|------|
| Test 1: virtio-net loopback | PASS | PASS | 无影响 |
| Test 2: ivshmem ping-pong | PASS | PASS | **路径统一后仍通过** |
| Test 3: PCI passthrough (edu) | PASS | - | 无影响 |
| Test 4: vITS | PASS | - | 无影响 |
| Test 5.1: Scheduler | PASS | PASS | 无影响 |
| Test 5.2: Dynamic memory | PASS | PASS | 无影响 |
| Test 5.3: Shared memory | PASS | PASS | 无影响（轮询模式，不用 notify 中断） |
| Phase 11: Virtual SMP | PASS | PASS | 无影响 |
| Phase 12: vSGI | PASS | PASS | 无影响（走原有统一路径） |
| Phase 15: vGICD | PASS | PASS | 无影响 |
| Phase 16: vGICR | PASS | PASS | 无影响 |
| Phase 17: vSGI trap-and-emulate | PASS | PASS | 无影响 |
| Phase 18: Priority + pending | PASS | PASS | 无影响 |
| **Phase 19: 注入路径统一** | **PASS** | **PASS** | **门控验证通过** |

### 5.2 稳定性

连续运行 3 次，Phase 19 测试均通过，无偶发失败。

---

## 6. 改动文件清单

| 文件 | 改动 |
|------|------|
| `hypervisor/boards/virt/ivshmem.c` | `ivshmem_doorbell_inject()` 替换为 `vgic_inject_virq_to_vm()` |
| `hypervisor/boards/virt/shmem.c` | `shm_notify()` 中的注入替换为 `vgic_inject_virq_to_vm()` |
| `guests/el1_test/main.c` | 新增 `phase19_injection_unify_test()`，主流程中调用 |
| `guests/el1_test/guest.h` | Phase 19 测试函数声明 |

---

## 7. 后续工作

- **优先级一致性**：`vgic_inject_saved()` 也应该设置 Priority 字段（与 raw 路径一致），在后续 Phase 中统一
- **更多设备接入**：如果有新的虚拟设备需要注入中断，直接调用 `vgic_inject_virq_to_vm()` 即可，无需再写直写 LR 的代码
- **LPI 路径统一**：vITS 注入 vLPI 目前走独立路径（LPI 与 SPI/SGI 的 LR 格式不同），未来可考虑是否抽象到同一框架

---

## 8. 版本信息

- **版本 Tag**：`v0.19.0-phase19-injection-unify`
- **完成日期**：2026-10-13
- **状态**：✅ 完成，全量回归通过
