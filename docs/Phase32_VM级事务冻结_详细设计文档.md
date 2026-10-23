# Phase 32：VM 级双 vCPU 事务冻结（VM-wide Stop-the-World）— 详细设计文档

## 1. 概述

### 1.1 背景

Phase 27–31 逐步建立了"冻结 → 快照 → 决策"的事务式冻结能力，但粒度始终是**单 vCPU**：SGI 冻结的是远端核上的 vCPU1，协调核（vCPU0）本身不静止，只在 HVC 边界外观察。对整 VM 做一致性快照（如未来 VM 整体迁移、检查点）时，两个 vCPU 的快照时刻不一致：vCPU1 ACK 到 vCPU0 采样之间，vCPU0 还在 Guest 态执行任意指令，快照窗口内可以观察到"半旧半新"的状态。

Phase 32 交付 **VM 级 stop-the-world 事务冻结**：BEGIN 之后整个 VM 的两个 vCPU 同时静止，形成 VM 级一致快照窗口；COMMIT 全体继续，ROLLBACK 全体回卷——包括协调核自身的通用寄存器与 PC。

### 1.2 核心语义

- **BEGIN（HVC 63）**：vCPU0（协调核）发起。先经 SGI#5 冻结 vCPU1 并等待 ACK（vCPU1 在 EL2 IRQ handler 内忙等保持）；ACK 之后协调核**立即**在当前 HVC 边界捕获自身快照。从 vCPU1 ACK 到协调核快照完成之间，vCPU0 上没有任何 Guest 指令执行（一直停留在 EL2 handler 内），因此两份快照构成 **VM 级一致快照**。
- **HOLD 窗口**：vCPU1 物理核忙等；vCPU0 停留在 BEGIN 的 HVC handler 内。整 VM 静止，协调核内存可正常读写（供快照消费方使用）。
- **COMMIT（HVC 64）**：vCPU1 恢复冻结点异常帧继续执行；vCPU0 从 COMMIT 边界正常 HVC 返回继续执行。窗口内 Guest 已提交的内存写保留。
- **ROLLBACK（HVC 65）**：vCPU1 从冻结点重新执行；**vCPU0 全量回卷**——通用寄存器 x0–x30、SP、PC（回卷到 BEGIN 的 HVC 指令本身）、pstate、EL1 sysregs、vGIC/CNTV 副本。Guest 感知：BEGIN 的 HVC 被重新执行且返回 **1（回放标识）**。
- **STATUS（HVC 66）**：查询 VM 级事务状态（0=无 1=冻结 2=回放）。

### 1.3 目标

- 整 VM 静止窗口内：vCPU1 计数不变 + 协调核内存读写一致。
- ROLLBACK 回卷协调核通用寄存器（局部寄存器变量探针验证）与 PC（BEGIN 重放返回 1）。
- 与 Phase 31 单 vCPU 事务状态机相互独立、互不干扰。
- 保留全部既有回归（Phase 11–31）。

### 1.4 非目标

- **内存回滚**：ROLLBACK 是**寄存器级回卷**——窗口内 Guest 已写内存保留。内存级回滚需要写日志（undo log），留待后续 Phase。
- 不冻结直通设备/DMA（edu、virtio 队列继续工作）。
- 快照不落盘、不跨 VM 传输。

## 2. 协议设计

### 2.1 状态机

```
              BEGIN: SGI#5 冻结 vCPU1 (ACK)
              + 协调核捕获自身快照
VM_TXN_NONE ─────────────────────────────► VM_TXN_FROZEN
   ▲                                          │
   │   COMMIT: vCPU1 恢复, vCPU0 从边界继续   │ ROLLBACK: vCPU1 重执行,
   │◄─────────────────────────────────────────│ vCPU0 回卷到 BEGIN 的 HVC
   │                                          ▼
   │   BEGIN 重放（返回 1）后回 NONE    VM_TXN_REPLAY
   └────────────────────────────────────────────┘
```

关键变量（`hypervisor/common/smp.c`）：

