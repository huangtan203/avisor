# Phase 183：ICC_PMR_EL1 模拟写入硬件修正 详细设计文档

## 1. 背景

Phase 181 通过 trap-and-emulate 模拟了 guest 对 `ICC_PMR_EL1` 的读写，仅修改了保存副本 `current->virt.gic_vmcr`，未写硬件 `ICH_VMCR_EL2`。Phase 181 的回归全部通过，但当时未验证跨世界切换后的持久性。

## 2. 缺陷

`virt_leaving_vm()`（`hypervisor/boards/virt/virt_board.c:670`）在任务切换时先调用 `vgic_save_state()`，用**硬件** `ICH_VMCR_EL2` 覆盖 `tsk->virt.gic_vmcr`。

PMR trap 发生在 guest 运行期间，不经过 `leaving_vm`（该回调只在 `sched.c:483` 的任务切换路径上调用）。因此：

1. guest 写 PMR → 只改副本 → 硬件 VMCR 仍是旧值。
2. 发生切换 → `vgic_save_state` 用硬件旧值覆盖副本 → **写入丢失**。

结论：Phase 181 的修复只在「同一 vCPU 持续运行、从未切换」时有效。这是我在 Phase 181 中未覆盖的场景。

## 3. 修复

trap 中同时写硬件与副本：

```c
unsigned long vmcr = READ_SYSREG64(ICH_VMCR_EL2);
vmcr &= ~(0xFFUL << ICH_VMCR_VPMR_SHIFT);
vmcr |= (regs->regs[rt] & 0xFFUL) << ICH_VMCR_VPMR_SHIFT;
WRITE_SYSREG64(vmcr, ICH_VMCR_EL2);
current->virt.gic_vmcr = vmcr;
```

读路径改为读硬件 VPMR，使读写与硬件保持单一来源。

硬件写入安全性：trap 发生时，硬件 VMCR 属于当前运行的 vCPU（EL2 中断入口与 `current` 一致），因此直接写硬件不会影响其他 VM。

## 4. 验证

- `make -B -f Makefile.virt -j2`：exit 0。
- 35 秒 QEMU 回归（exit 124）：
  - `task stack overflow`：0
  - `shm: PASSED`：2，`shm: FAILED`：0
  - `Phase 19: SOME tests FAILED`：0
  - `vITS: FAILED`：0
  - `not handled`：0
- 回归日志中的 8 条 `command at ... failed` 均为负面用例的预期 fail-stop（紧随 `[vmN] ... halted` 输出），非缺陷。

## 5. 局限

当前回归**不能直接证明**跨切换持久性：guest 只写 PMR、不读回，且在写入后不触发 VM 切换与读取比对。该缺陷的修复依据是代码路径分析（`leaving_vm` 仅在切换时调用、会覆盖副本），而非运行时复现。

若要运行时证明，需新增 guest 用例：写 PMR 后触发切换再读回。该用例不在本阶段范围内，记为后续工作。

## 6. 已排除项

- `ICC_IGRPEN1_EL1`：按 Arm 架构，`ICH_HCR_EL2.TC` 不捕获它（仅 `TALL1` 或 HFG 才会），而 `IMO=1` 时由硬件虚拟化到 `ICV_IGRPEN1_EL1`。日志中无其告警是正确的。
