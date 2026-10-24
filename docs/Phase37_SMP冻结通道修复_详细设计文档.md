# Phase 37：SMP 冻结通道修复详细设计

## 1. 背景与目标

Phase 27 的瞬时冻结依赖 CPU0 发布共享状态并通过 GICv3 SGI 通知 CPU1。双核回归中冻结 HVC 曾因 `vcpu_id` 异常而被拒绝，进一步检查发现大尺寸冻结上下文挤压了 task 页内的 EL2 栈，深层调用可覆盖 task 元数据。

Phase 37 聚焦冻结请求发布、SGI 发送及目标 vCPU 身份一致性。

## 2. 请求发布与 SGI 顺序

冻结协调核按以下顺序执行：

1. 写入目标 task、CPU、freeze generation 和 active 标记；
2. 以 release 语义发布请求；
3. 执行 `dsb ishst`，确保共享状态先于中断可见；
4. 写 `ICC_SGI1R_EL1` 发送 `SGI_VM_FREEZE`；
5. 等待与本 generation 对应的 ACK，超时期间重发 SGI。

CPU1 IRQ 分发器从 `ICC_IAR1_EL1` 取得 SGI，调用 `smp_ipi_handle()`，结束时写 `ICC_EOIR1_EL1` 并对 SGI 写 `ICC_DIR_EL1`。

## 3. 目标现场约束

冻结上下文必须来自真正被中断的目标 vCPU：

- CPU、VMID、vCPU ID 与请求目标相符；
- 当前运行 task 必须等于目标 task；
- 从当前 IRQ 异常帧捕获寄存器现场；
- 目标 vCPU 保存的系统寄存器和 VGIC/虚拟定时器状态与该现场对应。

不能对非 current task 使用其旧 `pt_regs` 作为当前运行现场，也不能在未停止目标 vCPU 时发布 ACK。

## 4. 元数据与栈布局修复

`freeze_context` 从 `task_struct` 内嵌对象改为独立页对象，降低 task 元数据占用并恢复 4KB task 页中的 EL2 栈空间。创建 task 时分配该对象，所有快照读写通过指针访问。

HVC 冻结门禁保持 VM2 vCPU0 在 CPU0 发起；目标选择同时检查 vCPU ID 和物理 CPU，阻止选中错误 task。

## 5. 回归

Test 18 对冻结/释放循环进行 100 轮测试，校验 ACK generation、快照状态及释放进度。随后 Test 19–24 覆盖事务冻结、迁移 abort/retry、双 vCPU 迁移和资源回收。
