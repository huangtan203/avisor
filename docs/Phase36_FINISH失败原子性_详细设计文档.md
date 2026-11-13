# Phase 36：FINISH 失败原子性详细设计

## 1. 背景与目标

Phase 33/34 已完成双 vCPU 迁移提交和源 VM 回收。Phase 36 处理 FINISH 在最后一轮脏页复制或目标 vCPU 准备阶段失败时的原子回滚，避免源 vCPU 身份、目标页表和迁移状态出现半提交。

目标：

- 失败返回 `-1`；
- 源 VM vCPU1 的身份、内存空间、设备数据和名称恢复；
- 目标 VM 本轮新增页全部回滚；
- 源 VM AF 脏页跟踪恢复；
- 事务冻结协议释放；
- 迁移回到 `MIGRATION_IDLE`，允许重新 START/PRECOPY/FINISH；
- 成功 FINISH 的提交语义不变。

## 2. 失败窗口

FINISH 的准备阶段顺序为：

1. VM 级事务冻结并确认 vCPU1；
2. 执行最后一轮 `MIG_OP_COLLECT_DIRTY`；
3. 保存 vCPU1 原始身份；
4. 暂时切换 vCPU1 到目标 VM；
5. 注册目标 vCPU；
6. 任何准备错误触发 abort；
7. 通过全部检查后才执行正式提交和源 VM 回收。

目标页表的临时新增页使用 Phase 35 的 migration-new 标记，因此 abort 可精确回收本轮资源，不影响目标 VM 基线映射。

## 3. 回滚算法

失败路径执行：

```text
restore vCPU1 mm/vmid/board_data/name
unregister destination vCPU
register source vCPU
rollback target migration-new pages
restore source Stage-2 AF
commit transaction rollback/release
clear migration source
state = MIGRATION_IDLE
abort_count++
return -1
```

恢复操作必须在迁移状态仍受事务冻结保护时完成，防止源或目标 vCPU 在中间状态重新运行。

## 4. 接口与测试

新增：

- `HVC_MIGRATE_FAIL_FINISH = 69`：注入一次准备阶段失败；
- `HVC_MIGRATE_ABORT_STATS = 70`：查询 abort 次数；
- `migration_fail_finish_once()`；
- `migration_finish_abort_stats()`。

Test 24 验证注入失败、目标页回滚、状态复位、abort 计数增加，以及同一源/目标可以重试并成功迁移。

## 5. 不变量

- abort 不释放迁移开始前已有的目标设备、共享内存和基线页；
- abort 不增加或减少源 VM 用户页、内核页统计；
- 成功路径不执行恢复操作；
- 失败注入为一次性，重试不会继承旧故障。

## 6. 偶发 PANIC 复现记录（t8，未修复）

Test 24 在部分运行中会出现 EL2 PANIC，且全部命中都在 Test 24 `FINISH Abort/Rollback` 附近。本节只记录实测现象，根因未定位，尚不得据此修改行为代码。

### 6.1 已观察到的现场

| 批次 | 插桩 | 轮次 | 命中 | 现场 |
|---|---|---|---|---|
| 早期 | 无 | 20 | 1 | `EL2 task stack exhausted: vmid=1 vcpu=1 free=6216` |
| task.c | 栈全量打印 | 20 | 1 | `SYNC_INVALID_EL2 esr=2000000 elr=41096000 far=40300040` |
| task.c | 栈全量打印 | 60 | 1 | `SYNC_INVALID_EL2 esr=86000006 elr=0 far=0` |
| irq.c | 异常全量打印 | 60 | 1 | `SYNC_INVALID_EL2 esr=2000000 elr=4108a358 far=40300040 cur=0x4108a000 vmid=1 vcpu=1 sp=0x4108bcb0 pt_pc=0x4000aa54` |
| kernel_exit 检查 | `diag_check_elr` | 60 | 4 | 见 6.2 |
| 冻结/增量 pc==0 检查 | `diag_freeze_pc`、`increment_current_pc` | 60 | 0 | 见 6.3 |

