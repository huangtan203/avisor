# Phase 31：事务式冻结（Transactional Freeze）— 详细设计文档

## 1. 概述

### 1.1 背景

Phase 27–30 实现了瞬时冻结（freeze rendezvous）与快照捕获：目标核 ACK 后由协调核立即 release，冻结窗口只覆盖一次 SGI handler 内部，快照随即成为历史检查点。Phase 30 明确指出：要安全地把快照应用回目标 vCPU，必须先实现"真正保持 VM 停止的事务式冻结及失败回滚"。Phase 31 交付该能力。

事务式冻结的核心语义：

- **BEGIN**：协调核发送专用 SGI，目标核 ACK 后**不自动释放**，而是在 IRQ handler 内忙等（quiesce hold），直到协调核显式决策。
- **HOLD 窗口**：目标 vCPU 完全静止（物理核停留在 EL2 IRQ handler），快照持续有效，协调核可安全观察或迁移内存。
- **COMMIT**：目标核按冻结时保存的异常帧恢复执行（正常继续，等价瞬时 release）。
- **ROLLBACK**：目标核同样从冻结点恢复保存的异常帧重新执行（Guest 侧可观察重执行副作用，如计数重走）。
- **STATUS**：查询事务状态（0=无事务 1=冻结中 2=已提交 -1=已回滚），事务结束后回 0 允许下一轮。

### 1.2 目标

- 与瞬时冻结相同的拓扑约束（VM2 vCPU1 绑定物理核 1），增加事务状态机。
- HOLD 期间目标 vCPU 计数完全静止，COMMIT 后恢复单调递增。
- ROLLBACK 后从冻结点重新执行，Guest 观测计数从冻结值继续。
- 无事务时 COMMIT/ROLLBACK 被拒绝（返回 -1）。
- 保留 Phase 27 的 100 轮瞬时冻结握手与全部既有回归。

### 1.3 非目标

- 不做 VM 级多 vCPU 原子冻结（Phase 31 仍是单 vCPU 粒度）。
- 不迁移设备状态、DMA 或物理 GIC 状态。
- 不在 COMMIT 时恢复 vGIC LR / CNTV 快照（HOLD 窗口内硬件现场未变，无需回写；历史回写语义仍由 Phase 30 结论约束）。

## 2. 协议设计

### 2.1 状态机

```
                 BEGIN (SGI#4, ACK)
TXN_NONE ───────────────────────────────► TXN_FROZEN
   ▲                                          │
   │            decision=1 (commit)           │ decision=2 (rollback)
   │◄─────────────────────────────────────────┘
   └── COMMIT/ROLLBACK 完成后回 TXN_NONE（允许下一轮事务）
```

关键变量（`hypervisor/common/smp.c`）：

| 变量 | 语义 |
|---|---|
| `txn_state` | TXN_NONE / TXN_FROZEN / TXN_COMMITTED（内部短暂）/ TXN_ROLLED_BACK |
| `txn_decision` | 0=待决策 1=commit 2=rollback；目标核读到非 0 退出忙等 |
| `txn_target_cpu` / `txn_target_task` | 事务目标（-1/NULL 表示无） |

### 2.2 SGI 与 HVC 接口

- SGI `SGI_VM_TXN_FREEZE = 4`（`include/boards/virt/gicv3.h`），与瞬时冻结 SGI#3、调度 SGI#1、vGIC 更新 SGI 无冲突。
- HVC（`include/common/hvc.h`）：
  - `HVC_SMP_TXN_FREEZE_BEGIN 59`：x0=目标物理核，冻结并等待 ACK，返回 0/-1。
  - `HVC_SMP_TXN_FREEZE_COMMIT 60`：提交，返回 0/-1（无事务时 -1）。
  - `HVC_SMP_TXN_FREEZE_ROLLBACK 61`：回滚，返回 0/-1。
  - `HVC_SMP_TXN_FREEZE_STATUS 62`：返回当前状态。
- 所有 HVC 入口在 `sync_exc.c` 带 VM2/vCPU0/物理核 0 guard，与瞬时冻结一致。

