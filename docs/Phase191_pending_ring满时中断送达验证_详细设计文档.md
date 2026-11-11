# Phase 191：pending ring 满时中断送达验证

## 1. 背景

软件注入的虚拟中断先尝试写入 LR1（`vgic_inject_virq_raw` 只循环 `i = 1 .. NR_VGIC_LRS-2`，即只使用 LR1）。LR1 被占用时，中断进入 `VGIC_PENDING_CAPACITY = 32` 的 pending ring。理论容量为 LR1 直存 1 个加 ring 32 个，共 33 个。

本阶段要回答：容量满后，超出的注入是否被丢弃，而已接受的注入是否全部送达。

## 2. 测试设计

guest 用例 `phase191_pending_ring_full_test`（`guests/el1_test/main.c`，仅 VM1 执行）：

1. 屏蔽 IRQ（`msr daifset, #2`）。
2. 连续调用 `vgicd_inject()` 注入 40 个不同 vSPI，编号 `P191_BASE_VIRQ + i`，即 52..91。统计返回 0 的 `accepted` 与返回 -1 的 `rejected`。
3. 放开 IRQ（`msr daifclr, #2`）。
4. 自旋等待，直到送达数等于 `accepted`，或超时。超时为 `160000000` 次 `nop`。
5. 判定两项：
   - 容量：`accepted == 33` 且 `rejected == 7`。
   - 送达：`delivered == accepted`。

送达统计使用位图 `p191_received[2]`，共 64 位 × 2，覆盖 40 个 vINTID。每个 handler 分支调用 `p191_hit(intid)` 置位。

注入路径：`vgicd_inject` → HVC `HVC_GICD_INJECT`（42）→ `vgic_inject_virq()`。该函数先做 GICD SPI enable 门控（默认全部使能），再调用 `vgic_inject_virq_raw()`，成功后调用 `vgic_refill_pending()` 把 ring 头部补进空闲 LR。

## 3. 测量中的既有 INTID 冲突

区间 52..91 与其他测试的中断号重叠，统计时必须纳入这些中断：

| vINTID | 占用者 | 定义位置 | 处理 |
|---|---|---|---|
| 52..55 | `VGICD_TEST_VIRQ` 测试 | `guests/el1_test/guest.h:129` | 分支内补 `p191_hit` |
| 60 | `SHM_NOTIFY_VIRQ` | `guests/el1_test/guest.h:126` | 分支内补 `p191_hit` |
| 72 | `PCI_EDU_IRQ`（vITS 测试映射中断，同时也是 edu MSI） | `guests/el1_test/main.c:1280` | 分支内补 `p191_hit` |

这些 handler 分支先于 P191 独立分支执行，且各自 `return` 或不进入 P191 分支。若不补计入，这三处会被误判为丢失。

对应的 guest handler 分派位于 `guests/el1_test/main.c` 约 5350–5389 行。

## 4. 排查过程

本阶段的初始结论是“ring 满时丢中断”，随后被推翻。排查顺序如下。

**4.1 采样过早（测量问题）**

首次用例只等待约 2000 万次自旋，送达数为 2、6、11、15、17 等不同值，随运行而变。拉长等待后，送达数随时间增长。

**4.2 60/72 误判丢失**

hypervisor 侧 LR 事件日志显示 60 和 72 确实经 LR1 送达并被 ACK，但位图没有计入。原因是 `SHM_NOTIFY_VIRQ` 与 `PCI_EDU_IRQ` 分支先于 P191 分支返回。补入 `p191_hit` 后，送达数上升一致。

**4.3 hypervisor 诊断计数（已回退）**

为确认是否有 LR 覆盖、refill 回推或去重，临时加入计数器与 LR1 事件日志。计数结果：

| 计数器 | 数值 | 含义 |
|---|---|---|
| `push_ok` | 32 | 入 ring 成功 |
| `refill_pop` | 32 | refill 出 ring |
| `push_dup` | 0 | ring 去重命中 |
| `push_fail` | 7 | ring 满拒绝 |
| `raw_dedup` | 0 | LR 去重命中 |
| `refill_back` | 0 | refill 回推 |
| `lr_overwrite` | 0 | LR1 被覆盖 |
| `lr_clear_pending` | 0 | LR1 pending 被清除 |

以上排除了 LR 覆盖、refill 回推与 ring 去重这三类假设。计数器为诊断用途，已全部回退，hypervisor 代码恢复为 HEAD。

**4.4 诊断插桩的副作用**

世界切换时 `vgic_restore_state` 会把所有 LR（包括空闲条目）写回硬件，因此 LR1 事件日志被大量相同的写入填满，需要过滤。该现象属于正常设计，不是缺陷。

## 5. 结论

1. **pending ring 没有丢中断。** 33 个已接受注入全部送达（`delivered = 33`）。
2. **容量为 33。** LR1 直存 1 个，ring 32 个。第 34 个起注入返回 -1，共 7 个被拒绝。
3. **“丢失”结论来自测量错误。** 包括采样过早，以及 52..91 区间与 60/72 既有中断号冲突两方面。
4. **被拒绝的注入是预期行为。** 拒绝由 `vgic_pending_push` 返回 -1 体现，并非静默丢弃，调用方可感知。

## 6. 实测输出

三次连续运行（`/tmp/w1.out`、`/tmp/w2.out`、`/tmp/w3.out`），VM1：

```
[vm1] Phase 191: pending ring capacity (LR1 + 32 slots)
[vm1]   injected=40 accepted=33 rejected=7 delivered=33
[vm1]   capacity=33 (LR1 + 32) PASSED
[vm1]   ring: all accepted interrupts delivered PASSED
```

全量回归（清理诊断插桩后单次运行）：`PANIC=0 notHandled=0 PASSED=80 FAILED=3`，`QEMU_EXIT=124`。3 个 FAIL 均为 vm2 既有用例（冻结请求 `轮次 0`、zombie task 回收、task 槽位复用），与 Phase 191 无关。Phase 191 本身三项判定均 PASSED。

## 7. 已知限制

- 等待窗口 `160000000` 次 `nop` 是经验值。实测 2000 万次时送达不足（11 至 27 个不等），1.6 亿次时稳定为 33。两点之间的阈值未测，不能据此推断最小窗口。
- 本用例只覆盖 LR1 + ring 路径，不覆盖 LR0（vtimer）与 LR2（PCI INTx）。
- 偶发 vm2 Phase 36 `SYNC_INVALID_EL2` PANIC 仅在一次运行出现（`elr=0x41096000`，`esr=0x2000000`），之后 6 次运行未复现，根因未定位，与本阶段无关。

## 8. 代码改动

仅 guest 侧：

- `guests/el1_test/main.c`：`P191_BASE_VIRQ`、`P191_COUNT`、`p191_received`、`p191_hit`、`p191_popcount`、`phase191_pending_ring_full_test`，以及 52/60/72 三处分支中的 `p191_hit` 计入。
- 调用点位于 `main.c` 约 5630 行。

hypervisor 侧无改动。
