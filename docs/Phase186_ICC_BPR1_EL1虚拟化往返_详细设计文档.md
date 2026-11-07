# Phase 186：ICC_BPR1_EL1 虚拟化往返验证 详细设计文档

## 1. 目的

Phase 182 因编码无法核实而只写了评估文档。本阶段在核实编码后，补充运行时往返用例，验证 guest 读写 `ICC_BPR1_EL1` 能正确映射并在 VM 切换后保持。

## 2. 编码核实

来源：Arm 官方 SysReg XML 页面（经 kib.kiev.ua 镜像）。

- 编码：`op0=3, op1=0, CRn=12, CRm=12, op2=3` → `S3_0_C12_C12_3`
- `BinaryPoint` 位于 bits[2:0]。
- `HCR_EL2.IMO=1` 时，EL1 访问映射到 `ICV_BPR1_EL1`，并更新 `ICH_VMCR_EL2.VBPR1`（bits[20:18]）。

该页面同时说明 BPR1 的非安全副本有最小值约束（不小于 `ICC_BPR0_EL1 + 1`）。本用例写入的 `BinaryPoint=3` 是否被钳位，取决于 `ICC_BPR0_EL1` 的当前值，因此用例只做读回比对，并如实报告。

## 3. 用例设计

新增 `phase186_bpr1_roundtrip_test()`（`guests/el1_test/main.c`），由主核在 Phase 185 之后调用：

1. 写 `ICC_BPR1_EL1 = 7`，`isb`，读回取 bits[2:0]。
2. 连续 8 次 `hvc_call(HVC_SCHED_YIELD, 0)` 迫使 VM 切换。
3. 再读回 bits[2:0]，应仍为 7。

用例未恢复原值。BPR1 的复位值为 UNKNOWN，且后续测试不依赖该字段。

**写入值选择**：最初写入 3，对照实验发现它无法检出丢失（见第 5 节），故改为 7。

## 4. 结果

正常实现：

```
[vm1] BPR1 after write=7 -> 7, after switch -> 7 ... PASSED
[vm2] BPR1 after write=7 -> 7, after switch -> 7 ... PASSED
```

回归其余指标：PANIC 0、shm 2/0、Phase 19 SOME 0、vITS FAILED 0、`not handled` 0。

## 5. 破坏性对照（已完成）

破坏方式：`vgic_save_state` 不再读取硬件 `ICH_VMCR_EL2`（对照后已还原，与 HEAD 一致）。

| 用例 | 结果 | 说明 |
|---|---|---|
| Phase 183 PMR | PASSED | 写路径直接改硬件与副本，不经 save，符合设计 |
| Phase 185 IGRPEN1 | FAILED（`after switch = 1`） | 能检出 |
| Phase 186 BPR1，写 3 | PASSED | **检不出**：切换后的值恰为 3，与写入值相同 |
| Phase 186 BPR1，写 7 | FAILED（`after switch -> 3`） | **能检出** |

结论：Phase 186 最初的 PASSED 是用例设计盲区（写入值与切换后遗留值相同），不是硬件路径问题。改用 7 后，用例能够检出 VBPR1 未随切换保存的缺陷，因此它具备真实的回归保护能力。

## 6. 证明力的局限

本对照只证明用例「能检出 save 路径丢失」。它没有证明 Phase 186 对应的 hypervisor 实现本身需要修复，实际上当前实现已正确保存 VBPR1。

## 7. 结论

- 编码已核实并落地，BPR1 访问未出现在未处理告警中。
- 用例已修正为写 7，并通过破坏性对照验证其有效性。
- 与 Phase 185 不同，本阶段已完成破坏性对照，证明力高于前者。

