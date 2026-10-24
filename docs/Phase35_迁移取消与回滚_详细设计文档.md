# Phase 35：迁移取消与回滚详细设计

## 1. 背景与目标

Phase 25–34 已形成双 vCPU pre-copy 迁移、事务式 FINISH 和源 VM 回收闭环，但迁移一旦进入 `MIGRATION_PRECOPY` 后只能继续执行 FINISH。若运维侧需要主动取消，目标 VM 中已经复制的页面无法释放，源 VM 的 Stage-2 AF 也持续处于脏页跟踪状态。

Phase 35 增加 PRECOPY 阶段取消能力：

- 只允许迁移发起源 VM 取消当前迁移；
- 回收本轮在目标 VM 新分配的页面；
- 保留目标 VM 创建时已有的设备和共享内存映射；
- 恢复源 VM Stage-2 AF；
- 状态回到 `MIGRATION_IDLE`；
- 同一源/目标可以再次 START 并最终 FINISH。

## 2. 状态语义

新增转换：

```text
IDLE --START--> PRECOPY --CANCEL--> IDLE
                     \
                      +--FINISH--> COMPLETED
```

CANCEL 只接受以下条件：

1. 全局状态为 `MIGRATION_PRECOPY`；
2. 调用者等于本轮绑定的 `mig_src`；
3. 源 VM 仍为 `TASK_RUNNING`；
4. 目标 VM 仍为 `TASK_WAITING`。

IDLE、COMPLETED 或错误调用者执行 CANCEL 均返回 `-1`，且不改变状态。

## 3. 目标增量页追踪

### 3.1 设计选择

没有采用固定容量 IPA 数组记录复制页，因为迁移页数随 VM 内存规模增长，静态数组会引入上限和额外内存。

Phase 35 使用 Stage-2 descriptor 的 ignored bit 54：

```c
#define MM_STAGE2_MIGRATION_NEW (1ULL << 54)
```

bit 55 已由 KSM 使用，bit 54 用于标记“本轮迁移在目标中新建的页”。

### 3.2 标记规则

`mig_copy_page()` 分两种情况：

- 目标 IPA 已映射：覆盖已有物理页，不设置临时标记；
- 目标 IPA 未映射：分配新页并以 `MM_STAGE2_MIGRATION_NEW` 建立映射。

后续 PRECOPY 再次覆盖同一页面时，临时标记保留，直到 CANCEL 或 FINISH。

## 4. CANCEL 回滚算法

`mig_finish_target_pages(commit=0)` 遍历目标 VM 的 L1→L2→L3 页表：

1. 找到带 `MM_STAGE2_MIGRATION_NEW` 的 L3 页；
2. 释放对应物理页；
3. 清除 L3 entry；
4. 递减目标 `user_pages_count`；
5. 若本轮删除页面后 L3 表为空，释放 L3 表；
6. 若本轮删除子表后 L2 表为空，释放 L2 表；
7. 保留目标根页表以及 START 前已经存在的空表/基线映射；
8. 刷新 Stage-2 TLB。

返回值为释放的目标数据页数量。

目标 VM 创建阶段已经建立的 MMIO、ivshmem 和设备状态映射没有临时标记，因此不会被取消路径删除。

## 5. FINISH 提交算法

FINISH 在状态转移完成、释放远端 vCPU1 前调用：

```c
mig_finish_target_pages(1);
```

commit 模式不释放页面，只清除所有 `MM_STAGE2_MIGRATION_NEW` 标记，使预拷贝页面成为 VM3 正式映射。随后继续执行 Phase 33/34 的事务提交与源 VM 回收流程。

## 6. 源 VM 恢复

START/PRECOPY 使用 AF 位进行脏页跟踪。取消后调用：

```c
mig_walk(src, MIG_OP_SET_AF);
```

该操作对所有 Normal-memory L2 block 和 L3 page 恢复 `MM_STAGE2_ACCESS`，并刷新 TLB，使源 VM 脱离本轮迁移的 AF 跟踪状态。

最后解除 `mig_src` 绑定并将状态改回 `MIGRATION_IDLE`。

CANCEL 不冻结源 VM；两个 vCPU 在取消前后持续运行。

## 7. PA 掩码修复

测试首次暴露：`ipa_to_pa()` 使用通用 `PAGE_MASK` 提取 Stage-2 descriptor 的 PA。软件 ignored bit 54/55 会被错误保留，导致 pre-copy 覆盖目标页时生成非法物理地址并在 `memcpy` 中触发 EL2 exception。

Phase 35 将 `ipa_to_pa()` 中 L2/L3 table 和最终 page PA 提取统一改为：

```c
entry & MM_STAGE2_PA_MASK
```

该修复同时保证迁移临时位和 KSM bit 55 都不会污染软件地址解析。

## 8. HVC 接口

新增：

```c
#define HVC_MIGRATE_CANCEL 68
```

返回值：

- `>= 0`：成功取消，值为释放的目标增量数据页数；
- `-1`：状态、调用者或目标条件不满足。

接口仅允许 VM2 vCPU0 在 CPU0 调用。

## 9. Guest Test 23

Test 23 在正式迁移前执行：

1. 验证 IDLE 下 CANCEL 返回 `-1`；
2. 对 8 页测试区执行 START；
3. 执行一次 PRECOPY；
4. 调用 CANCEL；
5. 验证释放页数大于 0；
6. 验证源 VM user/kernel 页计数不变；
7. 验证源 vCPU1 的 `hold_counter` 继续推进；
8. 验证状态回到 IDLE；
9. 验证重复 CANCEL 被拒绝；
10. 对同一 VM3 再次 START/PRECOPY/FINISH，并继续执行 Test 21/22。

## 10. 回归结果

双核 QEMU 两轮关键结果一致：

```text
[vm2] --- Test 23: Phase 35 Migration Cancel/Rollback ---
[vm2]   PASS: CANCEL 回滚目标新增页=102，源内存/AF 状态恢复且可重试
INFO[2]: migration: source VM reclaimed (pages=17 shm=0 ksm=345 done=1)
[vm3] --- Test 21: Phase 33 Dual-vCPU Migration ---
[vm3] --- Test 22: Phase 34 Source VM Reclaim ---
[vm3]   reclaim: pages=17 shm=0 ksm=345 done=1
[vm3]   PASSED: 双 vCPU Live Migration 成功（VM2 -> VM3）
[vm3] tick 58
```

结果：

- CANCEL 正确释放 102 个目标增量页；
- 源 VM 内存统计不变，双 vCPU 继续运行；
- 重复 CANCEL 被拒绝；
- 同一目标 VM 可再次迁移；
- Phase 33 双 vCPU 迁移和 Phase 34 源 VM 回收保持通过；
- 未出现 FAIL 或 PANIC。

## 11. 边界

Phase 35 只处理 START/PRECOPY 阶段的主动取消。FINISH 已进入 VM 级冻结及状态转移后仍采用单向提交语义；FINISH 中途错误的全事务补偿属于后续“迁移失败原子性”范围。