| 变量 | 语义 |
|---|---|
| `vm_txn_state` | VM_TXN_NONE / VM_TXN_FROZEN / VM_TXN_REPLAY（ROLLBACK 后的回放门） |
| `vm_txn_decision` | 0=待决策 1=commit 2=rollback；vCPU1 侧忙等退出条件 |
| `vm_txn_target_cpu` / `vm_txn_target_task` | 被冻结的 vCPU1（-1/NULL 表示无） |
| `vm_txn_coord_snap` | 协调核自身快照（freeze_context 全量副本） |

与 Phase 31 `txn_*` 状态机独立：两套事务都由 VM2/vCPU0 的 HVC 入口发起，HVC 处理是串行的（HOLD 窗口内协调核不返回 Guest），天然互斥，无需跨状态机加锁。

### 2.2 SGI 与 HVC 接口

- SGI `SGI_VM_TXN_FREEZE_ALL = 5`（`include/boards/virt/gicv3.h`）。SGI 0–4 已被通用通知/ping-pong/vGIC 维护/瞬时冻结/单 vCPU 事务占用，5 空闲。
- HVC（`include/common/hvc.h` / `guests/el1_test/guest.h`，两侧保持一致）：
  - `HVC_VM_TXN_BEGIN 63`：返回 0=冻结成功，**1=回放**，-1=失败。
  - `HVC_VM_TXN_COMMIT 64`：返回 0/-1（无事务时 -1）。
  - `HVC_VM_TXN_ROLLBACK 65`：返回 0/-1。
  - `HVC_VM_TXN_STATUS 66`：返回 0/1/2。
- HVC 分发（`sync_exc.c`）沿用 Phase 27/31 guard：`current->vmid == 2 && current->vcpu_id == 0 && smp_processor_id() == 0`，不满足返回 -1/0。

### 2.3 BEGIN 流程（`smp_vm_txn_begin()`）

1. **回放检查**：`vm_txn_state == VM_TXN_REPLAY` 时清回 NONE 并返回 1——这是上次 ROLLBACK 回卷后 BEGIN 的 HVC 被重新执行的落点。
2. 状态与拓扑校验：无进行中 VM 级事务；当前为 VM2/vCPU0/物理核 0；遍历 `cpu_state[]` 定位同 VM 且 `vcpu_id == 1`、TASK_RUNNING 的目标（绑定物理核 1）。
3. 持 `freeze_lock` 置 `VM_TXN_FROZEN`。
4. 清 `vm_txn_decision`、`freeze_ack_generation[1]`、`release_ack_generation[1]` 并 `dmb ish`（复用 Phase 31 的教训：ACK 数组与 Phase 27/31 共享，必须先清零防误判）。
5. 发 SGI#5，自旋等待 `freeze_ack_generation[1] != 0`（每 0x10000 次重发，容忍边沿丢失）。
6. **ACK 到达即捕获协调核自身快照**（`vm_txn_capture_coord_snapshot()`）并返回 0。此刻整 VM 静止。
7. 超时路径：置 `vm_txn_decision=2` 请求 vCPU1 退出、等 release_ack、复位状态返回 -1。

### 2.4 协调核快照捕获（`vm_txn_capture_coord_snapshot()`）

| 数据 | 来源 | 新鲜度 |
|---|---|---|
| cpu_sysregs | `current->cpu_sysregs` | 新鲜——`vm_leaving_work()` 在 `kernel_entry` 宏里已把 EL1 sysregs 存入（见 `sched.c` / `entry.S`） |
| pt_regs x0–x30/sp/pc/pstate | `task_pt_regs(current)` | 新鲜——就是本次异常压的异常帧；`regs->pc` 指向 HVC 之后（HVC 由硬件自动推进 ELR，无需软件 +4） |
| vGIC VMCR/LR | `vgic_save_state()` | 当前硬件实况 |
| CNTV_CTL/CVAL + vtimer_pending | 直接读寄存器 | 当前硬件实况 |

一致性论证：vCPU1 ACK（快照已在其 handler 内完成）→ 协调核观察到 ACK → 捕获自身快照。期间 vCPU0 从未 eret 回 EL1，无 Guest 指令执行；vCPU1 停留在忙等，pt_regs 不再变化。两份快照落在同一个"VM 静止"区间内，构成一致快照。