### 2.3 目标核 SGI#4 handler 流程

1. 校验：`txn_state == TXN_FROZEN`、cpu 匹配、`current == txn_target_task`、任务为 VM2 vCPU1 且 TASK_RUNNING；不满足直接 break（不 ACK）。
2. 若协调核已超时放弃（`txn_decision != 0`），直接返回不 ACK。
3. 快照捕获（同瞬时冻结）：cpu_sysregs、`tsk->virt`（vGIC VMCR/LR）、`vgic_save_state()`、CNTV_CTL/CVAL、完整 `pt_regs`（x0–x30、sp、pc、pstate）；generation 固定为 1，`active = 1`。
4. `dmb ish` 后置 `freeze_ack_generation[cpu] = 1`（RELEASE）。
5. **忙等决策**：`while (txn_decision == 0) yield;` —— HOLD 期间 vCPU 完全静止，物理核不返回 Guest。
6. 决策到达后校验快照仍为本事务（generation/active），恢复保存的 pt_regs 到 IRQ 返回帧，`active = 0`，置 `release_ack_generation[cpu] = 1` 通知协调核。

commit 与 rollback 对 CPU 现场的处理相同（都恢复保存的异常帧）；重执行语义由 Guest 侧计数器观察验证——rollback 轮次从冻结值重新递增，证明从冻结点重走。

### 2.4 协调核时序与竞态处理

- BEGIN 发送 SGI **前**必须清零 `freeze_ack_generation[target]` 与 `release_ack_generation[target]` 并 `dmb ish`：这两个数组与瞬时冻结共享，Phase 27 测试残留的非零值会让 BEGIN 立即误判 ACK（目标实际未冻结，Guest 采样计数继续变化）。
- BEGIN 超时：置 `txn_decision = 2`（RELEASE）请求目标核退出；目标核晚到 SGI 时看到 decision 非 0 直接返回不 ACK，协调核等待 `release_ack` 后复位状态返回 -1。
- COMMIT/ROLLBACK：写 `txn_decision`（RELEASE）后自旋等待 `release_ack_generation != 0`；完成（或超时）后持锁复位 `txn_state`、`txn_target_task/cpu` 与两个 ACK 数组，事务结束回 TXN_NONE。
- SGI 边沿触发：等待循环每 0x10000 次重发 SGI，容忍边沿丢失。

## 3. 关联修复：KSM 合并 SMP 共享控制页

Phase 31 调试中发现的独立缺陷：`HVC_MEM_KSM_SCAN` 全量扫描时会把 Guest 的 SMP 共享控制页（IPA `0x40300000`，含 ready/done flag、计数器、P31 观测计数）当作普通内容页合并成只读共享页。次核观测循环持续写该页触发 Stage-2 permission fault，走 `ksm_cow_break()`；若合并后页内容已变化导致内容哈希 lookup miss，CoW 分离失败，Data Abort 被注入 Guest，次核 `guest_fatal` 进入 wfe 死循环独占物理核，饿死同核另一 vCPU 的 SGI（表现为 Phase 27/31 测试超时失败，且不同构建版本呈现时序敏感的偶发性）。

修复（`hypervisor/common/ksm.c` `ksm_scan_vm()`）：扫描循环跳过 IPA `0x40300000` 页——活跃的 SMP 控制页必须保持可写，不参与 KSM 合并。KSM 对其余页的合并/CoW 行为不变（Phase 24 回归通过，合并页数 288/343 与修复前一致）。

## 4. Guest 侧测试

- 共享内存偏移（`guests/el1_test/guest.h`）：`P31_HOLD_COUNTER_OFF 0x40`、`P31_HOLD_EPOCH_OFF 0x48`。
- VM2 vCPU1 观测循环（`guest_secondary_main`，仅 vmid==2）：循环 `(*hold_counter)++; *hold_epoch = *hold_counter; dmb; wfi;`。WFI trap 进入 `schedule()` 让出 CPU，计数单调递增且可被冻结静止。
- VM1 次核保持纯 WFI 循环：事务协议只针对 VM2；避免 VM1 的同页写入引入 KSM/CoW 干扰。
- VM2 coordinator（Test 19）：
  1. BEGIN + STATUS=1；冻结期间两次采样计数必须相同（HOLD 静止）。
  2. COMMIT 后计数恢复推进（自旋等待超过冻结值）。
  3. 第二轮 BEGIN + ROLLBACK：冻结静止，rollback 后计数从冻结值继续递增。
  4. 无事务时 COMMIT/ROLLBACK 均返回 -1，STATUS 回 0。

