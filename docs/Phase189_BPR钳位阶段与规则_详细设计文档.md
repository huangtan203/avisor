# Phase 189：BPR 钳位发生阶段与钳位规则 详细设计文档

## 1. 目的

Phase 188 已确定：guest 写入 `ICC_BPR0_EL1` 得到的 2 不来自物理 `ICC_BPR0_EL1`（实测物理值为 0），而 `ICH_VMCR_EL2` 中的 `VBPR0` 已经是 2。但当时无法区分两种可能：

- **写入时钳位**：`msr` 在更新 `ICH_VMCR_EL2` 时就已写入钳位后的值。
- **读回时钳位**：VMCR 中存的是原值，`mrs` 读回时才做钳位。

本阶段判定钳位发生在哪一侧，并确定钳位下限的取值规则。

## 2. 方法

关键点：**写完立刻由 EL2 读 `ICH_VMCR_EL2`，中间不做任何 guest 读回**。

若 VMCR 中已是钳位值，说明钳位在写入路径；若仍是原值，说明在读回路径。

复用 Phase 188 的 `HVC_VGIC_BPR_PHYS`（编号 76）取其 x2 = `ICH_VMCR_EL2` 原始值，不新增 HVC。

每个样本的顺序固定为：

1. `bpr0_write(w)`（含 `isb`）
2. `vmcr_raw()` → 取 `VBPR0`（此时尚未读回）
3. `bpr0_read()` → 取 guest 侧读回值
4. `vmcr_raw()` → 再次取 `VBPR0`，确认读回不改变 VMCR

BPR0、BPR1 各测样本 `{0,1,2,3}`。结束恢复执行前的 BPR0/BPR1。

## 3. 判定标准

| 步骤 2 的 VMCR 字段 | 结论 |
|---|---|
| 已是钳位值 | 钳位发生在写入路径 |
| 等于写入值 | 钳位发生在读回路径 |

## 4. 实测结果

`/tmp/p189b.out`，VM1：

```
Phase 189: BPR clamp stage (write vs readback)
  BPR0 write=0 VMCR.VBPR0 before readback=2 readback=2 VMCR.VBPR0 after=2
  BPR0 write=1 VMCR.VBPR0 before readback=2 readback=2 VMCR.VBPR0 after=2
  BPR0 write=2 VMCR.VBPR0 before readback=2 readback=2 VMCR.VBPR0 after=2
  BPR0 write=3 VMCR.VBPR0 before readback=3 readback=3 VMCR.VBPR0 after=3
  BPR1 write=0 VMCR.VBPR1 before readback=3 readback=3 VMCR.VBPR1 after=3
  BPR1 write=1 VMCR.VBPR1 before readback=3 readback=3 VMCR.VBPR1 after=3
  BPR1 write=2 VMCR.VBPR1 before readback=3 readback=3 VMCR.VBPR1 after=3
  BPR1 write=3 VMCR.VBPR1 before readback=3 readback=3 VMCR.VBPR1 after=3
  restored BPR0=2 BPR1=4
```

回归指标：`PANIC=0`、`not handled=0`、`FAILED=0`。

## 5. 结论

### 5.1 钳位发生在写入路径

每个样本的「before readback」一列已经等于钳位值，且读回前后 VMCR 不变。因此 guest 的 `msr ICC_BPR0_EL1` 在更新 `ICH_VMCR_EL2` 时就写入了钳位后的值，`mrs` 只是原样取出字段。Phase 188 遗留的「写入时 vs 读回时」问题由此判定。

### 5.2 钳位下限的规则

实测下限：

| 寄存器 | 写入 0..下限-1 时 VMCR 的值 | 下限 |
|---|---|---|
| `ICC_BPR0_EL1` | 写 0/1 → VMCR 为 2 | **2** |
| `ICC_BPR1_EL1` | 写 0/1/2 → VMCR 为 3 | **3** |

这与 QEMU 参考实现 `hw/intc/arm_gicv3_cpuif.c` 的 `write_vbpr()` 形式一致（以下为参考实现的摘录，非本平台实测）：

```c
static void write_vbpr(GICv3CPUState *cs, int grp, int value)
{
    int min = icv_min_vbpr(cs);      /* = 7 - cs->vprebits */

    if (grp != GICV3_G0)
        min++;                        /* BPR1 下限 = BPR0 下限 + 1 */

    value = MAX(value, min);
    /* deposit64 到 ICH_VMCR_EL2 的 VBPR0/VBPR1 字段 */
}
```

若该公式在本平台成立，则 BPR0 下限 `7 - vprebits = 2` 意味着 `vprebits = 5`，BPR1 下限 `2 + 1 = 3`，与实测吻合。但 `vprebits` 本身未被直接读取（见 5.4），因此这是**与实测相容的推断**，不是已证实的结论。

对应的 `read_vbpr()` 只是 `extract64` 取出字段，不做任何钳位，与 5.1 的实测一致。

### 5.3 BPR1 下限与当前 VBPR0 的关系

实测中 BPR1 下限恒为 3，不随当前 `VBPR0` 变化：Phase 187c 把 BPR0 改为 0/2/4，Phase 188 测得物理 BPR0 为 0，两次下限都是 3。参考实现的公式中，BPR1 下限由常量 `(7 - vprebits) + 1` 决定，与当前 `VBPR0` 的值无关，与上述实测一致。

### 5.4 未验证的部分

- `vprebits = 5` 是由钳位值反推的，未能直接读取。本平台 `ICH_VTR_EL2` 未实现（见 Phase 188 第 2 节），读它会触发 `SYNC_INVALID_EL2`。
- 未测试 `ICC_CTLR_EL1.CBPR=1` 时 BPR1 的替代语义（读返回 `VBPR0+1` 饱和到 7、写被忽略）。该路径在 QEMU 实现中存在，但不属于「钳位规则」本身。

## 6. 与前序阶段的关系

- **Phase 186**：BPR1 下限 3 被误归因为 `ICC_BPR0_EL1=2`，已在本阶段彻底澄清。
- **Phase 187**：BPR1 下限不随 guest 侧 BPR0 变化，本阶段给出成因。
- **Phase 188**：排除物理 BPR0 来源，本阶段补齐「写入时钳位」这一环。

## 7. 实现与复现

### 7.1 代码位置（钉版本）

- 实现提交：本阶段代码与本文档在同一提交中，tag 为 `v0.189.0-phase189-bpr-clamp-stage`。
- 实现：
  - `guests/el1_test/main.c`：`vmcr_raw()` 与 `phase189_bpr_clamp_stage_test()`，主流程 `测试 11e` 内、`phase188_bpr0_physical_test()` 之后调用
  - 复用 `include/common/hvc.h` 与 `guests/el1_test/guest.h` 中的 `HVC_VGIC_BPR_PHYS`（编号 76）
- hypervisor 本阶段无改动。

### 7.2 复现步骤

```
make -B -f Makefile.virt -j2
timeout 35 qemu-system-aarch64 -M virt,virtualization=on,gic-version=3,highmem=off \
  -cpu max -smp 2 -m 1G -device edu -kernel bin_virt/kernel8.img \
  -display none -monitor none -serial file:/tmp/p189.out
```

### 7.3 预期输出（节选，实测于 `/tmp/p189b.out`）

见第 4 节。检查点：

- 所有 `before readback` 与 `after` 两列相等，且等于该寄存器的钳位下限或写入值本身。
- BPR0 写 0/1/2 → 2；写 3 → 3。
- BPR1 写 0/1/2 → 3。
- 回归 `PANIC=0`、`not handled=0`、`FAILED=0`，QEMU 退出码 124（正常超时结束）。
