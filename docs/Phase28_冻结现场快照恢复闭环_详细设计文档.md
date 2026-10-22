# Phase 28：冻结现场快照恢复闭环 — 详细设计文档

## 1. 概述

### 1.1 背景

Phase 27 建立了 VM2 双 vCPU 的受限 SGI 冻结握手，并将目标 vCPU 的 `pt_regs` 复制到 task 专属 `freeze_context`。但 release 后现有代码只是清除快照标志，并依赖原 IRQ 栈帧恢复 Guest；因此还没有验证独立快照本身可以承担恢复职责。

AArch64 IRQ 入口在 `entry.S` 的 `kernel_entry` 保存 x0-x30、SP、ELR 和 SPSR 到当前 task 栈上的 `pt_regs`；`kernel_exit` 从该帧恢复寄存器并执行 ERET。Phase 28 利用这个既有返回路径，在释放冻结现场时将快照回填到 IRQ 返回帧。

### 1.2 目标

- release 前校验冻结上下文有效且 generation 与当前请求一致。
- release 时从 `freeze_context` 回填 x0-x30、SP、PC、PSTATE 到 IRQ 异常帧。
- 只有回填完成后才清除上下文有效标记并发布 release ACK。
- 在 QEMU 中重复验证冻结、释放和 Guest 执行连续性。
- 保持 Phase 26 对双 vCPU 热迁移的拒绝行为。

### 1.3 非目标

- 不将冻结屏障接入 `migration_start()` 或 `migration_finish()`。
- 不支持通用 vCPU 集合、多个 VM 并发冻结或目标 vCPU 拓扑重建。
- 不迁移 vGIC、定时器、DMA、设备或共享内存状态。
- 不宣称支持双 vCPU 整体热迁移。

## 2. 现场布局与所有权

`struct pt_regs` 包含 31 个通用寄存器及 SP、PC、PSTATE，大小为 272 字节，与 `S_FRAME_SIZE` 一致。冻结 SGI handler 运行在 IRQ 上下文：

1. IRQ 入口先通过 `kernel_entry` 将现场保存到目标 task 的栈帧。
2. handler 将该帧复制到 `task->freeze_context`，并写入当前 generation 和 active 标志。
3. handler 在 EL2 等待 release generation，不调用调度器。
4. release 后，handler 将快照回填至同一 IRQ 栈帧。
5. handler 清除快照 active 标志并发布 release ACK。
6. `kernel_exit` 从回填后的异常帧恢复寄存器并 ERET。

异常栈帧仍是最终 ERET 的来源；独立快照现在负责释放时的恢复内容。冻结等待期间目标 task 不会被调度或执行 Guest 指令。

## 3. generation 与失败语义

- 快照 generation 必须等于活动冻结 generation，否则不得回填或发布成功 ACK。
- 只有目标核完成完整回填后才发布 release ACK。
- 无活动请求、未收到 ACK、重复 release 或目标身份不匹配均返回失败。
- 超时且未取得远端释放确认时保留活动状态，拒绝覆盖新请求。
- 超时及迟到 ACK 的故障注入不属于本阶段必测项，继续列为剩余验证工作。

## 4. Guest 验证

沿用 VM2 的测试专用 HVC：

- 执行至少 100 轮 begin / ACK / release。
- 每轮验证 ACK generation 非零且 progress 增加一次。
- 验证 release 返回成功后 Guest 仍继续执行，并验证无活动冻结时重复 release 返回失败。
- 确认 Phase 22–26 回归通过，尤其双 vCPU 迁移仍被拒绝。

本测试只能证明恢复后执行连续及握手状态一致，不能完全断言所有 GPR、EL1 系统寄存器、虚拟中断与定时器状态在任意负载下等价。

## 5. 文件范围

| 文件 | 变更 |
|---|---|
| `hypervisor/common/smp.c` | generation 检查及将快照回填到 IRQ 返回帧 |
| `guests/el1_test/main.c` | 继续使用 100 轮握手和无活动 release 检查 |
| 本文档 | 记录现场模型、边界和验证结果 |

## 6. 验收标准

1. AArch64 Guest 与 Hypervisor 构建通过。
2. QEMU 双核中 100 轮冻结/释放完成，无死锁。
3. 每轮 release 后目标 Guest 可继续运行，ACK/progress 符合预期。
4. 无活动冻结的重复 release 被拒绝。
5. Phase 22–26 回归通过，双 vCPU START 仍被拒绝。

## 7. 阶段边界与后续

Phase 28 完成快照到异常返回帧的闭环验证，但仍是固定 VM2 双核原型。接入迁移前还需设计 VM 级参与者集合、完整 per-vCPU 虚拟化状态快照、并发设备/DMA 一致性、冻结超时故障注入与回滚，并验证所有状态在目标拓扑恢复。Phase 26 的迁移拒绝逻辑必须保持不变。

## 8. 实测结果

AArch64 Guest 与 Hypervisor 均构建成功。双核 QEMU 回归中，Guest VM2 的 100 轮冻结/释放通过，无活动冻结时重复 release 被拒绝；Phase 26 非法 PRECOPY 与双 vCPU START 拒绝通过。Phase 22 大页/配额、Phase 23 页面回收、Phase 24 KSM 测试在 VM1 与 VM2 均通过。释放后两个物理核的 Hypervisor tick 和 Guest tick 继续推进。QEMU 由 240 秒外部 timeout 结束，测试日志没有显示异常退出；因此不将其描述为自然退出。
