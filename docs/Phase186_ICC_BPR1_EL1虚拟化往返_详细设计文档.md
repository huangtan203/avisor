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

1. 按 VMID 选择探测值：VM1 写 5，VM2 写 6。两者互不相同，且都偏离初值 0，避开 BinaryPoint 最小值边界。
2. 写 `ICC_BPR1_EL1`，`isb`，读回取 bits[2:0]。
3. 连续 8 次 `hvc_call(HVC_SCHED_YIELD, 0)` 迫使 VM 切换。
4. 再读回 bits[2:0]，应仍等于本 VM 的探测值。

**设计动机**：早期版本两个 VM 写入同一个值，无法区分“自己的值”与“对方泄漏进来的值”，因此漏检了 VM 间串扰。按 VMID 区分探测值后，任何泄漏都会表现为读回值不等于本 VM 的探测值。

用例未恢复原值。BPR1 的复位值为 UNKNOWN，且后续测试不依赖该字段。

## 4. 结果

正常实现：

```
[vm1] BPR1 write=5 -> 5, after switch -> 5 ... PASSED
[vm2] BPR1 write=6 -> 6, after switch -> 6 ... PASSED
```

回归其余指标：PANIC 0、shm 2/0、Phase 19 SOME 0、vITS FAILED 0、`not handled` 0。

## 5. 破坏性对照

两类破坏，均在 `hypervisor/boards/virt/vgic.c` 中临时注入，实验后已还原并与 HEAD 核对一致。

**破坏 A：保存丢失**（`vgic_save_state` 不读取 `ICH_VMCR_EL2`）

| 用例 | 结果 |
|---|---|
| Phase 183 PMR | PASSED（写路径直接改硬件与副本，不经 save，符合设计） |
| Phase 185 IGRPEN1（写 0） | FAILED（`after switch = 1`，两 VM 均检出） |
| Phase 186 BPR1（写 3） | PASSED（**漏检**：切换后遗留值恰为 3） |
| Phase 186 BPR1（写 7） | FAILED（`after switch -> 3`） |

**破坏 B：跨 VM 泄漏**（`vgic_save_state` 与 `vgic_restore_state` 均不处理 `ICH_VMCR_EL2`，且两个 VM 写入不同值）

| 用例 | vm1 | vm2 |
|---|---|---|
| Phase 185 IGRPEN1（vm1 写 0，vm2 写 1） | PASSED | FAILED（`1 -> 0`） |
| Phase 186 BPR1（vm1 写 5，vm2 写 6） | PASSED | FAILED（`6 -> 5`） |

破坏 B 中的 vm1 Phase 185 与 vm1 BPR1 之所以 PASSED，是因为此时切入 vm1 时硬件残留的恰好是 vm1 自己的值。

## 6. 证明力的局限

1. **泄漏是单向可见的。** 切换到 vm2 时读到 vm1 的残留值会暴露；切换到 vm1 时若残留恰为 vm1 自己的值则不会暴露。因此单看一侧 PASSED 不能说明无泄漏。
2. **IGRPEN1 只有 1 位**，只能取 0/1，而 1 恰为初值，导致“丢失到初值”的情形在 vm2 侧才能暴露。
3. 本对照只证明用例**能检出** save/restore 路径缺陷，并不证明当前 hypervisor 实现存在缺陷。当前实现已正确保存并恢复 VBPR1 与 VENG1。

## 7. 结论

- 编码已核实并落地，BPR1 访问未出现在未处理告警中。
- 用例已改为按 VMID 区分探测值，并通过两类破坏实验验证能检出泄漏。
- 正常实现下全部 PASSED，回归指标正常。