早期批次的 `free=6216` 与「`sp - guard_end < 256` 才会触发」的判定条件相互矛盾，未解释。

### 6.2 kernel_exit 检查批次的 4 次命中（复现 60 轮）

注意：4 次命中并非全部由 `kernel_exit` 检查触发。run17 为 `kernel_exit` 检查命中；run2 为栈耗尽判定（非 `kernel_exit`）；run37、run40 为 `SYNC_INVALID_EL2`，`kernel_exit` 检查未命中。

`diag_check_elr` 判据为 `pc` 落在 `[0x40000000, 0x41000000)` 之外即报告。该判据是诊断启发式，不是架构约束。

- run17：`pc=0x0`，`pt=0x4108bec0`。坏值在 `kernel_exit` 读取前已存在于帧中。该帧与 vcpu1 的 `pt_regs=0x4108bef0` 相差 `0x30`，未解释。
- run2：栈耗尽判定，`sp=0x4108bc50`，`guard_end=0x41096478`，但 `tsk=0x4108a000`。按 `tsk + sizeof(task_struct) + 64` 应为 `0x4108a478`，两者不一致，原因未知，该样本不作为可靠证据。
- run37：同一轮先出现 `SYNC_INVALID_EL2 esr=97800004 elr=400ae32c far=0x5b`，随后出现栈耗尽。该批 PANIC 输出约 34456 行，推测为 PANIC 路径经 `exit_task`/`schedule` 重入 `task_stack_check`，属既有行为，未验证。
- run40：`SYNC_INVALID_EL2 esr=86000004 elr=0x1b far=0x1b`。`kernel_exit` 检查未命中，说明存在不经过帧 PC 槽的异常控制流。

### 6.3 冻结恢复与 increment_current_pc 的 pc==0 检查（复现 60 轮，0 次）

在三处冻结恢复点（`smp.c` 的 `freeze-31`、`txn-freeze`、`txn-freeze-all`）与 `increment_current_pc` 写入前检查 `pc == 0`。

- 60 轮全部越过历史命中位置（日志最少 2414 行），结果有效。
- 0 次命中仅说明本样本中这两条路径未出现 `pc == 0`，**不能据此排除**二者。
- 上一批 60 轮中 `kernel_exit` 检查仍命中 4 次，说明 `pc==0` 问题在本批未复现，原因未知。

### 6.4 样本说明

- 表 6.1 中 `kernel_exit 检查` 行对应 `diag_check_elr` 批次（60 份，4 次命中）。
- 另有一批 `diag_check_pt_pc` 版本（判据为 `pt->pc` 落入 `current` 区间，已回退）共 3 次命中：run4 与 run8 为 `SYNC_INVALID_EL2`，`pt->pc` 为合法 guest IPA；run6 为 `DIAG` 误报，`pc=0x4000aa84` 不在任何 task 区间内，且打印的 `cur=0x1` 与其它现场矛盾，该判据不可信。该批仅得到 42 份日志，未核实中断原因。上述结果不计入 6.2。
- 6.6 的 `task_stack_check` 插桩已回退（见 6.7）。其余诊断插桩已于此前全部清理。

### 6.5 未解决问题

1. `pt` 与 `pt_regs` 相差 `0x30` 的帧属于哪个任务，未确认。
2. `elr=0x1b` 的控制流来源，未确认。
3. `guard_end` 与 `tsk` 不一致的原因，未确认。见 6.7：同一次 `task_stack_check` 调用内 `tsk` 与 `guard_end` 来自同一指针，不能用"两个不同 tsk"解释。
4. 栈耗尽与 `SYNC_INVALID_EL2` 是否同源，无证据，不合并结论。

### 6.6 第 3 次诊断批次（t8j，零命中）