### 2.5 vCPU1 侧 SGI#5 handler

与 Phase 31 SGI#4 handler 协议相同，仅状态机换成 `vm_txn_*`：

1. 校验（`vm_txn_state == VM_TXN_FROZEN`、cpu/task/VM2/vCPU1/RUNNING 匹配；`vm_txn_decision != 0` 则协调核已放弃，直接返回不 ACK）。
2. 快照捕获到 `target->freeze_context`（cpu_sysregs + virt + vgic_save_state + CNTV + pt_regs 全量）。
3. 置 `freeze_ack_generation[cpu] = 1`（RELEASE）。
4. 忙等 `vm_txn_decision != 0`。
5. 决策后校验 generation/active，恢复保存的 pt_regs（commit=继续 / rollback=重执行，CPU 现场处理相同），置 release_ack。

### 2.6 ROLLBACK 的协调核回卷（`vm_txn_restore_coord_snapshot()`）

这是 Phase 32 的核心新机制，在 ROLLBACK HVC handler 内（即协调核自己的异常帧上）执行：

1. **世界切换序列清场**：调 `board_ops->leaving_vm(current)`——保存并清掉当前 vGIC/vtimer 硬件实况，处理 LR0/LR2 HW 映射的 active 残留（物理 27 / edu INTx 的强制 deactivate）。
2. **换入快照**：`memcpy(&current->virt, &vm_txn_coord_snap.virt)`；`memcpy(&current->cpu_sysregs, ...)`（eret 前 `vm_entering_work` 会生效）。
3. **回卷异常帧**：x0–x30、sp、pstate 从快照恢复；`regs->pc = snap.pc - 4`——指向 BEGIN 的 HVC 指令本身，eret 后重新执行该 HVC。
4. **世界切换序列恢复**：调 `board_ops->entering_vm(current)`——恢复 CNTVOFF、vGIC VMCR/LR、CNTV_CVAL/CTL，vtimer_pending 时补注入。
5. 置 `vm_txn_state = VM_TXN_REPLAY`：重放的 BEGIN 在入口读到该状态返回 1（Guest 的回放感知），随后状态回 NONE。

### 2.7 PC 回卷的确定性

HVC 指令固定 4 字节（AArch64），`regs->pc` 由硬件推进到 HVC 之后，`pc - 4` 即 HVC 本身——不依赖反汇编。重放执行的是**同一个 HVC 实例**（同一条指令），不是重新构造的调用，x0/x8 参数与首次执行时相同（寄存器已回卷）。

## 3. 关键实现细节

### 3.1 为什么 vCPU0 用 HVC 边界而不是 SGI

- 协调核就在发起 HVC 的核上：自陷即同步点，无需自发自收 SGI。
- HVC 边界处 `cpu_sysregs` 已被 `vm_leaving_work` 刷新、`pt_regs` 就是异常帧——快照数据结构现成。
- HVC handler 内 DAIF.I=1：无嵌套中断、无抢占，回卷异常帧不会被打断。

### 3.2 x20 探针（Guest 侧验证寄存器回卷）

测试用局部寄存器变量约束探针驻留 x20：

```c
register u64 x20_probe __asm__("x20");
x20_probe = probe_a;                       /* 冻结前 */
asm volatile("" : "+r"(x20_probe) : : "memory");  /* 锚定，防优化 */
ret2 = hvc_call(HVC_VM_TXN_BEGIN, 0);      /* HVC 边界捕获 */
x20_probe = probe_b;                       /* 窗口内改写 */
hvc_call(HVC_VM_TXN_ROLLBACK, 0);          /* 回卷 → x20 恢复 probe_a */
```

局部寄存器变量是 GCC 官方支持的机制（编译器知情：函数序言自动保存/恢复 x20，寄存器分配不会挪用），比裸 `asm("mov x20, ...")` 写法安全。回卷后 `x20_probe == probe_a` 证明 freeze_context.regs[] 的恢复经 eret 正确落到硬件寄存器。

