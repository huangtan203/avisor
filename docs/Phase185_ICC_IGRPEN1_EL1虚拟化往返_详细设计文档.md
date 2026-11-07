# Phase 185：ICC_IGRPEN1_EL1 虚拟化往返验证 详细设计文档

## 1. 目的

Phase 182 评估 `ICC_BPR1_EL1` 时，曾核查 `ICC_IGRPEN1_EL1` 是否受 `ICH_HCR_EL2.TC` 影响。按 Arm 架构，它**不受 TC 捕获**，而在 `HCR_EL2.IMO=1` 时由硬件映射到虚拟接口的 `ICV_IGRPEN1_EL1`，后端为 `ICH_VMCR_EL2.VENG1`。

本阶段新增运行时用例，验证 guest 读写 `ICC_IGRPEN1_EL1` 能正确往返，并且在 VM 切换后保持。

## 2. 用例设计

新增 `phase185_igrpen1_roundtrip_test()`（`guests/el1_test/main.c`），由主核在 Phase 183 用例之后调用：

1. 写 `ICC_IGRPEN1_EL1 = 0`，立即读回，应为 0。
2. 连续 8 次 `hvc_call(HVC_SCHED_YIELD, 0)` 迫使 VM 切换。
3. 再读回，应仍为 0。
4. 恢复写 1，避免影响后续依赖 Group 1 使能的用例。

## 3. 结果

```
[vm1] IGRPEN1 after write=0 = 0, after switch = 0 ... PASSED
[vm2] IGRPEN1 after write=0 = 0, after switch = 0 ... PASSED
```

回归其余指标：PANIC 0、shm 2/0、Phase 19 SOME 0、vITS FAILED 0、`not handled` 0。`not handled` 为 0 与架构判断一致：IGRPEN1 未经 trap 路径。

## 4. 破坏性对照（后续补充）

本阶段初次提交时未做对照，理由是「当前实现下 VENG1 本来就正确保存，无法不改代码制造缺陷」。该理由不成立：Phase 186 中已验证，临时让 `vgic_save_state` 跳过 VMCR 读取即可构造对照组，改动一处、用完还原。

补充对照结果（破坏 `vgic_save_state`，不读取硬件 `ICH_VMCR_EL2`）：

```
[vm1] IGRPEN1 after write=0 = 0, after switch = 1 ... FAILED (enable bit did not stay 0 across switch)
[vm2] IGRPEN1 after write=0 = 0, after switch = 1 ... FAILED (enable bit did not stay 0 across switch)
```

结论：**本用例能够检出 VENG1 未随切换保存的缺陷**，证明力并不弱。

## 5. 依赖初值的局限

写 0 的检出能力依赖「切换后硬件 VENG1 遗留值为 1」。当前 `vgic_init()` 将 `VENG1` 初始置 1，因此已破坏的对照组读回 1，与写入的 0 可区分。

若初值恰为 0，即使切换丢失也会读回 0，用例将误判为 PASSED。这与 Phase 186 写入 3 时的盲区同类（切换后遗留值恰为 3，无法区分）。

因此本用例的准确表述是：**已通过破坏性对照证明有效，但有效性依赖 VENG1 初值为 1 这一前提**。当前代码满足该前提。

## 6. 结论与后续

- 确认 `ICC_IGRPEN1_EL1` 未出现在未处理告警中，与架构判断一致。
- 用例具备真实的回归保护能力，已被破坏性对照验证。
- 写入值 0 与初值 1 不同，当前设计合理，无需改代码。
- 若将来 VENG1 初值改为 0，需同步调整本用例的写入值。