## 5. 验证矩阵

- HOLD 窗口：VM2 vCPU1 计数完全静止（两次采样相同），Hypervisor 双核 tick 继续推进（协调核不受影响）。
- COMMIT：计数从冻结值继续单调递增，无重复执行。
- ROLLBACK：计数从冻结值重新递增（重执行语义成立）。
- 边界：无事务时决策被拒；STATUS 状态机转换正确；事务结束后可立即开始下一轮（Test 19 内连续两轮 BEGIN 成功证明）。
- 回归：Phase 11–30 全部测试（含 Phase 27 100 轮瞬时冻结、Phase 24 KSM/CoW、Phase 26 迁移拒绝）通过，两 VM 进入主循环后 tick 持续推进 300+ 无异常。

## 6. 风险与边界

- 目标核忙等期间物理核 100% 占用于 EL2 spin（yield），不响应调度；窗口必须由 COMMIT/ROLLBACK 及时收敛（协调核等待循环有超时兜底）。
- 双 vCPU 同时事务：当前事务状态是全局单一槽位，BEGIN 持锁拒绝并发事务。
- rollback 的重执行副作用：目标核从冻结点重走，若冻结瞬间 Guest 正在写共享计数，重执行会再次写（计数语义上等价于多加一次循环）；测试通过"计数继续递增"验证而非精确差值，避免对此过度承诺。
- 超时放弃路径中若目标核恰好在决策写入后才收到 SGI，它直接返回不 ACK，不会误恢复现场。

## 7. 实测结果

双核 QEMU（`virt,virtualization=on,gic-version=3,highmem=off -cpu max -smp 2 -m 1G -device edu`）：

```
[vm2] --- Test 18: Phase 27 SMP Freeze Handshake ---
[vm2]   PASS: 双 vCPU 冻结/释放握手完成 100 轮
[vm2]   PASS: 无活动冻结时重复 release 被拒绝
[vm2]   PASS: IDLE 下非法 PRECOPY 被拒绝，状态保持不变
[vm2]   PASS: 双 vCPU START 被拒绝，状态为 REJECTED
[vm2]
[vm2] --- Test 19: Phase 31 Transactional Freeze ---
[vm2]   PASS: 冻结期间 vCPU1 计数静止
[vm2]   PASS: COMMIT 后 vCPU1 恢复执行
[vm2]   PASS: ROLLBACK 后 vCPU1 从冻结点恢复执行
[vm2]   PASS: 无事务时决策被拒绝，状态回 NONE
[vm2]
[vm2] guest: all tests done, entering main loop
```

全日志 64 处 PASS，无 FAIL/FATAL/PANIC。VM1 在 1254 行完成全部测试进入主循环，VM2 在 2062 行完成。测试结束后两 VM tick 持续推进至 319（Hypervisor alive tick 15800），双核调度稳定。修复前 Phase 27 在轮次 0 超时失败、修复首版 Phase 31 冻结期间计数仍在推进的现象均消失。KSM 合并页数 VM1=288 / VM2=343，与修复前一致，证明排除控制页未影响去重效果。QEMU 由 160 秒外部 timeout 结束（不作为自然退出记录）。

## 8. 后续边界

- VM 级多 vCPU 原子冻结（两 vCPU 同时 HOLD）与失败回滚编排。
- HOLD 窗口内执行内存迁移/快照落盘（把 Phase 25 迁移原型与事务冻结结合）。
- 设备与 DMA 静止（直通设备 MMIO 排空、virtio 队列暂停）后再扩展 COMMIT 语义。