注意：`hvc_call` 内联 asm 使用的 x0/x8 是 caller-saved，回卷后它们也被恢复为 BEGIN 时刻的值——这正是重放语义，Guest 在 BEGIN 重放处用返回值（x0=1）区分轮次。

### 3.3 与 Phase 30 快照恢复结论的一致性

Phase 30 发现：vGIC LR 的 pending→invalid 漂移与 vtimer active 状态导致"直接把快照写回硬件"不可靠，需要走 leaving→换入→entering 的世界切换序列。Phase 32 的协调核回卷复用该结论（见 2.6 步骤 1/4）。vCPU1 侧恢复的只是 pt_regs（vCPU1 在忙等期间硬件现场未动），与 Phase 31 语义相同。

### 3.4 调试记录：陈旧嵌入镜像

首版验证时 Test 20 完全未执行（连测试头都不打印）：`el1_test.bin` 含 "Test 20" 字符串但 `kernel8.img` 不含。根因是第一次构建因 guest 编译错误（`frozen_at` 重声明）中断，guest 未产出新 bin；第二次 make 时 `guest_image_s.o` 的时间戳早于 make 启动时刻，被判定无需重建，链接了陈旧镜像。删除 `build_virt/hypervisor/common/guest_image_s.o` 强制重嵌后恢复。这是 Makefile 并行 `$(GUEST_BIN): guest` 伪目标依赖与 `.incbin` 规则时序的固有缺陷（子构建失败后恢复时缺少对嵌入对象的时间戳强制），对后续所有修改 guest 镜像的 Phase 都适用：**构建失败后重试，必须强制重建 guest_image_s.o 或先 clean**。

## 4. Guest 侧测试（Test 20）

- 共享内存偏移（`guest.h`）：`P32_COORD_SCRATCH_OFF 0x50`（协调核冻结窗口内写的草稿值）、`P32_REPLAY_COUNT_OFF 0x58`（BEGIN 重放次数计数）。复用 Phase 31 的 `P31_HOLD_COUNTER_OFF 0x40` 观测 vCPU1。
- 测试嵌套结构与 Test 18/19 相同：`guest_vmid()==2` → Test 18（100 轮瞬时冻结）→ 迁移拒绝边界 → Test 19（单 vCPU 事务）→ **Test 20（VM 级事务）**。
- 轮次 1（COMMIT）：
  1. BEGIN 返回 0、STATUS=1。
  2. 冻结窗口内写 scratch=0xAAAA1111、忙等 200000 nop、采样 vCPU1 计数——必须与冻结时相同（整 VM 静止）。
  3. scratch 读回一致（窗口内内存可用）。
  4. COMMIT 后 vCPU1 计数恢复推进，scratch 保留（提交语义）。
- 轮次 2（ROLLBACK + 重放）：
  1. 冻结前 `x20_probe = probe_a`；BEGIN 返回 0（若返回 1 则 goto 重放路径——首次执行不触发）。
  2. 窗口内 `x20_probe = probe_b`、scratch=0xBBBB2222，验证 vCPU1 静止。
  3. ROLLBACK 返回 0 后，**回卷使控制流回到 BEGIN 的 HVC**——"ROLLBACK 失败"之后的代码不可达（若执行到说明回卷失效）。
  4. 重放路径（`p32_replayed` 标签）：BEGIN 返回 1；`x20_probe == probe_a`（寄存器回卷）；`replay_count +1`；scratch **保留 0xBBBB2222**（寄存器级回卷语义：内存写不回滚）；vCPU1 计数恢复推进。
- 边界：无事务时 COMMIT/ROLLBACK 返回 -1，STATUS 回 0。

## 5. 验证矩阵

