# Phase 200：物理侧 PPI 27 DIR 验证（阶段 A 风险分析 + 阶段 B 实测）

## 1. 目标

Phase 199 指出：HW 映射 LR 的物理 DIR 只在 `virt_leaving_vm()` 中条件性发出，guest 无法直接观测物理 GIC 状态。本阶段验证：物理 PPI 27 真实处于 active 时，经 leaving 判断后物理 active 是否被去活。

## 2. 阶段 A：只读核实（代码 + 启动日志）

- hypervisor 定时器为 `timer_hyp_ppi = 26`（`hypervisor/common/platform.c:902`、`1010`），由 `handle_timer_irq()` 处理（`hypervisor/boards/virt/irq.c:206–207`），与 PPI 27 无共用分派。
- guest 虚拟定时器为 `timer_virt_ppi = 27`（`platform.c:903`、`1011`）。`irq.c:211–215` 对 PPI 27 只做 `vgic_inject_virq_hw(irq, irq)` 与 `gic_eoi_irq`，不做物理 DIR。
- QEMU 启动日志 `timer-ppi=26/27`，与代码默认值一致（`/tmp/p200a_run.out`，`PANIC=0`）。
- 结论：应答 PPI 27 不进入 hypervisor tick 路径。阶段 A 原先“应答 PPI 27 会影响 hypervisor 定时器”的判断不成立，已更正。

## 3. 阶段 B：实现

### 3.1 代码改动

- `hypervisor/boards/virt/virt_board.c`
  - 抽取 `static void vtimer_hw_lr_deactivate(uint64_t lr0)`：LR0 为 HW 映射、非 invalid、pINTID==27 时发出 `ICC_DIR_EL1`。`virt_leaving_vm()` 改为调用它。
  - 新增只读/测试接口：`virt_test_phys_ppi27_active()`（读 `GICR_ISACTIVER0` bit27）、`virt_test_ppi27_ack()`（写 `GICR_ISPENDR0` bit27 后读 `ICC_IAR1_EL1`，不做 EOI/DIR）、`virt_test_ppi27_clear()`（显式 EOI+DIR）、`virt_test_leaving_lr0_dir()`（读硬件 LR0 后调用 `vtimer_hw_lr_deactivate`，返回物理 active）。
- `hypervisor/arch/aarch64/sync_exc.c`：HVC 81–85 分派。
- `include/common/hvc.h`、`guests/el1_test/guest.h`：HVC 81–85 定义（两处同步）。
- `include/boards/virt/virt_board.h`：上述四个接口原型。
- `guests/el1_test/main.c`：`probe_ppi27_phys_dir()`，在 `guest_main` 中 `probe_hw_lr2_dir()` 之后调用。

### 3.2 探针流程

1. 屏蔽 IRQ，写 `CNTV_CTL_EL0=0` 关闭本 VM 的 vtimer。
2. HVC 81 读物理 active → `phys_before`。
3. HVC 82 写 `ISPENDR0` 并应答 → `ack_intid`，保持 active → HVC 81 → `phys_after_ack`。
4. HVC 83 注入 HW LR0（pINTID=27）→ guest 虚拟 IAR → HVC 81 → `phys_after_iar`。
5. HVC 84 读硬件 LR0 并调用 leaving 判断 → 返回 `phys_after_leaving`。
6. HVC 85 显式 EOI+DIR 清理 → HVC 81 → `phys_after_clear`。

### 3.3 设计修正（由实测推翻）

- 初版设计假定 `ICC_IAR1_EL1` 可直接应答 PPI 27。实测 `ack_intid=03ff`（1023，spurious），即物理 PPI 27 未 pending，IAR 无中断可返回。
- 修正：HVC 82 先写 `GICR_ISPENDR0` bit27 使其 pending，再读 IAR。修正后 `ack_intid=001b`（27）。

## 4. 实测结果

构建：`build=0`。QEMU 回归：`timeout 35`，退出码 124。

| 轮次 | VM | phys_before | ack_intid | phys_after_ack | phys_after_iar | phys_after_leaving | phys_after_clear | 判定 |
|---|---|---|---|---|---|---|---|---|
| p200c run1 | vm1 | 00 | 001b | 01 | 01 | 00 | 00 | 有效 |
| p200c run1 | vm2 | 00 | 001b | 01 | 01 | 00 | 00 | 有效 |
| p200c run2 | vm2 | 00 | 001b | 01 | 01 | 00 | 00 | 有效 |
| p200c run2 | vm1 | 01 | 03ff | 01 | 01 | 00 | 00 | 无效（前置条件被干扰） |

