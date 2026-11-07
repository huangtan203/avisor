# Phase 177：MAPTI 物理下发失败时设置 halt_reason 详细设计文档

## 1. 问题

`vits_cmd_mapti()` 在 `its_send_mapti()` 物理下发失败时，只回滚并返回 -1，**没有设置 `vits->halt_reason`**。

`halt_reason` 不随 `halted` 清除（`halted = 0` 时它保持原值），只在下一次失败时被覆写。因此物理失败后，软寄存器 ERR_STATUS[15:8] 报告的是**上一次失败的原因码**，而不是本次真实原因。

这与 Phase 171 修复的 "读取失败未复位原因码" 属于同一类缺陷。

## 2. 修复

在 `vits_cmd_mapti()` 的物理失败分支中补上原因码：

```c
if (its_send_mapti(vdevid, plpi, veventid, vicid) < 0) {
    vits->halt_reason = VITS_ERR_OTHER;
    vits_mapti_rollback(vits, plpi, have_prev, &prev_route);
    return -1;
}
```

选用 `VITS_ERR_OTHER`：物理 ITS 的失败没有比 "其它" 更具体的 vITS 原因码，且 `its_send_mapti` 的返回值只有 -1，无法再细分。

## 3. 范围

本阶段只修复 `its_send_mapti()` 物理失败这一路径，**不**扩大到其它失败分支。

仍未设置 `halt_reason` 的同类失败点（已核实，留待后续阶段）：

- 反查表满：`WARN` 后直接 `return -1`。
- 无空闲物理 LPI：`vits_reserve_plpi` 失败后直接 `return -1`。
- `irq_domain_register` 失败、`vits_lpi_audit_owned_range` 审计失败。

这些路径同样依赖旧 `halt_reason`，应作为独立阶段统一处理，以免本次改动过宽。

## 4. 验证

- 构建 `make -B -f Makefile.virt -j2` 通过（exit 0）。
- `git diff --check` 通过。
- 35 秒 QEMU 回归（`/tmp/phaseH.log`）：
  - `EL2 task stack overflow`：0
  - `shm: PASSED`：2，`shm: FAILED`：0
  - `Phase 19: SOME tests FAILED`：0
  - `vITS: PASSED! Received vSPI 72`、`share vSPI 72, both paths still deliver`：通过
- 本次未能在 QEMU 中直接触发物理 MAPTI 失败，因此该修复的效果由代码路径审查确认，而非运行时证明。

## 5. 说明

回归中 `grep stalled` 命中 2 处，经核实来自 `installed`（异常向量安装日志），与 ITS 队列无关；上一轮基线同样为 2，属于 grep 模式过宽的误报，不是新问题。
