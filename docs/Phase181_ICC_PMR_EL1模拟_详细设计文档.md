# Phase 181：ICC_PMR_EL1 trap-and-emulate 详细设计文档

## 1. 现象

Phase 180 补全诊断信息后，回归日志稳定出现 3 次（VM1/VM2/vCPU 各一次）：

```
system register access is not handled: op0=3 op1=0 crn=4 crm=6 op2=0 dir=0 rt=0
```

`dir=0` 表示 MSR，即 guest **写** `S3_0_C4_C6_0`。该编码对应 `ICC_PMR_EL1`（CPU 接口优先级屏蔽寄存器），与 `include/boards/virt/gicv3.h` 的 `#define ICC_PMR_EL1 S3_0_C4_C6_0` 一致。

guest 在 `guests/el1_test/guest.h` 中通过 `#define icc_pmr_write(v) SYSREG_WRITE(S3_0_C4_C6_0, v)` 写它，调用点在 `main.c:166` 的 `icc_pmr_write(0xFF)`（中断初始化）。

## 2. 根因

不是 guest 非法访问，而是 `ICH_HCR_EL2.TC` 的副作用。

`vgic_init()`（`hypervisor/boards/virt/vgic.c:151`）为 Phase 17 的 vSGI trap-and-emulate 置了 `ICH_HCR_EL2_TC`：

```c
WRITE_SYSREG64(ICH_HCR_EL2_EN | ICH_HCR_EL2_TC, ICH_HCR_EL2);
```

按 GICv3 架构，`TC=1` 并非只捕获 `ICC_SGI1R_EL1`，而是捕获 EL1 对一组 `ICC_*` 寄存器的访问，其中包含 `ICC_PMR_EL1`。`handle_trap_system()` 当时只实现了 SGI1R 分支，其余访问统一落到 WARN 并被丢弃。

后果有二：

1. guest 的 `icc_pmr_write(0xFF)` 被静默丢弃，虚拟优先级屏蔽停留在 `virt_initialize()` 设定的初始值 `0xF8`。
2. 若 guest 读 `ICC_PMR_EL1`，会落到未处理分支，`regs->regs[rt]` 不被写入，返回未定义值。

（本项目 guest 只写不读，第 2 点在当前用例中未被触发，但属于同一缺陷面。）

## 3. 修复方案

把 `ICC_PMR_EL1` 的访问映射到 `ICH_VMCR_EL2.VPMR`。

`include/arch/aarch64/sysregs.h:107` 已明确 VMCR 的定位：「Guest 看到的 ICC_PMR/BPR/IGRPEN1 的后端」，且 VPMR 位于 bits[31:24]（`ICH_VMCR_VPMR_SHIFT 24`）。因此 PMR 的正确模拟位置就是 VMCR 的 VPMR 字段。

### 写路径

在 `handle_trap_system()` 的 MSR 分支、SGI1R 判断之后加入：

```c
if (op0 == 3 && op1 == 0 && crn == 4 && crm == 6 && op2 == 0) {
    unsigned long vmcr = current->virt.gic_vmcr;
    vmcr &= ~(0xFFUL << ICH_VMCR_VPMR_SHIFT);
    vmcr |= (regs->regs[rt] & 0xFFUL) << ICH_VMCR_VPMR_SHIFT;
    current->virt.gic_vmcr = vmcr;
    goto sys_fin;
}
```

更新的是 **保存副本** `current->virt.gic_vmcr`，不是硬件寄存器。这是有意的：

- trap 发生在 guest 世界（EL1）执行期间，此时硬件 `ICH_VMCR_EL2` 属于当前 VM，直接改硬件也「能用」。
- 但 `gic_vmcr` 是 per-VM 状态的唯一权威副本，世界切换时由 `vgic_restore_state()`（`vgic.c:638`）从它写回硬件。只改副本能让修改随 vCPU 一起保存/恢复，与既有状态管理路径保持单一来源。

### 读路径

在 MRS 分支加入对应处理：

```c
if (op0 == 3 && op1 == 0 && crn == 4 && crm == 6 && op2 == 0) {
    regs->regs[rt] = (current->virt.gic_vmcr >> ICH_VMCR_VPMR_SHIFT) & 0xFFUL;
    goto sys_fin;
}
```

两者均置于 `#ifdef PLATFORM_VIRT` 内，与实际提供 vGIC 的平台保持一致。

## 4. 语义影响

`ICC_PMR_EL1` 写入值 `0xFF` 会覆盖 `virt_initialize()` 的初始 VPMR `0xF8`：

- `0xF8`：屏蔽优先级 0xF8–0xFF（最低 8 级），只放行前 31 级。
- `0xFF`：放行全部优先级。

由于 guest 明确请求 `0xFF`，按写入生效后虚拟中断的投递范围**变宽**，而非变窄。回归中 vITS / edu MSI / vSGI 用例全部保持通过，说明该放宽未引入重触发或错误投递。

反过来说，修复前 guest 的屏蔽设置从未生效，属于「配置被静默忽略」。修复后 guest 的配置才是真实的。

## 5. 验证

- `make -B -f Makefile.virt -j2`：exit 0。编译告警为既有项（`virt_mm.h:56`、`ksm.c` 的 int-conversion），本次未新增。
- 35 秒 QEMU 回归（exit 124，符合预期）：

| 指标 | Phase 180 | Phase 181 |
|---|---|---|
| `system register access is not handled` | 4 | **0** |
| `EL2 task stack overflow` | 0 | 0 |
| `shm: PASSED` / `shm: FAILED` | 2 / 0 | 2 / 0 |
| `Phase 19: SOME tests FAILED` | 0 | 0 |
| `vITS: FAILED` | 0 | 0 |

- 关键通过项（vsgi-trap、share vSPI 72、shm VM1 写入、TEST PASSED）数量与上阶段一致。

## 6. 未涉及

- `ICC_BPR1_EL1`、`ICC_IGRPEN1_EL1` 等其他受 `TC` 影响的寄存器：当前 guest 未访问，未触发告警。若后续触发，会以完整编码形式出现在告警中，可据 Phase 180 的诊断信息逐项补全。
- 取消 `TC`：不可行。Phase 17 的 vSGI 依赖它。
