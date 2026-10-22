# Phase 26：迁移状态机与 SMP 边界 — 详细设计文档

## 1. 概述

### 1.1 背景

Phase 25 提供了基于 Stage-2 Access Flag 的预拷贝迁移原型，但迁移流程由单一布尔变量隐式表示，无法区分空闲、预拷贝完成、迁移完成和拒绝状态。当前目标端只预建 vCPU0；对含多个 vCPU 的源 VM，系统必须拒绝迁移，避免只停止主 vCPU 而让其他 vCPU 在源 VM 上继续运行。

Phase 26 先建立明确、可查询的迁移状态机和稳定的 HVC 错误语义，为之后实现真正的 VM 级 SMP 冻结及恢复建立可测试边界。本阶段不宣称支持双 vCPU 整体迁移。

### 1.2 目标

- ✅ 将迁移的隐式 active 标志替换为显式状态枚举。
- ✅ 定义并实现合法状态转换：IDLE/COMPLETED/REJECTED → PRECOPY → COMPLETED。
- ✅ 对非法顺序的 PRECOPY/FINISH 返回错误，不触碰目标 VM 状态。
- ✅ 多 vCPU 源 VM 的 START 被拒绝并记录 REJECTED；不执行内存拷贝或改变源任务状态。
- ✅ 新增迁移状态查询 HVC，Guest 可验证状态转换及拒绝原因。
- ✅ 保持现有单 vCPU 迁移 ABI 和目标恢复行为不变。

### 1.3 非目标

- 不实现跨物理机传输或迁移协议。
- 不实现多 vCPU 冻结、跨核寄存器快照、目标侧 vCPU 拓扑重建。
- 不转移设备 DMA、物理中断路由或外设内部状态。
- 不改变 Phase 25 的 AF 脏页跟踪算法。

## 2. 状态机

```text
                  START 成功
 IDLE --------------------------------> PRECOPY
   ^                                        |
   |                                        | PRECOPY: 保持 PRECOPY
   |                                        |
   |                         FINISH 成功    v
   +------------------- COMPLETED <---------+

 IDLE / COMPLETED / REJECTED -- START 前置条件失败 --> REJECTED
 PRECOPY -- 非法 PRECOPY/FINISH 请求 --> PRECOPY（返回 -1，不变更状态）
```

状态值固定为：

| 值 | 名称 | 含义 |
|---:|---|---|
| 0 | `MIGRATION_IDLE` | 尚未开始迁移 |
| 1 | `MIGRATION_PRECOPY` | 已开始预拷贝，可继续 PRECOPY 或 FINISH |
| 2 | `MIGRATION_COMPLETED` | 单 vCPU 迁移已完成 |
| 3 | `MIGRATION_REJECTED` | START 前置条件未满足，操作未开始 |

失败的 PRECOPY/FINISH 不修改状态。START 的前置检查失败时设为 REJECTED；后续新的 START 可从 REJECTED 重新尝试。

## 3. 接口设计

### 3.1 HVC 编号

- `HVC_MIGRATE_START (50)`：保留 Phase 25 语义。
- `HVC_MIGRATE_PRECOPY (51)`：保留 Phase 25 语义。
- `HVC_MIGRATE_FINISH (52)`：保留 Phase 25 语义。
- `HVC_MIGRATE_STATUS (53)`：返回当前迁移状态枚举值。

### 3.2 C 接口

```c
enum migration_state migration_get_state(void);
```

状态为 Hypervisor 级当前迁移上下文；当前实现一次只允许一个活动迁移。

## 4. 实现设计

### 4.1 START

按顺序检查源 task、目标 task、源/目标 VMID、目标 WAITING 状态和当前状态。检查同 VM 的调度器任务表，若发现除源 task 外仍有非 ZOMBIE vCPU，则设置 `MIGRATION_REJECTED` 并返回 `-1`。所有检查通过后才设置 `MIGRATION_PRECOPY`，执行清 AF 和全量拷贝。

### 4.2 PRECOPY

只允许在 `MIGRATION_PRECOPY` 状态调用。成功返回本轮脏页数，状态保持不变。非法状态或源/目标 task 状态不符时返回 `-1`。

### 4.3 FINISH

只允许在 `MIGRATION_PRECOPY` 状态调用。完成最终脏页拷贝和单 vCPU 状态转移后，状态设为 `MIGRATION_COMPLETED`，然后调度到目标 task。

### 4.4 状态查询

状态查询不改变迁移状态，任何 Guest 可读取当前全局迁移状态。后续支持并发迁移时，需将该状态改为按 VM/迁移 ID 索引；不在本阶段实现。

## 5. Guest 测试

- VM2（双 vCPU）调用 START，预期返回 `-1`，状态变为 REJECTED；再次查询状态应一致。
- 在未开始迁移时调用 PRECOPY/FINISH，应返回 `-1`，状态维持 IDLE。
- VM1/VM2 既有内存、大页、页面回收和 KSM 测试继续运行。
- 若测试环境有单 vCPU migration source，可验证 IDLE → PRECOPY → COMPLETED；当前默认镜像不预建该测试拓扑，双 vCPU 拒绝路径为必测项。

## 6. 验收标准

1. AArch64 Hypervisor 与 Guest 均可构建。
2. QEMU 输出双 vCPU 源 VM 的迁移拒绝结果，且状态为 REJECTED。
3. 非法 PRECOPY/FINISH 不改变 IDLE 状态。
4. Phase 22 大页配额、Phase 23 页面回收、Phase 24 KSM 测试通过。
5. 单 vCPU 迁移现有行为不发生 ABI 回归。

QEMU 实测结果（90 秒限时）：

```text
[vm1] PASSED: 大页 + 内存配额正确
[vm1] PASSED: 页面回收功能正确
[vm1] PASSED: KSM 内存去重 + CoW 功能正确
[vm2] PASS: IDLE 下非法 PRECOPY 被拒绝，状态保持不变
[vm2] PASS: 双 vCPU START 被拒绝，状态为 REJECTED
```

## 7. 文件范围

| 文件 | 变更 |
|---|---|
| `include/common/migration.h` | 迁移状态枚举和查询接口 |
| `hypervisor/common/migration.c` | 状态转换及错误状态维护 |
| `include/` 中 Guest HVC 定义 | 新增状态查询编号 |
| `hypervisor/arch/aarch64/sync_exc.c` | 分发状态查询 HVC |
| `guests/el1_test/main.c` | 非法顺序和双 vCPU 拒绝状态测试 |

## 8. 后续阶段

Phase 26 不实现 VM 级 SMP 迁移。后续阶段必须设计可中断的 vCPU freeze rendezvous、每 vCPU 独立的 EL1/vGIC/定时器快照、共享 VM 设备状态及目标 vCPU 拓扑，并通过双 vCPU 一致性测试后，才能宣称支持双 vCPU VM 整体迁移。