有效样本（`phys_before=00`）中，`phys_after_ack=01`、`phys_after_leaving=00`，即 leaving 的 DIR 判断使物理 active 由 1 降为 0。该结论在 3 个有效样本中一致。

### 4.1 干扰与边界

- PPI 27 是两个 VM 共享的物理中断。vm1 的 vtimer 路径可能在 vm2 测试前使物理 PPI 27 处于 active（run2 中 vm1 的 `phys_before=01`，ack 失败）。
- 本探针只在本 VM 内关闭 vtimer，不能约束另一个 VM。因此只使用 `phys_before=00` 的样本判定，`phys_before=01` 的样本记为无效。
- 样本数有限（3 个有效样本，2 轮 QEMU）。未做更大规模的复现。

### 4.2 同轮的其他问题

- 两轮各出现 `FAIL`（2–3 条），来自 Phase 194/196 既有的 DIR 探针（`ICC_DIR_EL1 semantics`、`no-match`）。基线为 0，本次未确认是否与 Phase 200 改动相关。
- run1 中 vm2 在 Test 24（Phase 36 FINISH Abort/Rollback）出现 `PANIC[1]: SYNC_INVALID_EL2`，`elr=400b21c8`。这是 Phase 36 t8 已记录的偶发问题，发生在 Phase 200 探针之后，未确认与本改动有关。

### 4.3 FAIL 来源的删减实验

每个变体只改动探针函数体，其余代码不变。统计 `FAIL` 与 `PANIC` 出现的轮数。

| 变体 | 轮数 | FAIL>0 轮数 | PANIC |
|---|---|---|---|
| 完整探针 | 10 | 10 | 0 |
| 注释探针调用 | 10 | 0 | 0 |
| 空函数体（保留调用） | 10 | 0 | 0 |
| 仅 `daifset/daifclr` | 10 | 0 | 0 |
| 去掉 cntv 写入 | 10 | 10 | 0 |
| 去掉 HVC 82 | 10 | 10 | 0 |
| 仅 vm2 执行 | 10 | 8 | 0 |
| 去掉 `icc_iar1_read()` | 10 | 1 | 1 |
| 去掉 HVC 83 | 20 | 0 | 0 |
| 去掉 HVC 84 | 20 | 20 | 1 |
| 去掉 HVC 85 | 20 | 20 | 0 |
| 只保留 HVC 83 | 10 | 1 | 0 |
| 去掉 HVC 83 + `icc_iar1_read()` | 10 | 0 | 0 |

结论（仅限本组实验）：
- HVC 83（注入 pINTID=27 到 LR0）是 FAIL 的必要条件：去掉后 20/20 轮无 FAIL。
- HVC 83 不是充分条件：只保留它时 FAIL 仅 1/10。
- HVC 84、85 不是必要条件：分别去掉后 FAIL 仍每轮出现。
- 去掉 `icc_iar1_read()` 后 FAIL 基本消失，但同轮出现过 1 次 PANIC（Phase 36 task 栈类问题），未确认与探针有关。

边界：
- 每个删减都改变了探针的时序与语义，单项结论带有时序干扰，不能直接等同于因果机制。
- 本组实验未直接用日志证实“LR0 残留导致 vm1 的 `received=0`”这一机制，目前仅为代码推断。
- 本组实验仅在 QEMU 上进行。
- PANIC 出现次数低（0–1/20），不足以判断其与探针的关联。

## 5. 未验证的部分

- 有效样本仅 3 个，未验证统计稳定性。
- 未验证 `phys_after_leaving=00` 是否完全由 `vtimer_hw_lr_deactivate` 的 DIR 引起。`phys_after_leaving` 由 HVC 84 在 EL2 内部先去活再读取 active，两步之间没有 guest 介入，但未设置“不执行 DIR 的对照组”，因此不能排除其他副作用。
- 未验证真实 VM 切换路径（`virt_leaving_vm` 调用）下的同等效果。HVC 84 为人工触发，与真实切换的调用时机不同。
- 未验证两个 VM 互斥时的正确行为（干扰问题未解决）。
- FAIL 与 PANIC 的关联性未确认。

## 6. 代码位置

| 文件 | 内容 |
|---|---|
| `hypervisor/boards/virt/virt_board.c` | `vtimer_hw_lr_deactivate()`、`virt_test_*()`、`virt_leaving_vm()` |
| `hypervisor/arch/aarch64/sync_exc.c` | HVC 81–85 分派 |
| `include/common/hvc.h` | HVC 81–85 定义 |
| `guests/el1_test/guest.h` | HVC 81–85 定义（与 hvc.h 同步） |
| `guests/el1_test/main.c` | `probe_ppi27_phys_dir()` |
| `include/boards/virt/virt_board.h` | 测试接口原型 |
