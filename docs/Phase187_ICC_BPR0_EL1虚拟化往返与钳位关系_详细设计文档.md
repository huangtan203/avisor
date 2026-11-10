# Phase 187：ICC_BPR0_EL1 虚拟化往返与 BPR1 钳位关系 详细设计文档

## 1. 目的

Phase 186 排查时发现：guest 写 `ICC_BPR1_EL1` 的 1、2 均读回 3，而 `ICC_BPR0_EL1` 读回 2。这说明 BPR1 存在由 BPR0 决定的最小值钳位，但该结论目前仅是推断。

本阶段要解决两件事：

1. `ICC_BPR0_EL1` 的虚拟化路径（`VBPR0`）从未被运行时验证，需要验证它能否跨 VM 切换保持。
2. 把 Phase 186 的钳位结论从推断变为实测：写入 BPR0 后，BPR1 的有效下限随之变化。

本阶段仅产出设计，不修改 hypervisor 实现，也不改动 guest 用例代码。

## 2. 编码与位域

- `ICC_BPR0_EL1`：`op0=3, op1=0, CRn=12, CRm=8, op2=3` → `S3_0_C12_C8_3`
- `ICC_BPR1_EL1`：`S3_0_C12_C12_3`（Phase 186 已核实）
- `BinaryPoint` 位于 bits[2:0]，两个寄存器相同。
- `HCR_EL2.IMO=1` 时，EL1 访问 `ICC_BPR0_EL1` 映射到 `ICV_BPR0_EL1`，后端为 `ICH_VMCR_EL2.VBPR0`。
- `VBPR0` 位于 `ICH_VMCR_EL2` bits[23:21]。该位置来自 Arm 参考页面对 ICH_VMCR 的描述（AArch32 视图），AArch64 编码以官方手册为准，实现前需复核。

**仓库现状**：`include/arch/aarch64/sysregs.h` 仅定义了 `ICH_VMCR_VENG1` 与 `ICH_VMCR_VPMR_SHIFT`，没有 VBPR0 或 VBPR1 的位域宏。`hypervisor/boards/virt/vgic.c` 与 `virt_board.c` 初始化 VMCR 时写入 `(0xFF << 24) | VENG1`，即 VBPR0 与 VBPR1 初值均为 0。

## 3. 待验证假设

| 编号 | 假设 | 验证方式 |
|---|---|---|
| H1 | BPR0 读回存在架构下限，写 0/1 读回 2，有效下限为 2 | **已支持**（187a 实测） |
| H2 | 初值冲突：VMCR 初值 VBPR0=0，但 guest 读到 2，需区分"硬件下限"与"初值" | 在未写入 BPR0 的状态下读取，与映射表对照（未单独验证，见第 9 节） |
| H3 | BPR0 写入经 `VBPR0` 保存/恢复，跨 VM 切换后保持 | **已支持**（187b 实测，两侧均 PASSED） |
| H4 | 写 BPR0 后，BPR1 的最小值随之变化（下限 = BPR0 + 1） | **已否定**（187c 复核：BPR0=0/2/4 下 BPR1 下限均为 3） |

H4 的否定是本阶段的主要结论。Phase 186 基于 `BPR0=2` 推断"BPR1 下限 = BPR0+1 = 3"，这个数值碰巧正确，但推断的机制（BPR1 下限依赖 BPR0）不成立。

## 4. 用例设计

### 4.1 用例 187a：BPR0 映射表（单 VM）

- 仅由 VM1 执行，VM2 跳过。
- 依次写 BPR0 = 0..7，每次 `isb` 后读回 bits[2:0]，打印 `写入 -> 读回`。
- 不做 VM 切换，用于建立 BPR0 的读回函数，验证 H1、H2。

### 4.2 用例 187b：BPR0 跨切换往返（双 VM）

- 先按 187a 的映射表选取两组互不重叠、且都落在 BPR0 有效读回范围内的探测值：VM1 取一组，VM2 取另一组。
- 共 2 轮，每轮写入探测值，`isb`，连续 8 次 `hvc_call(HVC_SCHED_YIELD, 0)` 迫使切换，再读回两次。
- 任一读回不等于本轮探测值即判 FAILED。
- 同时打印 `[pcpu 起始->结束]`，沿用 Phase 186 的方式证明两 VM 同核。

**注意**：BPR0 是每个物理核一份的 VMCR 字段，若 VM2 在 VM1 的 187a 期间也写 BPR0，映射表会被污染。因此 187a 必须只在 VM1 执行，且 187b 之前不得有其他用例修改 BPR0。

### 4.3 用例 187c：BPR0 对 BPR1 下限的影响（单 VM）

- 仅由 VM1 执行。
- 对 BPR0 取 0、2、4 三个样本。每个样本先写 BPR0 并读回，再遍历 BPR1 = 0..7 写入并读回，记录完整读回表。
- 每个样本遍历结束后再读一次 BPR0，确认循环未改写 BPR0。
- 结束时恢复执行前的 BPR0 与 BPR1。
- 用于验证 H4。第一版只输出 BPR1 下限一项，复核时改为输出完整表以排除 BPR0 被循环改写的干扰。

## 5. 判定标准

| 用例 | PASSED 条件 |
|---|---|
| 187a | 映射表可获得，且写 0/1/2 读回 2、写 3..7 原样读回（即 BPR0 有效下限为 2） |
| 187b | 所有轮次读回等于本轮探测值，两 VM 均 PASSED |
| 187c | 不以 PASSED/FAILED 判定，产出为 BPR1 读回表。当前实测不支持 H4，结论记入第 9 节 |

