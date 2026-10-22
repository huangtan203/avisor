# Phase 27：SMP 迁移冻结握手 — 详细设计文档

## 1. 概述

### 1.1 背景

Phase 26 将迁移流程改为显式状态机，并在双 vCPU VM 上安全拒绝当前尚不支持的整体迁移。后续若要支持双 vCPU VM，首先需要可靠的跨物理核 rendezvous：迁移发起核请求同一 VM 的其他 vCPU 到达一致性停止点，确认其 Guest 执行现场已保存，再允许迁移方读取或复制 VM 状态。

当前调度器按物理核维护独立任务表，vCPU 固定绑定物理核。SGI 处理运行在 EL2 IRQ 上下文。仅在 IPI handler 中把任务改为 `TASK_WAITING` 并不能自动证明完整 Guest 异常现场已保存、可在恢复时无损返回。因此 Phase 27 必须先建立可验证的冻结/释放协议，不能直接接入 Phase 25 的状态复制流程。

### 1.2 目标

- 定义按 VMID/vCPU 标识的冻结请求、到达确认、释放与失败状态。
- 明确每个 vCPU 的冻结现场所有权及其生命周期。
- 使用 SGI 触发远端 vCPU 在 EL2 进入协调路径。
- 只有全部目标 vCPU 到达已保存现场的停止点后，协调方才得到成功结果。
- 在冻结失败、取消或部分到达时，保证已到达 vCPU 可被释放，不留下永久停机的 Guest。
- 通过双 vCPU QEMU 测试验证 request → rendezvous → release，且冻结期间不继续执行 Guest 指令。

### 1.3 非目标

- 不在本阶段实现页面传输、脏页收敛或目标 VM 恢复。
- 不转移 vGIC、定时器、设备、DMA 或共享内存状态。
- 不将 Phase 26 的多 vCPU 迁移拒绝改为允许迁移。
- 不支持并发冻结多个 VM；首版只允许一个活动 rendezvous。

## 2. 当前约束与实现前置检查

实现前必须逐项确认以下架构事实，并在代码评审中引用对应向量/汇编路径：

1. IRQ 入口保存的 `pt_regs` 位于哪个栈、归属哪个 task，是否涵盖 x0-x30、ELR_EL2、SPSR_EL2。
2. `cpu_switch_to` 保存的 `cpu_context` 与异常现场的关系；被 IRQ 打断的 Guest 现场是否会在 `TASK_WAITING` 后稳定保留。
3. `schedule()` 从 IRQ 上下文切换的栈生命周期，以及恢复任务时如何回到被中断 Guest。
4. EL2 IRQ handler 是否允许安全地改变当前 task 状态并调度；中断嵌套和本地 IRQ 屏蔽规则是什么。当前实现中 IRQ handler 调用 `schedule()` 会导致异常帧栈与切换任务栈不匹配，因此冻结 handler 不得调用调度器。
5. SGI 对目标 CPU 的亲和路由和发布/读取顺序，原子变量使用的 acquire/release 语义。

若任一项不能由当前异常入口与调度器证明，本阶段应先增加专用的 per-vCPU freeze context，而不得复用不明确的任务栈内存。

## 3. 状态模型

每个 rendezvous 使用单调递增 generation，避免旧 SGI/迟到 ACK 被新请求误认：

```text
IDLE --request(gen)--> REQUESTED
REQUESTED --目标核保存现场并 ACK(gen)--> FROZEN
REQUESTED --取消/超时--> ABORTING --> IDLE
FROZEN --release(gen)--> RELEASING --> IDLE
```

每个参与 vCPU 状态：`RUNNING`、`REQUESTED`、`FROZEN`、`RELEASED`。目标集合按源 VM 的 vCPU task 表确定，忽略 idle task 与 ZOMBIE task。每个 vCPU 最多只能由其绑定物理核处理本地 freeze 请求。

## 4. 握手流程

### 4.1 请求

1. 请求方在本地锁/中断屏蔽保护下确认 coordinator 为 IDLE。
2. 扫描所有 CPU 的任务表，构建同 VM 的参与 vCPU 集合，并记录 task 指针、物理核、vcpu_id 和 generation。
3. 发布 REQUESTED 状态及 generation，执行适当的内存屏障。
4. 对每个远端参与核发送专用 `SGI_VM_FREEZE`；本地 vCPU 直接进入相同的保存路径。

### 4.2 远端到达

1. SGI handler 校验 generation、VMID、vcpu_id 和 task 仍有效。
2. 在规定的 EL2 入口保存完成之后，复制/固定可恢复的 vCPU 异常现场到专用 freeze context。
3. 发布该 vCPU 的 FROZEN ACK。
4. 该 vCPU 不再执行 Guest 指令；它进入可被安全调度的等待状态，或停留在明确设计的 EL2 rendezvous wait 路径。

### 4.3 完成与释放

- coordinator acquire-load 所有参与者 ACK；全部 FROZEN 后才报告成功。
- release 使用同 generation 广播；每个核恢复原 task 的寄存器和调度状态，再发布 RELEASED。
- coordinator 收齐 RELEASED 后回到 IDLE。
- 超时/取消必须释放已经冻结的 vCPU；不能只清 coordinator 标志。

## 5. 同步与内存模型

- 单活动请求用原子 compare-exchange 从 IDLE 进入 REQUESTED。
- 请求 payload（VMID、generation、参与者位图和 task 标识）先写入，之后以 release 发布状态。
- SGI handler 以 acquire 读取请求 payload。
- ACK/release 状态使用 release/acquire 配对。
- generation 采用足够宽的无符号整数；回绕处理需保证不存在旧请求仍在途。
- 不在跨核共享状态中使用普通非 volatile、无屏障访问来承担同步语义。

