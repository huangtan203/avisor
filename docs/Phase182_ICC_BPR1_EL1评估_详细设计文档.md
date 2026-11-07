# Phase 182：ICC_BPR1_EL1 评估（证据驱动） 详细设计文档

## 1. 背景

Phase 181 模拟了 `ICC_PMR_EL1` 的读写。`ICH_HCR_EL2.TC=1` 同样使 EL1 访问 `ICC_BPR1_EL1` 陷入 EL2，因此需要评估它是否也需要模拟。

## 2. 事实核实

| 项目 | 结果 |
|---|---|
| guest 是否访问 BPR1 | 否。`guests/el1_test/` 中无 `BPR` 或 `S3_0_C12_C12_3` 的读写 |
| 回归日志是否出现 BPR1 相关 trap | 否。Phase 181 回归中 `system register access is not handled` 为 0 |
| 本项目是否定义 BPR1 编码 | 否。`include/` 中无 `BPR` 定义 |
| VMCR 中 VBPR1 位置 | bits[20:18]（GICv3 variant），与 VPMR[31:24] 不冲突 |

VBPR1 的位位置来自 Arm 架构参考页面 `ICH_VMCR_EL2`。`ICC_BPR1_EL1` 的独立编码页面本次无法访问，因此本文档不写入任何 BPR1 编码。

## 3. 结论

**本阶段不实现 BPR1 模拟。** 理由：

1. 不存在触发条件：guest 不访问该寄存器，回归中无 trap。
2. 编码未经核实：在没有可靠编码来源的情况下写入寄存器，可能把 trap 误映射到其他寄存器，比不处理更危险。
3. 行为影响未知：BPR1 决定抢占分组，模拟它会改变虚拟中断的抢占行为，需要专门的测试设计。

## 4. 触发时的处理路径

若将来 guest 访问 `ICC_BPR1_EL1`，`handle_trap_system()` 会输出包含 `op0/op1/crn/crm/op2/dir/rt` 的完整诊断（Phase 180）。届时按以下步骤处理：

1. 从告警编码确认寄存器，并对照架构文档核实编码。
2. 映射到 `ICH_VMCR_EL2.VBPR1`（bits[20:18]），与 Phase 181 的 PMR 方式一致。
3. 补充针对抢占分组的回归用例。

## 5. 风险

- 当前无风险：该寄存器未被访问。
- 残余风险：若 guest 使用 `ICC_BPR1_EL1` 而 hypervisor 未模拟，写入会被丢弃、读取返回未定义值，抢占分组将停留在默认值。这与 Phase 181 修复前的 PMR 问题同类，但目前没有证据表明会发生。

## 6. 验证

本阶段只新增文档，不改变代码，不需要运行时回归。已确认的事实来自代码检索与 Phase 181 回归日志。
