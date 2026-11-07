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

## 4. 证明力的局限（必须说明）

**本用例的证明力弱于 Phase 184，不能声称它已证明切换持久性。**

Phase 184 通过对照实验证实：还原为有缺陷的实现后，用例会失败。本阶段**没有**做同样的对照。原因是：在当前实现下 VENG1 本来就被正确保存，因此无法仅靠「不改代码」产生一个有缺陷的对照组。若要做等价对照，需要人为破坏 `ICH_VMCR` 的保存路径（例如让 `vgic_save_state` 跳过 VMCR），这属于对基础设施的侵入性改动，超出本阶段范围。

因此本用例的准确结论是：**在当前实现下读写往返成立且切换后保持**，而不是「已证明缺陷不存在」。

## 5. 结论与后续

- 确认 `ICC_IGRPEN1_EL1` 未出现在未处理告警中，与架构判断一致。
- 用例可作为回归守护：若将来有人破坏 VENG1 的映射，用例会失败。
- 若需要更强证明，应另立阶段做破坏性对照，但需先评估侵入性。
