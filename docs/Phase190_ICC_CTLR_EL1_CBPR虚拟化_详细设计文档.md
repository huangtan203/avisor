# Phase 190：ICC_CTLR_EL1.CBPR 虚拟化与 BPR1 替代语义

## 1. 背景

Phase 189 判定 BPR 的钳位发生在 guest `msr` 写入虚拟接口的路径上，并测出 BPR0 下限 2、BPR1 下限 3。当时遗留一项未验证：`ICC_CTLR_EL1.CBPR = 1` 时，BPR1 的读写会走另一套语义。

本阶段先按 guest 直写 `ICC_CTLR_EL1` 的方式做验证，发现该寄存器根本没有被 hypervisor 虚拟化：写入触发 `system register access is not handled`，CBPR 无法置位。因此本阶段先补齐 `ICC_CTLR_EL1.CBPR` 的虚拟化，再验证 CBPR=1 时的 BPR1 行为。

## 2. 架构语义

`ICC_CTLR_EL1.CBPR`（bit 0）控制 Group 0 与 Group 1 是否共用一个 binary point 寄存器：

| CBPR | 含义 |
|---|---|
| 0 | `ICC_BPR0_EL1` 只决定 Group 0 的抢占；`ICC_BPR1_EL1` 决定 Group 1 |
| 1 | `ICC_BPR0_EL1` 同时决定 Group 0 和 Group 1；`ICC_BPR1_EL1` 读回 `ICC_BPR0_EL1 + 1`（饱和到 7），写入被忽略 |

在虚拟化侧，该位对应 `ICH_VMCR_EL2.VCBPR`（bit 4）。`HCR_EL2.IMO=1` 时 guest 的 `ICC_CTLR_EL1` 访问映射到 `ICV_CTLR_EL1`，后端即 `ICH_VMCR_EL2.VCBPR`。

## 3. 实现

共三处改动。

### 3.1 新增 VCBPR 位定义

`include/arch/aarch64/sysregs.h` 第 129 行：

```c
#define ICH_VMCR_VCBPR (1UL << 4)  /* ICC_CTLR_EL1.CBPR 的虚拟后端（Phase 190） */
```

### 3.2 写路径

`hypervisor/arch/aarch64/sync_exc.c` 第 640–658 行，接入位置在 `ICC_PMR_EL1` 写分支之后、同一 `#ifdef PLATFORM_VIRT` 块内：

```c
/*
 * Phase 190：ICC_CTLR_EL1.CBPR（bit 0）虚拟化。
 * CBPR 置位时 BPR1 的读写语义改由 BPR0 派生，后端是
 * ICH_VMCR_EL2.VCBPR（bit 4）。与 PMR 一样必须同时更新硬件
 * 寄存器与保存副本，否则世界切换会用旧副本覆盖。
 *
 * 本实现只虚拟化 CBPR；CTLR 的其余位（EOImode、PRIbits 等）
 * 不在此处理，写入被忽略。
 */
if (op0 == 3 && op1 == 0 && crn == 12 && crm == 12 && op2 == 4) {
    unsigned long vmcr = READ_SYSREG64(ICH_VMCR_EL2);
    vmcr &= ~ICH_VMCR_VCBPR;
    if (regs->regs[rt] & 1UL)
        vmcr |= ICH_VMCR_VCBPR;
    WRITE_SYSREG64(vmcr, ICH_VMCR_EL2);
    current->virt.gic_vmcr = vmcr;
    goto sys_fin;
}
```

要点：

- 同时更新硬件 `ICH_VMCR_EL2` 与 `current->virt.gic_vmcr`。原因与 PMR 相同：trap 不经过 `leaving_vm`，若只改副本，下一次世界切换会用硬件旧值覆盖副本。
- 只取 bit 0。CTLR 其余位（EOImode、PRIbits、SEIS 等）本阶段不虚拟化，写入被忽略。这是有意的范围限制，不是遗漏。

### 3.3 读路径

`hypervisor/arch/aarch64/sync_exc.c` 第 669–672 行：

```c
/* Phase 190: 读 ICC_CTLR_EL1 返回虚拟 CBPR，见写分支注释。 */
if (op0 == 3 && op1 == 0 && crn == 12 && crm == 12 && op2 == 4) {
    regs->regs[rt] = (READ_SYSREG64(ICH_VMCR_EL2) & ICH_VMCR_VCBPR) ? 1 : 0;
    goto sys_fin;
}
```

读只返回 CBPR 位，其余位读为 0。guest 侧目前只有本阶段的用例访问该寄存器。

### 3.4 guest 侧用例

`guests/el1_test/main.c` 第 2657–2748 行（`ICC_CBPR_BIT` / `ICH_VMCR_VCBPR_BIT` 两个宏、`ctlr_read`、`ctlr_write`、`phase190_cbpr_test`），调用点第 5495 行。

用例流程（仅 VM1 执行）：

1. 保存 CTLR、BPR0、BPR1 原值。
2. 基线：清 CBPR，`BPR1` 写 6，应读回 6，`VMCR.VCBPR = 0`。
3. 置 CBPR，依次把 `BPR0` 设为 2、4、6，每次 `BPR1` 写 1，观察读回与 `VMCR.VBPR1`。
4. 恢复 CTLR、BPR0、BPR1。