## 6. 失败语义

- 无参与 vCPU、目标 task 已退出、VMID 不一致、已有 rendezvous：请求立即失败，不发送 SGI。
- 超时前部分 vCPU 已冻结：进入 ABORTING，向已冻结核发送 release，并等待确认恢复后才返回失败。
- generation 不匹配或迟到 SGI：不改变 task 状态，只记录可诊断计数。
- 冻结现场保存失败：禁止向 coordinator 报成功，执行同一回滚路径。
- 本阶段不复用 MIGRATION_PRECOPY 状态；冻结握手状态独立于迁移状态机。

## 7. 接口与测试设计

最终 HVC 编号和 ABI 在代码实现前再依据现有 HVC 头文件分配。本阶段拟提供测试专用的 FREEZE_BEGIN / FREEZE_RELEASE / FREEZE_STATUS 操作，禁止把内部 task 指针暴露给 Guest。

双 vCPU Guest 测试：

1. vCPU0 发起 begin，VMID 与参与 vCPU 数匹配。
2. vCPU1 记录执行计数并在请求窗口内停止 Guest 进展。
3. vCPU0 观察到两 vCPU 均 FROZEN，检查 generation 与参与位图。
4. vCPU0 release；vCPU1 恢复后确认现场寄存器、栈和计数连续。
5. 重复多轮并施加定时器/虚拟中断负载，检查无死锁、丢中断或状态串扰。
6. 注入目标 vCPU 退出或错误 generation，验证部分冻结可以完整回滚。

## 8. 验收标准

- AArch64 Hypervisor 和 Guest 构建通过。
- 双 vCPU request/ACK/release 至少重复 100 轮无死锁。
- 冻结期间目标 Guest 计数器不增加；release 后继续运行。
- vCPU 的通用寄存器、PC、SP、PSTATE 与中断状态在释放前后满足预期。
- 取消/失败测试无 vCPU 遗留在 WAITING/FROZEN 状态。
- Phase 22–26 现有测试通过；Phase 26 对不完整整体迁移的拒绝行为保持不变。

## 9. 文件范围（实现阶段）

预计涉及 `include/common/smp.h`、`include/common/sched.h`、`hypervisor/common/smp.c`、`hypervisor/common/sched.c`、AArch64 异常入口/向量汇编、virt SGI 分发、HVC 定义和 Guest SMP 测试。具体范围须由第 2 节现场保存审查决定，避免未经证明地复用 task stack。

## 10. 当前原型实现与验证结果

本阶段当前交付的是 VM2 双 vCPU 的受限冻结原型，不是通用迁移屏障：HVC 仅允许 VM2 vCPU0 在物理 CPU0 发起，目标固定为物理 CPU1。协调端在 CPU1 等待 ACK 时周期重发 SGI，使 CPU1 上当前运行非目标 VM 时，请求可延后到 VM2 vCPU1 获得执行机会。SGI handler 校验当前 task 与预期 VMID/vCPU/物理核一致，在 task 专属 `freeze_context` 中保存完整 `pt_regs` 的 x0-x30、SP、PC 和 PSTATE，发布 ACK 后在 EL2 IRQ handler 中等待共享 release generation。release 后清理快照并从原 IRQ 异常帧返回。目标核不调用调度器。

该实现只测试 VM2 固定双核拓扑，快照尚未被用于迁移复制或独立恢复，也未完成超时情况下的完整参与者回滚；因此不能作为生产迁移安全屏障。

HVC 接口为：

- `HVC_SMP_FREEZE_BEGIN`（54）：`x0=目标物理核`，验证并等待绑定目标 vCPU 的 ACK；
- `HVC_SMP_FREEZE_RELEASE`（55）：释放当前冻结目标，返回实际成功/失败；
- `HVC_SMP_FREEZE_ACK`（56）：`x0=核号`，返回 ACK generation；
- `HVC_SMP_FREEZE_PROGRESS`（57）：`x0=核号`，返回成功进入冻结现场的累计次数。

Guest VM2 测试重复执行 100 轮 begin/ACK/release，并检查 ACK 非零及 progress 每轮恰好递增一次；随后验证无活动冻结时重复 release 被拒绝。AArch64 Guest 与 Hypervisor 构建成功。最终 QEMU 双核日志确认握手与重复 release 检查、Phase 26 非法 PRECOPY 和双 vCPU START 拒绝测试通过；VM1/VM2 的 Phase 22 大页/配额、Phase 23 页面回收、Phase 24 KSM 测试通过；握手后 VM 与两核 Hypervisor tick 继续推进。QEMU 运行由 240 秒外部 timeout 结束，故不将其记为自然退出。

失败处理约束：begin 超时后发布当前 generation 的 release，并等待远端 release ACK；若 ACK 迟到，保留活动 generation，下一次 begin 只有在确认远端已 release 后才回收该请求。若仍未确认，则拒绝新的 begin，避免覆盖可能仍冻结的目标。release 仅接受当前 generation 已 ACK 且尚未 release 的请求；等待远端 release ACK 超时返回失败并保留状态。当前 Guest 测试未注入真实 ACK 超时或迟到 ACK，相关故障路径尚未由 QEMU 验收。

## 11. 阶段边界

Phase 27 只交付可重复验证的物理核握手原型，不执行迁移状态复制，也不解锁双 vCPU 热迁移。要接入迁移前，必须先实现并验证按 VMID/vCPU 标识的参与者集合、目标 task 校验、可恢复的专用冻结现场、明确的停止/恢复语义及部分冻结回滚；在这些条件满足前，Phase 26 的双 vCPU 迁移拒绝逻辑必须保持不变。
