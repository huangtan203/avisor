# Phase 29：vCPU 架构状态快照 — 详细设计文档

## 1. 概述

### 1.1 背景

Phase 28 已让 freeze_context 中的通用寄存器、SP、PC、PSTATE 在 release 时回填到 IRQ 返回帧。若要把 freeze rendezvous 发展为 VM 级一致性屏障，还需要在同一冻结代次中取得 EL1 system registers、vGIC 和虚拟定时器状态。当前 IRQ 入口的 `vm_leaving_work()` 保存 EL1 system registers；vGIC/定时器的常规保存仅在任务实际切换离开 VM 时发生，因此冻结路径必须明确捕获它们。

### 1.2 目标

- 在目标 vCPU IRQ 入口完成现有 `save_sysregs()` 后，将 `cpu_sysregs` 复制到 freeze_context。
- 在冻结 handler 内保存 vGIC VMCR/LR 和虚拟定时器控制/比较值。
- 使用 generation 标记快照一致性，并在 ACK 前发布完整快照。
- 增加只读 HVC，让 VM2 coordinator 查询目标快照是否有效及对应 generation。
- QEMU 验证 100 轮冻结中快照有效，且 release 后现有 Guest 行为连续。

### 1.3 非目标

- 不在 Phase 29 从软件快照恢复硬件 vGIC/定时器状态；Phase 28 仍负责通用异常帧恢复。
- 不冻结本地 coordinator vCPU，也不构建通用参与者位图。
- 不将冻结接入 migration.c，不改变双 vCPU 迁移拒绝行为。
- 不涵盖设备 DMA、共享设备状态、物理中断路由或跨机器状态传输。

## 2. 快照模型

每个 task 的 freeze_context 增加：

- `struct cpu_sysregs`：由 IRQ 入口既有 `vm_leaving_work()` 捕获的 EL1 系统寄存器；
- `struct vm_virt_state`：冻结 handler 显式捕获的 vGIC VMCR/LR 与 CNTV 控制/比较值；
- 既有通用寄存器、SP、PC、PSTATE、generation 与 active。

目标 vCPU 在 IRQ handler 中被固定停留，协调方确认 ACK 后才能查询快照。handler 先写所有快照字段和 generation，再以 release 语义发布 ACK；查询方 acquire 读取 ACK/快照代次。只有 generation 匹配且上下文 active 时报告有效。

注意：快照捕获期间硬件 vGIC 和 CNTV 仍保持原运行态；Phase 29 不将快照回写硬件，避免在 IRQ 路径引入未经验证的 GIC active/EOI 与定时器重装副作用。

## 3. HVC 接口

新增测试专用 `HVC_SMP_FREEZE_SNAPSHOT_STATUS = 58`：

- 仅允许 VM2 vCPU0 在物理 CPU0 查询；
- `x0=目标物理核`；
- 返回 `x0=1` 表示当前冻结目标快照有效且 generation 匹配，`x0=0` 表示无效；
- 返回 `x1=快照 generation`。

不向 Guest 暴露 task 指针或快照内容。

## 4. 流程

1. begin 选择并验证固定拓扑中的 VM2 vCPU1。
2. SGI 进入目标 IRQ handler；入口先执行 `vm_leaving_work()`，保存 EL1 system registers。
3. freeze handler 捕获 `cpu_sysregs`、vGIC VMCR/LR、CNTV_CTL/CVAL 与通用异常现场。
4. 写入 generation 和 active 后发布 ACK。
5. coordinator 查询 snapshot status，验证 active 和 generation。
6. release 后 Phase 28 逻辑将 GPR/异常返回现场恢复到 IRQ 帧并返回 Guest；软件 vGIC/定时器快照保留为本代诊断快照，不执行硬件恢复。

## 5. 测试与验收

- Guest/Hypervisor AArch64 构建成功。
- 100 轮 begin 后，快照状态有效且 generation 等于 ACK generation。
- release 后 Guest vCPU 可继续执行；无活动 release 仍被拒绝。
- Phase 22–26 回归通过，双 vCPU START 仍为拒绝。
- 真实超时、迟到 ACK、虚拟中断 active 状态、定时器到期边界不在本阶段声称已验证。

## 6. 文件范围

| 文件 | 变更 |
|---|---|
| `include/common/sched.h` | 扩充 freeze_context 的架构状态字段 |
| `hypervisor/common/smp.c` | 捕获虚拟化状态并检查快照代次 |
| `include/common/hvc.h`、Guest HVC 头文件 | 新增快照状态查询 HVC |
| `hypervisor/arch/aarch64/sync_exc.c` | 分发受限只读查询 |
| `guests/el1_test/main.c` | 每轮核对快照有效性和 generation |

## 7. 后续阶段边界

Phase 29 只证明冻结时能够捕获并查询 per-vCPU 软件快照。下一阶段需评估 vGIC LR active 状态、虚拟定时器到期、EL1 system register 恢复和 IRQ 返回的原子协调，随后才可设计多 vCPU 迁移屏障。Phase 26 的双 vCPU 迁移拒绝逻辑继续保持。

## 8. 实测结果

Guest 与 Hypervisor AArch64 构建成功。双核 QEMU 测试中，100 轮 freeze begin 均返回有效快照状态，快照 generation 与 ACK generation 一致，release 成功；无活动冻结时重复 release 被拒绝。VM1 与 VM2 的 Phase 22 大页/配额、Phase 23 页面回收、Phase 24 KSM/CoW 测试通过；Phase 26 非法 PRECOPY 与双 vCPU START 拒绝测试通过。释放后两个物理核的 Guest 与 Hypervisor tick 持续推进。QEMU 由 240 秒外部 timeout 结束（退出码 124），因此不记为自然退出。构建输出包含若干既有警告，本阶段无新增编译错误。