第 3 步中读回值用于验证 `min(VBPR0 + 1, 7)`；`VMCR.VBPR1` 用于验证写入被忽略（应保持为基线写入的 6）。

## 4. 实测输出

`/tmp/p191.out`，VM1：

```
[vm1] Phase 190: CBPR path (ICC_CTLR_EL1.CBPR)
[vm1]   base CBPR=0 BPR1 write=6 readback=6 VMCR.VCBPR=0
[vm1]   CBPR=1 BPR0=2 BPR1 write=1 readback=3 VMCR.VCBPR=1 VMCR.VBPR1=6
[vm1]   CBPR=1 BPR0=4 BPR1 write=1 readback=5 VMCR.VCBPR=1 VMCR.VBPR1=6
[vm1]   CBPR=1 BPR0=6 BPR1 write=1 readback=7 VMCR.VCBPR=1 VMCR.VBPR1=6
[vm1]   CBPR=1 BPR0 write=0 VMCR.VBPR0=2 readback=2 BPR1 readback=3
[vm1]   CBPR=1 BPR0 write=1 VMCR.VBPR0=2 readback=2 BPR1 readback=3
[vm1]   CBPR=1 BPR0 write=2 VMCR.VBPR0=2 readback=2 BPR1 readback=3
[vm1]   CBPR=1 BPR0 write=3 VMCR.VBPR0=3 readback=3 BPR1 readback=4
[vm1]   restored CTLR=0000 BPR0=2 BPR1=4
```

回归：`PANIC=0 notHandled=0 FAILED=0 PASSED=78`，`QEMU_EXIT=124`。

## 5. 结论

1. **CBPR 虚拟化生效**：guest 写 `ICC_CTLR_EL1` 后，EL2 读到的 `ICH_VMCR_EL2.VCBPR` 随之为 1；写入不再报 `not handled`。
2. **CBPR=1 时 BPR1 读回为 `min(VBPR0 + 1, 7)`**：BPR0=2 → 3、BPR0=4 → 5、BPR0=6 → 7，包含饱和边界。
3. **CBPR=1 时 BPR1 写入被忽略**：三次写 1 后 `VMCR.VBPR1` 始终等于基线的 6。
4. **CBPR=0 时 BPR1 正常读写**：写 6 读回 6，与 Phase 187 一致。
5. **CBPR=1 时 BPR0 下限仍为 2**：写 0/1/2 时 `VMCR.VBPR0` 均为 2，写 3 时为 3。与 CBPR=0 下 Phase 189 的结果一致，CBPR 不改变 BPR0 的钳位规则。

第 2、3 条在架构语义（第 2 节）中已有预期，本阶段是首次在本平台实测确认。

## 6. 与 CBPR=0 时的钳位规则的关系

Phase 189 测得的 BPR1 下限 3 是 CBPR=0 下的行为。CBPR=1 时 BPR1 不再有独立的下限，读回值完全由 `VBPR0` 派生（下限为 VBPR0 的下限 2 加 1，饱和到 7）。两者不矛盾：前者是 `VBPR1` 字段自身的钳位，后者是读路径上的派生计算。

## 7. 未验证的部分

- CTLR 其余位（EOImode bit 1、PRIbits bits[10:8]、SEIS bit 3、A3V/IDS 等）未虚拟化，guest 读恒为 0。若后续有 guest 依赖这些位，需要扩展本分支。
- BPR0 下限仅在 CBPR=1 下测试，采样为写 0..3。未测 BPR0 写入 4 以上的下限行为（该区间由 BPR0 ≥ 2 的读回已间接覆盖）。
- 本次串口日志中，VM2 的输出与 VM1 的 `restored CTLR=` 行交错，该行被打断。恢复值 `BPR0=2 BPR1=4` 仍可读出；交错是两个 vCPU 并发打印造成的，不影响测试结果。

## 8. 代码位置与复现

| 内容 | 位置 |
|---|---|
| `ICH_VMCR_VCBPR` 定义 | `include/arch/aarch64/sysregs.h` 第 129 行 |
| 写路径 | `hypervisor/arch/aarch64/sync_exc.c` 第 640–658 行 |
| 读路径 | `hypervisor/arch/aarch64/sync_exc.c` 第 669–672 行 |
| guest 用例 | `guests/el1_test/main.c` 第 2657–2748 行 |
| 调用点 | `guests/el1_test/main.c` 第 5495 行，`测试 11e` 段内、`phase189` 之后 |

复现：

```sh
make -B -f Makefile.virt -j2
timeout 35 qemu-system-aarch64 -M virt,virtualization=on,gic-version=3,highmem=off \
  -cpu max -smp 2 -m 1G -device edu -kernel bin_virt/kernel8.img \
  -display none -monitor none -serial file:/tmp/p190.out
grep -A8 "Phase 190" /tmp/p190.out
```

预期输出与第 4 节一致，且 `grep -c "not handled" /tmp/p190.out` 为 0。

## 9. 提交信息

实现与本文档同一提交，tag 为 `v0.190.0-phase190-ctlr-cbpr`。
