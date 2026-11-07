# Phase 185：ICC_IGRPEN1_EL1 虚拟化往返验证 详细设计文档

## 1. 目的

Phase 182 评估 `ICC_BPR1_EL1` 时，曾核查 `ICC_IGRPEN1_EL1` 是否受 `ICH_HCR_EL2.TC` 影响。按 Arm 架构，它**不受 TC 捕获**，而在 `HCR_EL2.IMO=1` 时由硬件映射到虚拟接口的 `ICV_IGRPEN1_EL1`，后端为 `ICH_VMCR_EL2.VENG1`。

本阶段新增运行时用例，验证 guest 读写 `ICC_IGRPEN1_EL1` 能正确往返，并且在 VM 切换后保持。

## 2. 用例设计

新增 `phase185_igrpen1_roundtrip_test()`（`guests/el1_test/main.c`），由主核在 Phase 183 用例之后调用：

1. 按 VMID 选择探测值：VM1 写 0，VM2 写 1。`EnableGrp1` 只有 1 位，这是唯一能区分两个 VM 的取值组合。
2. 写 `ICC_IGRPEN1_EL1`，立即读回，应等于本 VM 的探测值。
3. 连续 8 次 `hvc_call(HVC_SCHED_YIELD, 0)` 迫使 VM 切换。
4. 再读回，应仍等于本 VM 的探测值。
5. 恢复写 1，避免影响后续依赖 Group 1 使能的用例。

**设计动机**：早期版本两个 VM 都写 0，无法区分“自己的值”与“对方泄漏进来的值”，因此漏检了 VM 间串扰。按 VMID 区分探测值后，任何泄漏都会表现为读回值不等于本 VM 的探测值。

## 3. 结果

正常实现：

```
[vm1] IGRPEN1 write=0 -> 0, after switch -> 0 ... PASSED
[vm2] IGRPEN1 write=1 -> 1, after switch -> 1 ... PASSED
```

回归其余指标：PANIC 0、shm 2/0、Phase 19 SOME 0、vITS FAILED 0、`not handled` 0。`not handled` 为 0 与架构判断一致：IGRPEN1 未经 trap 路径。

## 4. 破坏性对照

两类破坏，均在 `hypervisor/boards/virt/vgic.c` 中临时注入，实验后已还原并与 HEAD 核对一致。

**破坏 A：保存丢失**（`vgic_save_state` 不读取 `ICH_VMCR_EL2`）

在旧版用例（两 VM 均写 0）下：

```
[vm1] IGRPEN1 after write=0 = 0, after switch = 1 ... FAILED
[vm2] IGRPEN1 after write=0 = 0, after switch = 1 ... FAILED
```

能检出。原因是切换后硬件遗留值为 1（`vgic_init()` 的初值），与写入的 0 可区分。

**破坏 B：跨 VM 泄漏**（save/restore 均不处理 VMCR，两个 VM 写入不同值）

```
[vm1] IGRPEN1 write=0 -> 0, after switch -> 1 ... FAILED
[vm2] IGRPEN1 write=1 -> 1, after switch -> 0 ... FAILED
```

两侧均检出。旧版用例在同样的破坏下只有一侧失败（见 Phase 186 文档第 5 节的同类记录），说明按 VMID 区分探测值确实提升了检出能力。

## 5. 证明力的局限

1. **只有 1 位可取**，两个 VM 的探测值必然是 {0,1}，而 1 恰为初值。因此“丢失到初值”这类缺陷在 VM2 侧无法与“正确读回自己的值”区分。这是寄存器位宽决定的，无法通过调整探测值规避。
2. 本对照只证明用例**能检出** save/restore 路径缺陷，并不证明当前 hypervisor 实现存在缺陷。当前实现已正确保存并恢复 VENG1。
3. 检出依赖切换顺序与残留值，单看某一侧的 PASSED 不足以说明无泄漏。

## 6. 结论与后续

- 确认 `ICC_IGRPEN1_EL1` 未出现在未处理告警中，与架构判断一致。
- 用例已改为按 VMID 区分探测值（VM1 写 0，VM2 写 1），并通过两类破坏实验验证能检出泄漏。
- 若将来 `VENG1` 初值改为 0，本用例在 VM2 侧的检出能力会进一步下降，需重新评估。
