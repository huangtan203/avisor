# 迁移与任务回收路径间歇失败观察

## 1. 背景

vm2/vm3 的 Phase 194/196 探针曾出现间歇 `eoir_done=0`，经复核后 194/196 在当前代码上未复现（见 Phase194 文档第 10 节）。复核过程中，同批运行还观察到若干其它路径的低频失败。本文件只记录观察，不包含根因结论，也不包含代码改动。

## 2. 观察方法

- 代码：当前 `HEAD`（无诊断插桩）。
- 运行：`timeout 35 qemu-system-aarch64 -M virt,virtualization=on,gic-version=3,highmem=off -cpu max -smp 2 -m 1G -device edu -kernel bin_virt/kernel8.img -display none -monitor none`，退出码 124 为正常。
- 样本：两批，分别为 20 轮（Phase194 复核）与 30 轮（Test 21 复核）。

## 3. 观察结果

| 批次 | 轮次 | 失败项 | 所在 VM | 次数 |
|---|---|---|---|---|
| A | 20 | Test 21 `迁移后 vCPU1 未继续推进 (213384 -> 213384)` | vm3 | 1 |
| A | 20 | Test 22 `FAILED`（Test 21 失败的连带输出） | vm3 | 1 |
| B | 30 | `冻结请求失败，轮次 0` | vm2 | 1 |
| B | 30 | `zombie task 未在切栈后回收` | vm2 | 1 |
| B | 30 | `task 槽位未被实际复用` | vm2 | 1 |
| B | 30 | `SYNC_INVALID_EL2`（`esr=0x2000000`）PANIC | vm1 | 1 |

批次 B 中，后三条 FAIL 出现在同一轮（run27）；PANIC 出现在另一轮（run21）。Test 21 在批次 B 的 30 轮中 **0 次**失败。

### 3.1 关于 Test 22 的更正

批次 A 中 Test 22 显示 `FAILED`，最初误以为它本身失败。实际上 Test 22 的 `PASS: 源 VM2 资源已回收 (35 页)` 正常输出，`FAILED` 是同一函数 `test_phase25_migration()` 的收尾打印，其 `passed` 已因 Test 21 被置 0。Test 22 自身不失败。

### 3.2 PANIC 现场

`!!! PANIC[1]: uncaught exception(SYNC_INVALID_EL2) esr: 2000000, elr: 4108a358, far: 40300040 x30: 4108a358 sp: 4108bea0`

与 Phase191 文档第 8 节记录的历史 PANIC（`esr=0x2000000`，`elr` 落在 task 区间）同类。本次 `elr` 为 `0x4108a358`，未与符号表对照。

## 4. 已排除与未排除

**已确认**（当前代码，实测）：
- Phase 194/196 探针：20 轮未复现。
- Test 21：30 轮未复现（批次 A 中 1 次失败的真实性由该批日志证实，见 3 节）。

**未确认**：
- 上述各失败的根因。
- 失败与 QEMU 双核调度的关系。这是推测，未验证。
- Test 21 的 8M nop 等待窗口是否足够。未做消融实验。

## 5. 后续

本次收尾不改代码。若要定位，需要按失败项单独立项，并在失败现场抓取更完整的上下文（例如 `task` 状态、冻结轮次计数）。失败率低（约 1/20 至 1/30），单次复现成本较高。
