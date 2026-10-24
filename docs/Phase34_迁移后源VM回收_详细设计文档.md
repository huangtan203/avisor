# Phase 34：迁移后源 VM 回收详细设计

## 1. 背景与目标

Phase 33 已实现 VM2 双 vCPU 到 VM3 的事务式整体迁移：

- VM2 vCPU0 在 FINISH 后进入 `TASK_ZOMBIE`；
- VM2 vCPU1 被就地改造为 VM3 vCPU1；
- VM3 vCPU0 从迁移快照恢复执行；
- VM3 两个 vCPU 继续共享 VM3 的 Stage-2 地址空间和设备模型。

迁移完成后，VM2 原 Stage-2 页表、普通物理页、设备模型和板级注册信息仍然存在。Phase 34 的目标是在不影响 VM3 和全局共享资源的前提下，回收这些源 VM 独占资源。

## 2. 设计约束

### 2.1 可回收资源

VM2 vCPU0 在迁移完成后仍持有源 VM 的资源入口：

1. `mm.first_table` 指向 VM2 Stage-2 根页表；
2. `board_data` 指向 VM2 的 `virt_vm_devices`；
3. `vm_vcpus[1][0]` 和 `vm_vcpus[1][1]` 仍保留 VM2 查找项；
4. `ivshmem_peers[1]` 仍可能指向 VM2 的 ivshmem 设备。

### 2.2 不可直接释放的共享资源

回收必须区分独占页与共享页：

- ivshmem 共享内存由所有 VM 映射同一组物理页，只能解除 VM2 映射；
- 跨 VM shm 由 `shm_table` 管理，只能删除 VM2 attach；
- KSM 页由引用计数管理，只有引用计数归零才能释放规范物理页；
- MMIO/device memory 不是页池普通页，只清除映射，不调用 `deallocate_page()`。

### 2.3 vCPU1 就地改造约束

Phase 33 已将原 VM2 vCPU1 的：

- `vmid` 改为 3；
- `mm` 改为 VM3 `mm`；
- `board_data` 改为 VM3 `board_data`。

因此 Phase 34 必须以 VM2 vCPU0 作为回收入口，不能通过已经改造的 vCPU1 回收资源。

## 3. 回收顺序

`migration_finish()` 在事务提交后按如下顺序调用回收：

1. `virt_unregister_vcpu(2, 0/1)`：清除 VM2 板级 vCPU 查找项；
2. `shm_detach_vm(src)`：解除 VM2 的全部 shm attach；
3. `ksm_drop_vm(src)`：释放 VM2 对 KSM 页的引用；
4. `virt_destroy_vm(src)`：注销 ivshmem peer、解除 ivshmem 共享内存映射、释放设备模型页；
5. `destroy_vm_pages(src)`：释放剩余普通页、Stage-2 L2/L3 页表页和根页表页。

共享资源必须先于 Stage-2 全树销毁处理，否则普通页遍历无法判断某个 Normal-memory 映射是否由其他 VM 共享。

## 4. 内存回收实现

### 4.1 `destroy_vm_pages()`

新增 Stage-2 全树销毁接口，遍历：

```text
L1 first_table
  └── L2 table
        ├── L2 block（2MB）
        └── L3 table
              └── L3 page（4KB）
```

处理规则：

- L2 Normal-memory block：释放 512 个 4KB 页；
- L3 Normal-memory page：释放对应页池物理页；
- KSM 标记页：由 `ksm_drop_vm()` 先处理，此处只清映射；
- device memory：只清映射，不释放物理页；
- 自底向上释放 L3、L2、L1 页表页；
- 将源 VM `first_table` 和页计数清零；
- 执行全局 Stage-2 TLB 失效。

返回值是释放的普通页与页表页总数（按 4KB 页计）。

### 4.2 `ksm_drop_vm()`

遍历源 VM Stage-2 L3 映射，对带 `MM_STAGE2_KSM_SHARED` 标志的页：

- 查找对应 `ksm_page`；
- 若释放前 `refcount > 1`，递减 `pages_sharing`；
- 递减 `refcount`；
- `refcount == 0` 时从哈希表删除，递减 `pages_shared`，释放规范页和元数据；
- 清除源 VM L3 entry。

若 KSM 标记页无法在哈希表中找到，接口返回失败，回收统计不会将操作误报为完成。

### 4.3 `shm_detach_vm()`

遍历全部 shm 区域与 attach 槽位：

- 找到 `attach.vm == src` 的记录；
- 调用 `unmap_stage2_page(src, ipa, 0)`，只解除映射；
- 清空 attach 槽；
- 递减 shm 引用计数。

保持现有 shm 持久化语义：引用计数归零时也不销毁共享物理页。

## 5. 设备模型与板级注册回收

### 5.1 ivshmem 注销

`struct ivshmem_dev` 新增 `shmem_ipa`，保存初始化时的 Guest IPA。`ivshmem_unregister()`：

- 从 `ivshmem_peers[]` 删除源 VM 设备；
- 按页解除源 VM 的 ivshmem 共享内存映射；
- 不释放全局 ivshmem 共享物理页；
- 清除设备的 VM 与 peer 信息。

### 5.2 VM 设备模型销毁

`virt_destroy_vm()`：

1. 调用 `ivshmem_unregister()`；
2. 释放 `virt_vm_devices` 所在物理页；
3. 将源 task 的 `board_data` 置空。

### 5.3 vCPU 查找表清理

`virt_unregister_vcpu()` 清除指定 `(vmid, vcpu_id)` 的 `vm_vcpus` 槽位。迁移后同时清除 VM2 vCPU0 与 vCPU1 的旧槽位；VM3 vCPU1 已在 Phase 33 注册到 VM3 槽位，不受影响。

## 6. 迁移状态与可观测性

新增回收统计：

- `pages`：释放的普通页和页表页数量；
- `shm`：解除的 shm attach 数量；
- `ksm`：释放的 KSM 映射数量；
- `done`：所有回收步骤是否成功。

新增 HVC：

```c
#define HVC_MIGRATE_RECLAIM_STATS 67
```

返回寄存器：

- `x0 = pages`
- `x1 = shm`
- `x2 = ksm`
- `x3 = done`

`done` 仅在 shm、KSM 和 Stage-2 回收都成功时置 1。

## 7. Guest 验证

新增 Test 22：`Phase 34 Source VM Reclaim`。

VM3 在迁移恢复后查询 HVC 67，并验证：

1. `done == 1`；
2. `pages > 0`；
3. Test 21 的 VM3 vCPU1 推进检查仍通过；
4. VM3 在回收后持续输出 tick，证明目标 VM Stage-2 和 vCPU1 未被误伤。

## 8. 回归结果

双核 QEMU 配置：

```text
-M virt,virtualization=on,gic-version=3,highmem=off
-cpu max
-smp 2
-m 1G
```

最终关键输出：

```text
INFO[2]: migration: source VM reclaimed (pages=17 shm=0 ksm=344 done=1)
[vm3] --- Test 21: Phase 33 Dual-vCPU Migration ---
[vm3] --- Test 22: Phase 34 Source VM Reclaim ---
[vm3]   reclaim: pages=17 shm=0 ksm=344 done=1
[vm3]   PASSED: 双 vCPU Live Migration 成功（VM2 -> VM3）
[vm3] tick 55
```

结果：

- 源 VM2 回收完成；
- ivshmem peer 和设备模型成功注销；
- KSM 引用正确释放；
- VM3 双 vCPU 继续运行；
- 未出现 FAIL 或 PANIC。
