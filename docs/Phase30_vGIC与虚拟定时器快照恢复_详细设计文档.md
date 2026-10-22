# Phase 30：vGIC 与虚拟定时器快照恢复 — 详细设计文档

## 1. 概述

### 1.1 背景

Phase 29 在双 vCPU freeze rendezvous 中保存每个目标 vCPU 的 EL1 system registers、vGIC VMCR/LR、CNTV_CTL/CVAL 和通用异常现场，并通过 generation 查询确认快照有效。当前 release 仅恢复通用 IRQ 返回帧，vGIC/定时器快照未应用。直接在仍处于目标 IRQ handler 时写回 vGIC LR 或 CNTV，可能与当前中断的 EOI/Deactivate、Guest 重新进入及定时器过期竞争。

### 1.2 目标

- 验证 freeze SGI 快照所处的 IRQ EOI/Deactivate 阶段，以及 vGIC/定时器快照各字段的含义。
- 增加只读一致性观测和 Guest 回归，证明捕获快照不会改变目标 vCPU 的后续中断与定时器行为。
- 校验 generation，拒绝查询过期或无效快照。
- 保留并验证既有 guest IRQ、虚拟定时器、跨核注入和 100 轮 freeze 测试。

Phase 30 不将冻结快照回写到正在继续运行的 vCPU。Phase 28 release 后目标 Guest 会继续执行，冻结快照随即成为历史检查点；后续 IRQ、定时器或设备活动会推进实际状态。把历史快照覆盖到活动 vCPU 会回滚其执行状态。

### 1.3 非目标

- 不启用 `migration_start()` 的双 vCPU 迁移。
- 不迁移物理 GIC distributor/redistributor 状态、设备 DMA 或共享设备状态。
- 不恢复其它 VM 的状态，不修改 GIC active/deactivate 全局语义。
- 不声称支持跨物理机迁移。

## 2. 当前路径与恢复安全边界

1. 目标 Guest 进入 SGI IRQ，`vm_leaving_work()` 保存 EL1 system registers。
2. freeze handler 捕获 vGIC 与 CNTV 软件快照并 ACK；目标物理 vCPU 停留在 IRQ handler，硬件接口现场不变。
3. coordinator 请求 release；freeze handler 回填通用异常帧并返回。
4. `handle_irq()` 对 SGI 执行 `ICC_EOIR1_EL1` 与 `ICC_DIR_EL1`，随后 `kernel_exit` 调用 `vm_entering_work()`。
5. 当前 vCPU 的 `virt_entering_vm()` 恢复 task 正常运行态 `tsk->virt` 与 CNTV；freeze_context 是仅供检查的历史快照，不得覆盖运行态。
6. 后续 Guest 执行、中断和定时器事件继续更新 `tsk->virt` 与硬件状态。

由于 release 不再维持冻结屏障，Phase 29 freeze_context 不是可供延迟应用的恢复映像。要安全恢复快照，必须另行设计真正保持 VM 停止的事务式冻结及失败回滚；该能力不在 Phase 30 实现。

## 3. 只读一致性观测

- 现有 snapshot status 只报告 active 与 generation，不暴露 LR、CNTV 或 task 指针。
- Phase 30 保持该最小接口，不增加可修改运行态的 Guest HVC。
- 验证快照捕获前后的 Guest tick、中断计数继续单调推进，冻结快照查询本身不会改变目标 vCPU。
- vGIC LR active 状态和物理 HW 映射仅做日志/测试观测；不通过软件写 LR 来伪造恢复。

## 4. 虚拟定时器语义检查

- 文档明确 `CNTV_CTL_EL0.ISTATUS` 为只读观察值，不作为可写状态恢复。
- 比较快照 CVAL/控制位与目标 Guest 后续 timer IRQ/tick 行为；捕获路径不改写 CNTV。
- 对 pending 注入重复、LR0 active 状态和 timer 到期边界记录结果。
- 在事务式冻结和回滚协议完成前，不尝试恢复快照中保存的 timer 字段。

## 5. 状态字段与接口

复用 task 的 `freeze_context.generation`、`active` 与 `virt` 快照，当前不增加 restore-pending 标记，也不增加可写 Guest HVC。Phase 30 首项实现限定为快照有效性检查与只读回归。

## 6. 验证矩阵

- 100 轮冻结/释放中，冻结时快照 generation 与 ACK 匹配，release 后 status 立即失效。
- VM2 vCPU1 在冻结/释放期间及之后 Guest tick 持续推进，无重复执行或寄存器破坏。
- vGIC：现有 pending/active 注入行为在快照捕获后不丢失、不重复；本阶段不回写 LR。
- vtimer：现有 timer IRQ 在快照捕获后继续触发；ISTATUS 仅作为快照观察值，本阶段不恢复 CNTV。
- Phase 22–29 回归通过，尤其是跨核 vGIC 注入、guest timer 和迁移拒绝。
- generation 不匹配或 freeze inactive 时 status 必须报告无效，不执行部分恢复。

## 7. 风险与停机条件

若无法证明 freeze SGI 在 ACK 前已完成 EOI/Deactivate，或无法避免 LR active 状态重复，则禁止写回 LR，Phase 30 仅交付验证和日志，不启用快照恢复。若 CNTV 快照与 vtimer 注入路径无法保持单一来源，也应停止 timer 写回。任何回归失败都保持 Phase 29 行为，不改动迁移状态机。

## 8. 后续边界

Phase 30 结束后仍需进一步设计 coordinator 与所有 vCPU 的 VM 级原子冻结、设备状态/DMA 静止及失败回滚；完成前 Phase 26 的双 vCPU 迁移拒绝逻辑保持不变。

## 9. 实测结果

Guest 与 Hypervisor AArch64 构建成功。双核 QEMU 中，VM2 的 100 轮冻结/释放全部通过：冻结期间快照 status 有效且 generation 与 ACK 一致，release 后 status 立即失效；无活动冻结时重复 release 被拒绝。VM1/VM2 的 Phase 22 大页配额、Phase 23 页面回收、Phase 24 KSM/CoW 回归通过；Phase 26 非法 PRECOPY 与双 vCPU START 拒绝通过。测试完成后两核 Guest tick 与 Hypervisor tick 持续单调推进至 100+，未出现重复执行或寄存器破坏迹象。QEMU 由 240 秒外部 timeout 结束（退出码 124），不作为自然退出记录。本阶段未回写任何 vGIC LR 或 CNTV 寄存器。