187a 与 187c 不以 PASSED/FAILED 判定，产出为实测数据。187b 是唯一有通过/失败判定的用例。

## 6. 破坏性对照（实现阶段执行）

与 Phase 186 相同，在 `hypervisor/boards/virt/vgic.c` 中临时注入后必须还原。

- **破坏 A**：`vgic_save_state` 不读取 `ICH_VMCR_EL2`。预期 187b 有 VM 检出失败。
- **破坏 B**：`vgic_save_state` 与 `vgic_restore_state` 均不处理 `ICH_VMCR_EL2`。预期 187b 两侧均检出失败。

两类破坏下均应能检出 187b，否则说明 187b 的探测值或轮次设计不足。

## 7. 风险与边界

- **Group 0 影响**：修改 BPR0 会影响 Group 0 的抢占判断。当前 guest 仅使用 Group 1 中断，但实现前需确认 Phase 185/186 之后的测试不依赖 Group 0。
- **恢复原值**：BPR0 的复位值为 UNKNOWN，实验后不保证恢复。与 Phase 186 一致，后续用例不依赖该字段。
- **范围外**：不改 hypervisor 对 VBPR0 的处理逻辑；不做 Group 0 中断投递；若破坏实验发现实现缺陷，另起 phase 修复。

## 8. 与前序阶段的关系

- Phase 186 的 `BPR0=2` 与"BPR1 下限 = BPR0+1"的推断，由本阶段验证：BPR0 下限为 2（H1 成立），但 BPR1 下限不随 BPR0 变化（H4 否定）。
- Phase 186 中关于钳位机制的表述需要修正：BPR1 的下限为 3 是实测事实，而其依据（BPR0 决定下限）不成立。

## 9. 实测结论

1. **BPR0 有效下限为 2**：写 0/1/2 均读回 2，写 3..7 原样读回（187a）。
2. **BPR0 跨切换保持**：两 VM 各两轮均 PASSED，且同在 pCPU 0（187b）。
3. **BPR1 下限恒为 3，与 BPR0 无关**（187c）：

   | BPR0 写入 | BPR0 读回 | BPR1 读回表（写 0..7） | 下限 |
   |---|---|---|---|
   | 0 | 2 | 3,3,3,3,4,5,6,7 | 3 |
   | 2 | 2 | 3,3,3,3,4,5,6,7 | 3 |
   | 4 | 4 | 3,3,3,3,4,5,6,7 | 3 |

4. **未确定的解释**：BPR1 下限可能由**物理** `ICC_BPR0_EL1` 决定，而 guest 写 BPR0 只更新 `ICH_VMCR_EL2.VBPR0`，不改物理寄存器。若物理 BPR0 为 2，则下限为 3，与全部样本吻合。该解释只能在 EL2 侧读取物理 BPR0 才能证实，本阶段未验证，不作为结论。

## 10. 实现与复现

### 10.1 代码位置

- 实现：`guests/el1_test/main.c`，段落 `Phase 187` 起，包含 `bpr0_read/bpr0_write/bpr1_read/bpr1_write` 与 `phase187a_bpr0_map_test`、`phase187b_bpr0_roundtrip_test`、`phase187c_bpr1_floor_test`。
- 调用：主流程 `测试 11e`，位于 `phase186_bpr1_roundtrip_test()` 之后、`phase19_injection_unify_test()` 之前。
- hypervisor 无改动，`hypervisor/` 下本阶段不产生 diff。

### 10.2 复现步骤

```
make -B -f Makefile.virt -j2
timeout 35 qemu-system-aarch64 -M virt,virtualization=on,gic-version=3,highmem=off \
  -cpu max -smp 2 -m 1G -device edu -kernel bin_virt/kernel8.img \
  -display none -monitor none -serial file:/tmp/p187.out
```

退出码 124 为正常（超时结束）。检查：`PANIC=0`、`not handled=0`，Phase 186 两侧 PASSED。

### 10.3 预期输出（节选，实测于 `/tmp/p187final.out`）

```
Phase 187a: ICC_BPR0_EL1 readback map
  BPR0 write=0 -> 2   write=1 -> 2   write=2 -> 2   write=3 -> 3
  write=4 -> 4   write=5 -> 5   write=6 -> 6   write=7 -> 7
  BPR0 restored=2
Phase 187b: ICC_BPR0_EL1 virtualization round-trip
  [vm1] round 0: write=2 -> 2, again -> 2 [pcpu 0->0] ... ok
  [vm1] round 1: write=3 -> 3, again -> 3 [pcpu 0->0] ... ok
  [vm1] BPR0 PASSED
  [vm2] round 0: write=4 -> 4, again -> 4 [pcpu 0->0] ... ok
  [vm2] round 1: write=5 -> 5, again -> 5 [pcpu 0->0] ... ok
  [vm2] BPR0 PASSED
Phase 187c: BPR1 effective floor vs BPR0
  BPR0 write=0 readback=2   BPR1 table: 0->3 1->3 2->3 3->3 4->4 5->5 6->6 7->7
  BPR0 write=2 readback=2   BPR1 table: 0->3 1->3 2->3 3->3 4->4 5->5 6->6 7->7
  BPR0 write=4 readback=4   BPR1 table: 0->3 1->3 2->3 3->3 4->4 5->5 6->6 7->7
```

以上节选逐字取自 `/tmp/p187final.out`，与第 9 节实测表一致。

### 10.4 提交说明

第一版提交 `2f9349a` 仅含设计文档，不含代码。本阶段补交的实现提交包含 `guests/el1_test/main.c` 与本文档，tag 指向该实现提交。
