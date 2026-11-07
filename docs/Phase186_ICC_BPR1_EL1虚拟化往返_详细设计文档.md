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

1. 写 `ICC_BPR1_EL1 = 3`，`isb`，读回取 bits[2:0]。
2. 连续 8 次 `hvc_call(HVC_SCHED_YIELD, 0)` 迫使 VM 切换。
3. 再读回 bits[2:0]，应仍为 3。

用例未恢复原值。BPR1 的复位值为 UNKNOWN，且后续测试不依赖该字段。

## 4. 结果

```
[vm1] BPR1 after write=3 -> 3, after switch -> 3 ... PASSED
[vm2] BPR1 after write=3 -> 3, after switch -> 3 ... PASSED
```

回归其余指标：PANIC 0、shm 2/0、Phase 19 SOME 0、vITS FAILED 0、`not handled` 0。

## 5. 证明力的局限

与 Phase 185 相同：本用例只证明「当前实现下往返成立且切换后保持」，**没有做破坏性对照实验**，因此不能证明切换持久性的修复确实必要。

## 6. 结论

- 编码已核实并落地，BPR1 访问未出现在未处理告警中。
- 用例可作为回归守护，若 VBPR1 映射被破坏会失败。
- 若要强证明，需单独阶段做破坏性对照，并先评估对基础设施的侵入性。
