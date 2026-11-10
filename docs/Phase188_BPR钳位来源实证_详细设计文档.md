# Phase 188：BPR 钳位来源实证（EL2 读物理寄存器）详细设计文档

## 1. 目的

Phase 187 实测得到一个未被解释的现象：guest 写入 `ICC_BPR0_EL1` 的值 0、1 均读回 2，写入 `ICC_BPR1_EL1` 的 0、1、2 均读回 3。当时提出两种可能：

- **解释 X**：读回值由虚拟 CPU 接口在访问时钳位，物理寄存器无影响。
- **解释 Y**：EL2 的物理 `ICC_BPR0_EL1` 本身就是 2（BPR1 下限 = BPR0 + 1 = 3），guest 读回的是物理值。

本阶段在 EL2 侧直接读取物理寄存器，以判定哪一种解释成立。

## 2. 方法

在 hypervisor 的 HVC 分发中新增只读诊断调用 `HVC_VGIC_BPR_PHYS`（编号 76），返回：

| 返回寄存器 | 内容 |
|---|---|
| x0 | 物理 `ICC_BPR0_EL1`（`S3_0_C12_C8_3`） |
| x1 | 物理 `ICC_BPR1_EL1`（`S3_0_C12_C12_3`） |
| x2 | `ICH_VMCR_EL2` 原始值 |
| x3 | 物理 `ICC_IGRPEN1_EL1`（`S3_0_C12_C12_7`），作为附加读数 |

该调用不修改任何状态，只在 VM1 执行一次。

`ICH_VTR_EL2` 曾作为读数尝试，但这套 QEMU 上该寄存器未实现，读取触发 `SYNC_INVALID_EL2`（`esr=0x2000000`）。已撤回，`hypervisor/boards/virt/pci.c` 第 2546 行有同样的记录。

## 3. 判定标准

| 物理 BPR0 | 结论 |
|---|---|
| 0 | 解释 X 成立，Y 不成立 |
| 2 | 解释 Y 成立 |

## 4. 预期的反例检查

- 读数前后 guest 侧 BPR0 读回应保持一致（钳位现象仍存在），确认诊断调用没有意外改写 VMCR。
- 若 VMCR 中的 `VBPR0` 字段本身等于 2，则说明钳位发生在写入虚拟接口时（已写入 VMCR 的就是钳位后的值），而不是读回时才钳位。

## 5. 实测结果

输出（`/tmp/p188c.out`，VM1）：

```
Phase 188: BPR0 clamp source probe
  phys BPR0=0 phys BPR1=1 phys IGRPEN1=1 VMCR=00000000ff50000a
  guest BPR0 readback=2
```

`ICH_VMCR_EL2` 解码：

| 字段 | 位域 | 值 |
|---|---|---|
| VPMR | [31:24] | 0xff |
| VBPR0 | [23:21] | **2** |
| VBPR1 | [20:18] | **4** |
| VENG1 | [1] | 1 |

## 6. 结论

1. **物理 `ICC_BPR0_EL1` = 0，物理 `ICC_BPR1_EL1` = 1。** 解释 Y 被否定：物理 BPR0 不是 2，BPR1 下限 3 不来自物理寄存器。
2. **VMCR 中的 `VBPR0` 为 2，而 guest 写入的是 0/1。** 这说明读到的 2 来自 VMCR 里保存的值，而 VMCR 不是物理寄存器的镜像。本阶段只观测到这一对照，尚未区分「写入时钳位」与「读回时钳位」，两者需要后续用例（写后不读回、直接查 VMCR）才能分开。
3. **BPR0 下限 2 与 BPR1 下限 3 是两套独立规则。** 若 BPR1 下限由 BPR0 派生（BPR0+1），则 BPR0=2 时应为 3、BPR0=4 时应为 5；Phase 187c 已证明 BPR1 下限恒为 3，本阶段进一步证明它与物理 BPR0 也无关。
4. **具体钳位规则未在本阶段验证。** 本阶段只确定「不来自物理 BPR0」，未确定下限 2/3 的取值依据。

## 7. 与前序阶段的关系

- **Phase 186**：观测到 BPR1 下限 3，当时归因于 `ICC_BPR0_EL1=2`。该归因已在本阶段被否定，Phase 186 文档第 2 节已加更正。
- **Phase 187**：观测到 BPR1 下限不随 guest 侧 BPR0 变化，本阶段解释了原因并否定了「物理 BPR0 决定」的备选解释。

## 8. 风险与边界

- 诊断调用只读，不影响 hypervisor 状态机与回归指标。
- `ICH_VTR_EL2` 在本平台不可读，不能用于读取虚拟接口的优先级位数配置。
- 结论限定于当前 QEMU 版本（`-cpu max`、`gic-version=3`）。硬件平台上物理 BPR0 的复位值可能不同，需重新实测。

## 9. 实现与复现

### 9.1 代码位置（钉版本）

- 实现提交：本阶段代码与本文档在同一提交中，tag 为 `v0.188.0-phase188-bpr-clamp-source`。
- 实现：
  - `include/common/hvc.h`：`HVC_VGIC_BPR_PHYS` 定义（编号 76）
  - `guests/el1_test/guest.h`：guest 侧同编号定义
  - `hypervisor/arch/aarch64/sync_exc.c`：`case HVC_VGIC_BPR_PHYS` 分支
  - `guests/el1_test/main.c`：`phase188_bpr0_physical_test()`，主流程 `测试 11e` 内、`phase187c_bpr1_floor_test()` 之后调用（`main.c` 第 5342 行）

### 9.2 复现步骤

```
make -B -f Makefile.virt -j2
timeout 35 qemu-system-aarch64 -M virt,virtualization=on,gic-version=3,highmem=off \
  -cpu max -smp 2 -m 1G -device edu -kernel bin_virt/kernel8.img \
  -display none -monitor none -serial file:/tmp/p188.out
```

### 9.3 预期输出（节选，实测于 `/tmp/p188c.out`）

```
[vm1] Phase 187c: BPR1 effective floor vs BPR0
[vm1]   BPR0 write=0 readback=2
[vm1]     BPR1 table: 0->3 1->3 2->3 3->3 4->4 5->5 6->6 7->7
[vm1]     BPR0 after table=2 floor=3
[vm1]   BPR0 write=2 readback=2
[vm1]     BPR1 table: 0->3 1->3 2->3 3->3 4->4 5->5 6->6 7->7
[vm1]     BPR0 after table=2 floor=3
[vm1]   BPR0 write=4 readback=4
[vm1]     BPR1 table: 0->3 1->3 2->3 3->3 4->4 5->5 6->6 7->7
[vm1]     BPR0 after table=4 floor=3
[vm1]   restored BPR0=2 BPR1=4
[vm1]
[vm1] Phase 188: BPR0 clamp source probe
[vm1]   phys BPR0=0 phys BPR1=1 phys IGRPEN1=1 VMCR=00000000ff50000a
[vm1]   guest BPR0 readback=2
```

回归指标：`PANIC=0`、`not handled=0`、`FAILED=0`，QEMU 退出码 124（超时结束，正常）。
