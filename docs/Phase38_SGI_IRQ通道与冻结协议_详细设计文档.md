# Phase 38：SGI/IRQ 通道与 SMP 冻结协议详细设计

## 1. 背景与目标

Phase 38 将 Phase 37 暴露的冻结失败收敛为完整的 SGI/IRQ 通道协议，确保共享内存请求、GICv3 SGI 投递、CPU1 IRQ 应答、冻结快照、ACK 和 release 具有明确的内存序和状态边界。

## 2. 状态机

```text
IDLE
  -- publish request + SGI --> REQUESTED
REQUESTED
  -- target current captures context --> FROZEN
FROZEN
  -- release generation --> RELEASED
RELEASED
  -- target restores context and ACKs --> IDLE
```

每次请求使用单调 generation。ACK 和 release ACK 必须匹配当前 generation，旧请求的迟到 SGI 不得改变新请求状态。

## 3. 内存序

协调核在发送 SGI 前使用：

```asm
dsb ishst
write ICC_SGI1R_EL1
isb
```

目标核读取 active/generation 后捕获现场，再以 DMB/原子 release 发布 ACK。协调核以 acquire 读取 ACK。release 同样先发布 release generation，目标恢复现场后发布 release ACK。

## 4. IRQ 生命周期

IRQ 入口保存当前 task 的异常帧；`handle_irq()` 读取 IAR 并分发 SGI/PPI；处理完成后写 EOI。SGI 需要显式 DIR 去活，定时器 PPI 维持其既有 EOI/DIR 路径。冻结协议不使用定时器轮询替代 SGI，避免在非 current task 上错误捕获现场。

## 5. 失败处理

- 找不到目标 vCPU：不发布 ACK，协调核超时返回失败；
- generation 不匹配：丢弃旧请求；
- 目标不是 current：不捕获、不 ACK；
- release 校验失败：不恢复异常帧，并报告 release ACK，避免死循环；
- 所有失败路径保持 task 身份字段不变。

## 6. 验证

双核 QEMU 验证 Test 18 的 100 轮冻结，以及事务冻结和迁移测试。验证内容包括无 HVC gate 拒绝、无 SGI 超时、无栈覆盖、快照 generation 一致和 release 后 vCPU 继续运行。
