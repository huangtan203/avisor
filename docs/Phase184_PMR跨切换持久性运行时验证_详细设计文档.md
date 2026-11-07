# Phase 184：ICC_PMR_EL1 跨切换持久性运行时验证 详细设计文档

## 1. 目的

Phase 183 修复了「PMR 写入在任务切换后丢失」的缺陷（trap 中只更新保存副本、未写硬件 `ICH_VMCR_EL2`，切换时 `vgic_save_state()` 会用硬件旧值覆盖副本）。

但 Phase 183 的结论完全来自代码路径分析，现有回归无法证明：guest 只写 PMR、不读回，也从不触发切换后比对。本阶段新增运行时用例，把该缺陷从「分析得出」升级为「可复现、可验证」。

## 2. 用例设计

新增 `phase183_pmr_persist_test()`（`guests/el1_test/main.c`），由主核在 `phase18_priority_pending_test()` 之后调用：

1. 写 `ICC_PMR_EL1 = 0xA0`（与初始值 `0xFF` / `0xF8` 均不同，便于区分）。
2. 立即读回，确认写入已生效。
3. 连续 8 次 `hvc_call(HVC_SCHED_YIELD, 0)`，迫使调度器切换到另一个 VM，从而触发 `virt_leaving_vm()` → `vgic_save_state()` → `virt_entering_vm()`。
4. 再读回，与 `0xA0` 比较。
5. 恢复 `ICC_PMR_EL1 = 0xFF`，避免影响后续测试。

选择 `0xA0` 的理由：切回后若读到 `0xF8`（`virt_initialize()` 的初始 VPMR），正好证明副本被硬件旧值覆盖。

## 3. 用例有效性验证（关键）

只有用例能在缺陷存在时失败，它才有证明力。为此做了对照实验：

在**保留用例**的前提下，临时把 `handle_trap_system()` 中的 PMR 处理还原为 Phase 183 之前的「只改副本」版本（去掉 `WRITE_SYSREG64(vmcr, ICH_VMCR_EL2)`，读路径改回读副本），重新构建运行。

结果：

```
[vm1]   PMR after write = 0xa0, after switch = 0xf8 ... FAILED (lost across switch)
[vm2]   PMR after write = 0xa0, after switch = 0xf8 ... FAILED (lost across switch)
```

两个 VM 均失败，且切回值为 `0xf8`，与预期一致——证明用例确实能捕获该缺陷，不是恒真断言。

随后恢复 Phase 183 的修复版本，同一用例变为：

```
[vm1]   PMR after write = 0xa0, after switch = 0xa0 ... PASSED
[vm2]   PMR after write = 0xa0, after switch = 0xa0 ... PASSED
```

## 4. 验证结果

- `make -B -f Makefile.virt -j2`：exit 0。
- 35 秒 QEMU 回归（exit 124）：

| 指标 | 结果 |
|---|---|
| Phase 183 用例（VM1/VM2） | 均 PASSED |
| `EL2 task stack overflow` | 0 |
| `shm: PASSED` / `shm: FAILED` | 2 / 0 |
| `Phase 19: SOME tests FAILED` | 0 |
| `vITS: FAILED` | 0 |
| `system register access is not handled` | 0 |

## 5. 副作用说明

用例末尾恢复 `0xFF`，与 `gic_vcpu_init()` 的初始值一致，因此不影响后续用例的优先级屏蔽行为。

用例运行在 `phase18_priority_pending_test()` 之后，此时 Phase 18 对优先级的操作已结束。

## 6. 结论

Phase 183 的修复得到运行时证实：PMR 写入在跨 VM 切换后保持。缺陷曾真实存在，且已消除。
