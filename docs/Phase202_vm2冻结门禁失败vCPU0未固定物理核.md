# vm2 冻结门禁失败：vCPU0 未固定物理核的实现缺口

## 1. 现象

vm2 Test 18（SMP 冻结握手）在部分轮次输出：

```
[vm2]   FAIL: 冻结请求失败，轮次 0
```

30 轮中出现 4–5 次。失败位于第 0 轮，即第一次 `HVC_SMP_FREEZE_BEGIN` 即被拒绝。

## 2. 根因（实测）

在 `hypervisor/arch/aarch64/sync_exc.c` 的 `HVC_SMP_FREEZE_BEGIN` 分派处临时插桩，记录条件不成立时的取值。30 轮中 4 次失败，**4/4 均为**：

```
[FZDBG2] vmid=2 vcpu=0 cpu=1
```

即失败时 vm2 vCPU0 运行在物理 CPU1 上。分派条件要求 `vmid==2 && vcpu_id==0 && smp_processor_id()==0`，第三项不满足，HVC 直接返回 -1，`smp_freeze_begin` 未被调用（同批插桩中 `smp_freeze_begin` 三处返回点计数均为 0，佐证这一点）。

## 3. 设计要求与实现的差距

- 设计要求：冻结 HVC 的发起方必须是 VM2 vCPU0，且运行在物理 CPU0。见 `docs/Phase37_SMP冻结通道修复_详细设计文档.md` 第 36 行、`docs/Phase32_VM级事务冻结_详细设计文档.md` 第 72 行。
- 实现：`hypervisor/boards/virt/main_virt.c` 中 vm2 vCPU1 设置了 `TASK_PINNED`（约第 348 行，注释说明「被负载均衡器迁走后即失效」），**vCPU0 未设置**（约第 308–329 行，创建处仅设置优先级与名称）。

## 4. 迁移路径（代码已读，运行时是否发生未逐次确认）

1. `hypervisor/common/sched.c` 的 `sched_balance_tick()`：CPU0 本地负载 ≥75%、远端负载低 25% 以上、且 `current` 未 pinned，则把 `current` 标为迁移目标。
2. `sched_balance_safe_point()`：取出目标并调用 `sched_migrate_current()`。
3. `sched_migrate_current()`：把 task 从 `source_cpu` 搬到 `target_cpu`，并更新 `tsk->phys_cpu`。

vm2 vCPU0 未 pinned，满足第 1 步条件时即可能被迁到 CPU1，与第 2 节的实测现象一致。

## 5. 修复（已实施）

`hypervisor/boards/virt/main_virt.c` 中 vm2 vCPU0 创建后增加 `flags |= TASK_PINNED`，与 vCPU1 对称。负载均衡 `sched_balance_tick()` 对 `TASK_PINNED` 的 task 不设迁移目标，`sched_migrate_current()` 也会直接拒绝，因此 vCPU0 不再被迁到 CPU1。

## 6. 验证

A/B 对照，每组 30 轮，同一构建方式，仅 pin 开关不同。

| 指标 | 无 pin（基线） | 有 pin（第 1 批） | 有 pin（复测） |
|---|---|---|---|
| 轮数 | 30 | 30 | 30 |
| `冻结请求失败` | 1 | 0 | 0 |
| `Test 18` 握手完成 | 29/30 | 30/30 | 30/30 |
| `stack exhausted` PANIC | 0 | 3 | 0 |
| `SYNC_INVALID_EL2` PANIC | 2 | 1 | 0 |

冻结失败的修复方向与前面的实测证据一致：插桩中 4/4 失败时 `vmid=2 vcpu=0 cpu=1`，pin 后两批共 60 轮未再出现该失败。

**未定性的 PANIC**：第 1 批出现的 3 次 `EL2 task stack exhausted`（vmid=1/2 的 vcpu1）在复测中 0/30 出现，且基线（无 pin）也为 0。该类 PANIC 可能是偶发，也可能与 pin 有关，目前样本不足以区分，**未定性**。`SYNC_INVALID_EL2` 同理，基线 2 次、pin 后 1 次，未见规律。

### 6.1 有 pin 版本的补充轮次

有 pin 版本共 150 轮（5 批 × 30 轮），每批统计 `FAIL:`、`PANIC` 与 `握手完成 100 轮`。

| 批次 | Test 18 PASS | 冻结类 FAIL | 回收类 FAIL | PANIC |
|---|---|---|---|---|
| 1 | 30/30 | 0 | 0 | 4（stack exhausted 3、SYNC_INVALID 1） |
| 2（复测） | 30/30 | 0 | 0 | 0 |
| 3 | 30/30 | 0 | 0 | 1（SYNC_INVALID，run4） |
| 4 | 30/30 | 0 | 3 行，run6 | 0 |
| 5 | 30/30 | 0 | 3 行，run16 | 1（SYNC_INVALID，run16） |

合计：Test 18 冻结 150/150 通过；冻结类 FAIL 0；回收类 FAIL 2 轮（run6、run16，各含「zombie 未回收」「槽位未复用」，run16 另含「FINISH abort 未回滚到 IDLE」）；PANIC 6 次（stack exhausted 3、SYNC_INVALID 3）。

回收类 FAIL 与 Phase201 观察到的 vm2 回收类失败症状相似。两者是否同一根因未验证。pin 对它是否有影响，本批数据也无法判断。

## 7. 未验证的部分

- 「冻结失败由 vCPU0 迁移引起」的因果链由实测（失败时 `cpu=1`）与代码路径共同支持，但未直接对迁移事件本身插桩计数。
- `stack exhausted` 与 pin 的关系未定性，需要更多轮次或专门插桩才能判断。
- 本次只验证了 vm2 Test 18 的冻结路径，未全面回归其它测试。

## 8. 状态

- 插桩已全部还原（`hypervisor/common/smp.c`、`hypervisor/arch/aarch64/sync_exc.c` 相对 HEAD 无差异）。
- `main_virt.c` 为加 pin 的版本，构建 `build=0`。
- 未提交，未推送。