- 插桩：`task_stack_check` 的 exhausted 分支打印 `tsk`、`cur`、`guard_end`、`sp`（仅该分支，overflow 分支未改）。构建 `build=0`。
- 复现 60 轮，PANIC 命中 0 次。60 份输出均非空，其中 56 份出现 `Test 24`，其余 4 份未到达 Test 24，原因未查。
- 输出前缀为 `t8j_`。同一轮曾先启动一批 `t8g_` 前缀的复现，其状态查询失效，结果未核实，不计入。
- 本批完成判定依据为 QEMU 进程数为 0 且输出文件完整。
- 结论：零命中，样本量不足以判断 pc 写坏或栈问题是否已消失。历史命中率约 1/20 至 1/60，60 轮零命中并不罕见。

### 6.7 未解决项 3 的代码核对

- `task_stack_guard_end(tsk) = tsk + sizeof(struct task_struct) + TASK_STACK_GUARD_SIZE`。反汇编确认为 `add x0, x0, #0x478`，与 `0x438 + 0x40` 一致。
- 在同一次 `task_stack_check(tsk)` 调用内，`tsk->vmid` 与 `guard_end` 都由同一指针 `tsk` 得出。因此 run2 中打印的 `tsk=0x4108a000` 与 `guard_end=0x41096478` 不一致，不能用"两个不同 tsk"解释。此前对此的表述有误，已更正。
- 按代码，`tsk=0x4108a000` 时 `guard_end` 应为 `0x4108a478`。打印值与之相差 `0xC000`（12 页）。
- 未证实的解释：日志行被其他核的输出交错，打印的各参数并非来自同一次调用。该假设未经验证。
- `free=6216` 与判定条件相矛盾，仍未解释（见 6.2）。
- 验证方法：当前插桩在同一行打印 `tsk`、`cur`、`guard_end`、`sp`，可直接核对 `guard_end == tsk + 0x478`。本批未命中，待下一次命中时检验。
- 该插桩已回退，`task.c` 与 HEAD 一致（`git diff` 为空），构建 `build=0`。

### 6.8 第 4 次诊断批次（t8k，SYNC_INVALID_EL2 打印 x30 与 sp）

- 插桩：`entry.S` 的 `handle_invalid_entry` 从异常帧读取保存的 x30（`[sp, #16*15]`）与异常前 sp（`[sp, #16*15+8]`），传给 `show_invalid_entry_message`，PANIC 输出增加 `x30` 与 `sp`。构建 `build=0`。
- 复现 60 轮，输出前缀 `t8k_`，60 份均非空，QEMU 进程数为 0。其中 59 份日志出现 `Test 24`。
- 命中 1（`t8k_38`，日志 126 KB，`Test 24` 出现 1 次）：`SYNC_INVALID_EL2 esr: 2000000, elr: 41096000, far: 40300040, x30: 41096000, sp: 41097eb0`。`elr` 与 6.1 节早期记录的 `elr=41096000` 相同。
- 命中 2（`t8k_17`，日志 2.9 MB，`Test 24` 出现 0 次）：`task stack exhausted` 刷屏约 45260 次。同一文件中有 5 条 `uncaught exception` 行，因其他核输出交错，部分行被截断。其中一条为 `esr: 96000046, elr: 400c8fb4, far: 40144000, x30: 400c2210, sp: 40143f50`，另有 `elr: 0`、`elr: 600003c9`、`elr: 118994d3` 等。这些行是否来自同一次运行，无法从日志确认。
- 对 `x30 == elr` 的解读：`t8k_38` 中异常时 x30 与 elr 相同，说明异常时 x30 未必保存调用返回地址。该结论仅基于 1 个样本，不能作为第 6.5 节第 2 项的证据。
- 地址归属：`sp` 与 `elr` 落在 `0x40xxxxxx`、`0x41xxxxxx` 区间，与 guest IPA 数值重叠，不能据此判断归属。
- 结论：本批 2 次命中，与历史 1/20 至 1/60 的命中率一致，样本仍然不足。第 6.5 节 4 项未解决问题保持不变。
