# Phase 199：HW 映射 LR 的虚拟 DIR 语义梳理

## 1. 背景

Phase 197（LR0）与 Phase 198（LR2）的实测显示：虚拟 IAR 把 HW 映射 LR 从 pending 转为 active，虚拟 EOIR 与虚拟 DIR 之后，LR 仍保持 active。

这一结果与代码一致，但“HW 映射 LR 是否应由虚拟 DIR 去活”没有论证。本阶段只梳理代码路径与语义，不修改代码。

## 2. 代码路径

### 2.1 虚拟 DIR 的处理

`hypervisor/arch/aarch64/sync_exc.c:669`：`ICH_HCR_EL2.TC=1` 使 guest 的 DIR 写入陷入 EL2，调用 `vgic_handle_dir()`。

`hypervisor/boards/virt/vgic.c:140–154`：`vgic_handle_dir()` 扫描 LR1..NR-2，遇到 `ICH_LR_HW` 置位的 LR 直接 `continue`。因此虚拟 DIR 不修改 LR0/LR2。注释说明：“HW 映射 LR（LR0/LR2）的物理 DIR 由 virt_leaving_vm 负责，此处跳过。”

### 2.2 物理 DIR 的处理

`hypervisor/boards/virt/virt_board.c:670–703`，`virt_leaving_vm()` 在离开 VM 时执行：

- LR0 分支：仅当 `ICH_LR_HW` 置位、状态非 invalid、且 pINTID == `PPI_VIRT_TIMER`（27）时，发出物理 `ICC_DIR_EL1`。
- LR2 分支：仅当 `pci_edu_intx_irq != 0`、`ICH_LR_HW` 置位、状态非 invalid、且 pINTID == `pci_edu_intx_irq` 时，发出物理 `ICC_DIR_EL1`。

`pci_edu_intx_irq` 在 `hypervisor/boards/virt/pci.c:1731` 运行时计算为 `32 + VIRT_PCIE_INTX_SPI_BASE + host_pin`，不是固定值。

### 2.3 中断处理路径中的物理 DIR

`hypervisor/boards/virt/irq.c:199`、`208`、`238`：hypervisor 自身处理物理中断时，对非目标 VM、vtimer 等情况直接写物理 `ICC_DIR_EL1`。这些路径与 HW LR 的虚拟 DIR 无关。

## 3. 与 Phase 197/198 实测的对应关系

| 项 | Phase 197/198 使用的值 | 物理 DIR 触发条件 |
|---|---|---|
| pINTID | 1000 | 必须等于 27（LR0）或 `pci_edu_intx_irq`（LR2） |
| 结果 | 两个分支均不满足 | `virt_leaving_vm` 不发出物理 DIR |

因此，Phase 197/198 的测试中，**物理 DIR 在本测试中不会发出**。这一点是代码推导，未通过物理侧观测验证。

## 4. 语义结论

基于代码，HW 映射 LR 的去活职责分为两层：

1. **虚拟侧**：虚拟 DIR 不清除 HW LR，由 `vgic_handle_dir` 的跳过逻辑保证。
2. **物理侧**：物理 DIR 由 `virt_leaving_vm` 在离开 VM 时按 pINTID 条件发出，而不是由虚拟 DIR 触发。

据此，“虚拟 DIR 后 LR 仍 active”在当前代码的设计内是**自洽的**：HW LR 的生命周期由 `virt_leaving_vm` 管理，而不是虚拟 DIR。

但是否“符合 GICv3 架构语义”，需要对照 GICv3 规范中 HW 映射 LR 的 deactivate 规则确认，本阶段未查证规范原文，因此不下定论。

## 5. 未验证的部分

- 与 GICv3 规范的逐条对照：未查证规范原文。
- 物理 GIC 状态：guest 无法观测，未在运行时读取物理 active 状态，因此“物理 DIR 是否真正去活物理中断”仍未验证。
- `virt_leaving_vm` 的 pINTID 条件分支：只通过代码阅读确认，未用真实 HW LR 触发过。
- Phase 197/198 的 pINTID 1000 不触发物理 DIR，因此这两次测试没有覆盖物理去活路径。

## 6. 后续建议

如需验证物理侧，需要：
- 使用真实 pINTID（vtimer PPI 27 或 edu INTx）注入 HW LR，并在 `virt_leaving_vm` 前后读取物理 active 状态；
- 或在 hypervisor 中加入只读诊断。

这两项都涉及改动 hypervisor 代码或新增测试路径，需要单独批准。

## 7. 代码位置

| 文件 | 内容 |
|---|---|
| `hypervisor/arch/aarch64/sync_exc.c` | DIR trap 分发（669） |
| `hypervisor/boards/virt/vgic.c` | `vgic_handle_dir()`，跳过 HW LR |
| `hypervisor/boards/virt/virt_board.c` | `virt_leaving_vm()`，物理 DIR 条件 |
| `hypervisor/boards/virt/pci.c` | `pci_edu_intx_irq` 运行时计算（1731） |
| `hypervisor/boards/virt/irq.c` | 物理中断处理中的 DIR（199、208、238） |