| 场景 | 验证点 | 结果 |
|---|---|---|
| VM 静止 | 冻结窗口内 vCPU1 计数两次采样相同 | PASS |
| 窗口内存 | 协调核窗口内写/读 scratch 一致 | PASS |
| COMMIT（vCPU1） | 计数从冻结值恢复单调递增 | PASS |
| COMMIT（vCPU0） | scratch 保留（提交） | PASS |
| 第二轮静止 | 同上 | PASS |
| ROLLBACK（vCPU0 寄存器） | x20 探针恢复冻结前值 | PASS |
| ROLLBACK（vCPU0 PC） | BEGIN 重放返回 1、replay_count +1、不可达代码未执行 | PASS |
| ROLLBACK（内存语义） | scratch 保留窗口值（不回滚） | PASS |
| ROLLBACK（vCPU1） | 计数恢复推进 | PASS |
| 边界拒绝 | 无事务 COMMIT/ROLLBACK=-1、STATUS=0 | PASS |
| 回归 | Test 18（100 轮瞬时冻结）、Test 19（单 vCPU事务）、Phase 11–30 全部 | PASS |

## 6. 风险与边界

- **HOLD 窗口双核占用**：vCPU1 物理核忙等 + vCPU0 停在 HVC handler，两核都不响应调度；窗口必须由 COMMIT/ROLLBACK 收敛（等待循环有超时兜底，超时路径会请求 vCPU1 退出并复位状态）。
- **REPLAY 状态泄漏**：若 ROLLBACK 后 Guest 异常地不再执行 BEGIN（例如重放路径上崩溃），`vm_txn_state` 停留在 REPLAY，后续 BEGIN 会误返回 1。当前测试流程保证重放即接 BEGIN；生产化需加代际计数或超时自愈。
- **重放的副作用**：BEGIN 到 ROLLBACK 之间 Guest 已产生的内存副作用（如 UART 输出、对其他共享页的写）不会消失——这是寄存器级回滚的固有语义，Guest 编写事务体时须自律（事务体内只做幂等或可丢弃操作）。
- **vtimer 补注入**：回卷走 entering_vm，若冻结瞬间 vtimer 已到期的 active 状态被快照记录，重放后会补注入一次 vIRQ 27——与正常世界切换行为一致，不构成额外风险。
- **单 VM 单槽位**：VM 级事务全局一个槽位，只支持 VM2 双 vCPU 拓扑；多 VM 并发事务需要 per-VM 状态表。

## 7. 实测结果

双核 QEMU（`virt,virtualization=on,gic-version=3,highmem=off -cpu max -smp 2 -m 1G -device edu`），两轮独立运行均 **71 PASS / 0 FAIL**：

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
[vm2] --- Test 20: Phase 32 VM-wide Transactional Freeze ---
[vm2]   PASS: 冻结期间整 VM 静止（vCPU1 计数不变）
[vm2]   PASS: COMMIT 后 vCPU1 恢复执行，协调核改动保留
[vm2]   PASS: 第二轮冻结期间整 VM 静止
[vm2]   PASS: ROLLBACK 回卷协调核现场（x20 探针恢复）
[vm2]   PASS: BEGIN 重放返回 1，重放计数 +1
[vm2]   PASS: 窗口内内存写保留（寄存器级回卷语义）
[vm2]   PASS: ROLLBACK 后 vCPU1 从冻结点恢复执行
[vm2]   PASS: 无 VM 事务时决策被拒绝，状态回 NONE
[vm2]
[vm2] guest: all tests done, entering main loop
```

VM1 在全部测试后进入主循环；测试结束后双核 tick 持续推进至 239（Hypervisor alive tick 11800），236 次 alive 心跳，无 FAIL/FATAL/PANIC。QEMU 由 120 秒外部 timeout 结束（不作为自然退出记录）。

## 8. 后续边界

- **内存级回滚**：HOLD 窗口内的 Guest 写进入 undo log，ROLLBACK 时回放逆操作——配合寄存器回卷构成完整事务语义。
- **HOLD 窗口内迁移/检查点**：VM 级一致快照窗口已具备，可把 Phase 25 迁移原型的 stop-and-copy 阶段迁入本窗口（整 VM 静止代替单 vCPU 冻结）。
- **设备/DMA 静止**：edu 直通与 virtio 队列在窗口内仍活跃，迁移前需排空。
- **多 VM 并发事务**：`vm_txn_*` 扩展为 per-VM 数组，BEGIN 按 `current->vmid` 选槽。
- **超时自愈**：REPLAY 状态泄漏检测（下一轮 BEGIN 前检查时间戳）。
