# Phase 186：ICC_BPR1_EL1 虚拟化往返验证 详细设计文档

## 1. 目的

Phase 182 因编码无法核实而只写了评估文档。本阶段在核实编码后，补充运行时往返用例，验证 guest 读写 `ICC_BPR1_EL1` 能正确映射并在 VM 切换后保持。

## 2. 编码核实

来源：Arm 官方 SysReg XML 页面（经 kib.kiev.ua 镜像）。

- 编码：`op0=3, op1=0, CRn=12, CRm=12, op2=3` → `S3_0_C12_C12_3`
- `BinaryPoint` 位于 bits[2:0]。
- `HCR_EL2.IMO=1` 时，EL1 访问映射到 `ICV_BPR1_EL1`，并更新 `ICH_VMCR_EL2.VBPR1`（bits[20:18]）。

该页面同时说明 BPR1 的非安全副本有最小值约束（不小于 `ICC_BPR0_EL1 + 1`）。

**钳位实测**：在 guest 内直接读 `ICC_BPR0_EL1`（`S3_0_C12_C8_3`）得到 **2**，因此 `ICC_BPR1_EL1` 的有效范围为 **3..7**。实测写 1、2 时，写后**立即**读回即为 3（尚未发生任何 VM 切换），确认是硬件钳位而非 VM 间泄漏。探测值必须全部落在 3..7 内。

## 3. 用例设计

新增 `phase186_bpr1_roundtrip_test()`（`guests/el1_test/main.c`），由主核在 Phase 185 之后调用：

1. 按 VMID 选择探测值区间：VM1 取 `{3,4}`，VM2 取 `{5,6}`。两者互不重叠，且全部落在钳位后的有效范围 3..7 内。
2. 共 2 轮，每轮写入 `base + r`，`isb`，读回取 bits[2:0]。连续 8 次 `hvc_call(HVC_SCHED_YIELD, 0)` 迫使 VM 切换，再连续读回两次，任一次不等于本轮探测值即判 FAILED。

**设计动机**：
- 早期版本两个 VM 写入同一个值，无法区分“自己的值”与“对方泄漏进来的值”，因此漏检了 VM 间串扰。按 VMID 区分探测值后，任何泄漏都会表现为读回值不等于本 VM 的探测值。
- **单轮存在盲区**：破坏 save/restore 后，同核读回值等于该核上最后一次写入。只写一次时，“最后写入者”必然读回自己的值而 PASSED。改为多轮、每轮换值，只要有一轮两侧执行顺序使先写者读到对方的值即可检出。
- 探测值必须避开钳位区：曾用 `{1,2,3}` 作为 VM1 区间，导致 vm1 写 1、2 均读回 3，产生正常实现下的误报，根因即 `BPR0=2` 的钳位。

用例未恢复原值。BPR1 的复位值为 UNKNOWN，且后续测试不依赖该字段。

## 4. 结果

正常实现：

```
[vm1]   round 0: write=3 -> 3, again -> 3 ... ok
[vm2]   round 0: write=5 -> 5, again -> 5 ... ok
[vm1]   round 1: write=4 -> 4, again -> 4 ... ok
[vm2]   round 1: write=6 -> 6, again -> 6 ... ok
[vm1]   BPR1 PASSED
[vm2]   BPR1 PASSED
```

回归其余指标：PANIC 0、shm 2/0、Phase 19 SOME 0、vITS FAILED 0、`not handled` 0。

## 5. 破坏性对照

两类破坏，均在 `hypervisor/boards/virt/vgic.c` 中临时注入，实验后已还原并与 HEAD 核对一致。

**破坏 A：保存丢失**（`vgic_save_state` 不读取 `ICH_VMCR_EL2`）

| 用例 | vm1 | vm2 |
|---|---|---|
| Phase 185 IGRPEN1（vm1 写 0，vm2 写 1） | FAILED（`0 -> 1`） | PASSED |
| Phase 186 BPR1（vm1 `{3,4}`，vm2 `{5,6}`） | FAILED（第 1 轮 `4 -> 3`） | FAILED（第 0 轮 `5 -> 3`，第 1 轮 `6 -> 3`） |

**破坏 B：跨 VM 泄漏**（`vgic_save_state` 与 `vgic_restore_state` 均不处理 `ICH_VMCR_EL2`）

| 用例 | vm1 | vm2 |
|---|---|---|
| Phase 185 IGRPEN1（vm1 写 0，vm2 写 1） | FAILED（`0 -> 1`） | FAILED（`1 -> 0`） |
| Phase 186 BPR1（vm1 `{3,4}`，vm2 `{5,6}`） | FAILED（第 0 轮 `3 -> 6`） | FAILED（第 0 轮 `5 -> 3`，第 1 轮 `6 -> 4`） |

两类破坏下 BPR1 用例两侧均被检出。破坏 A 下 vm2 的 IGRPEN1 PASSED 属于单侧漏检，判断时需结合两侧数据（见第 6 节）。

**pCPU 时序验证**：用例打印 `[pcpu 起始->结束]`（由 `HVC_SCHED_GET_CPU` 获取）。前序破坏 B 实验中两个 VM 均为 `pcpu 0->0`，证实两者在同一物理核上交替运行，因此 `ICH_VMCR_EL2` 的串扰是同核上的真实泄漏，而非跨核现象。

## 6. 证明力的局限

1. **单轮写入会漏检。** 在 save/restore 被破坏时，同核读回值等于该核上最后一次写入，只写一次的用例中「最后写入者」必然读回自己的值而 PASSED，单侧 PASSED 无法说明无泄漏。多轮写入（每轮换值）后，任一轮发生顺序反转即可让两侧被检出，因此当前用例两侧 FAILED 才足以作为泄漏证据。
2. **IGRPEN1 只有 1 位**，只能取 0/1，而 1 恰为初值，导致“丢失到初值”的情形在 vm2 侧才能暴露。
3. 本对照只证明用例**能检出** save/restore 路径缺陷，并不证明当前 hypervisor 实现存在缺陷。当前实现已正确保存并恢复 VBPR1 与 VENG1。

## 7. 结论

- 编码已核实并落地，BPR1 访问未出现在未处理告警中。
- 用例已改为按 VMID 区分探测值（落在有效范围 3..7 内），并通过两类破坏实验验证两侧均能检出泄漏。
- 正常实现下全部 PASSED，回归指标正常。
