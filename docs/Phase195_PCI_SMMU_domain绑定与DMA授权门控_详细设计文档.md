# Phase 195：SMMU domain 绑定与 DMA 授权门控（方向 A）

## 1. 背景

Phase 122 为每个 PCI function 引入显式 DMA 授权状态，但授权只表示 Hypervisor 允许开启 BME，没有任何机制把「授权」与「地址隔离能力」关联。Phase 122/124 的限制章节都记着同一件事：`pci_dma_set_authorized(..., true)` 之前应先要求 requester 已绑定 SMMUv3 domain。

本阶段按「方向 A：抽象 + 授权事务合并」落地其中的授权前置条件部分：引入 `smmu_domain` 软件记录，把「已绑定 domain」作为开启 Bus Master 的前置条件，并保证撤销时序为先清 BME、再解绑。

**明确的范围限制**：QEMU 6.2 没有 ARM SMMUv3 设备，本阶段不写任何 SMMU 寄存器，不做 Stream Table / Context Descriptor 编程。`smmu_domain` 是纯软件状态，用于承载授权事务的不变式，不具备地址隔离能力。

## 2. 不变式

1. 未绑定 domain 的 function 不得开启 Bus Master；已授权则必须已绑定 domain。
2. 撤销时序：先清 `PCI_CFG_COMMAND` 的 BME，再解绑 domain。反向会出现在 BME 仍开启时 domain 已失效的窗口。
3. 拒绝必须发生在任何 `pci_config_write` 之前，保证负向用例无副作用。

## 3. 实现

### 3.1 状态字段

`hypervisor/boards/virt/pci.c`，`struct pci_device_resource`：

```c
/*
 * SMMU domain 绑定（软件记录，0 = 未绑定）。
 * QEMU 6.2 无 ARM SMMUv3 设备，此处只维护授权事务所需的软件状态，
 * 不写任何 SMMU 寄存器；真正的 Stream Table/Context Descriptor
 * 编程待 Orin 实机环境验证后再补。
 */
uint16_t smmu_domain;
```

### 3.2 bind / unbind

```c
static int pci_smmu_bind(struct pci_device_resource *resource, uint16_t domain);
static void pci_smmu_unbind(struct pci_device_resource *resource);
```

- `bind`：参数或 domain 为 0 时返回 -1，否则写入并打印 `SMMU bind ... (software only)`。
- `unbind`：未绑定时直接返回；否则打印 `SMMU unbind` 并清零。

### 3.3 授权门控

`pci_dma_set_authorized()` 中，检查顺序为：

参数/owner → 取锁 → stale generation → **SMMU domain** → generation 耗尽 → 读写 `PCI_CFG_COMMAND`。

因此 SMMU 拒绝发生在任何配置空间写之前：

```c
if (authorized && !resource->smmu_domain) {
    pci_dma_lock_release(flags);
    WARN("PCI: DMA authorize refused, no SMMU domain for %02x:%02x.%u",
         resource->bus, resource->dev, resource->func);
    return -1;
}
```

### 3.4 调用点

| 位置 | 时序 |
|---|---|
| MSI 启用路径 | `pci_smmu_bind(edu_resource, 1)` → `pci_dma_set_authorized(..., 1, ...)` |
| 重新授权路径 | `vits_lpi_audit_owned_range(...)` → `pci_smmu_bind` → `pci_dma_set_authorized`，任一失败 `goto rollback` |
| `pci_edu_quiesce` | `pci_dma_set_authorized(..., 1, 0, ...)` 成功后才 `pci_smmu_unbind` |
| `pci_dma_guard_init` 重置 | `dma_authorized = 0;` 后 `pci_smmu_unbind(resource);` |

### 3.5 状态审计

`pci_dma_state_audit_locked()` 新增不变式：`dma_authorized && !smmu_domain` 视为不一致，`WARN` 输出追加 `smmu=%u`。

### 3.6 自检

`pci_smmu_authorize_guard_selftest()`，在 `pci_enumerate()` 中于 `pci_dma_guard_init()` / `pci_dma_state_audit()` 之后调用：

1. 找 `owner_vmid == 1` 且未绑定 domain 的 resource。
2. 前置条件检查：该 resource 必须未授权。否则拒绝原因可能来自 stale generation，用例失去意义，直接判失败。
3. 用当前 generation 调用 `pci_dma_set_authorized(..., 1, 1, generation)`，期望返回非 0。
4. 成功（返回 0）则报 `SMMU guard failed`。

## 4. 实测输出

QEMU（`-M virt,virtualization=on,gic-version=3 -smp 2 -device edu`）：

```
WARN[0]: PCI: DMA authorize refused, no SMMU domain for 00:02.0
INFO[0]: PCI: SMMU guard PASSED, authorize refused for 00:02.0
INFO[0]: PCI: DMA state audit PASSED functions=3 authorized=0
```

绑定与解绑时序（同一轮启动，实际输出）：

```
INFO[0]: PCI: SMMU bind 00:02.0 domain=1 (software only)
INFO[1]: PCI: SMMU unbind 00:02.0 domain=1
INFO[1]: PCI: SMMU bind 00:02.0 domain=1 (software only)
```

日志只记录 bind 与 unbind，不记录 authorize 与 revoke 的单独行。「先清 BME 再解绑」的顺序由代码保证（见 3.4），日志无法单独证明该顺序。

结果：`PANIC=0`、`FAIL=0`、`WARN=21`（与 Phase 194 基线一致；`no SMMU domain` 的 WARN 为负向用例的预期输出，已计入该基线），无 `DMA state mismatch`。

## 5. 结论

- 授权前必须已绑定 SMMU domain 的不变式成立，负向用例无副作用（拒绝先于配置空间写）。
- 撤销时序正确：先清 BME 再解绑。
- generation 与 SMMU domain generation 的绑定**未实现**。

## 6. 未验证 / 未实现

- 不编程 SMMU 寄存器。Stream Table、Context Descriptor、Stage 2 页表均未实现，因此没有实际地址隔离。
- QEMU 6.2 无 SMMUv3 设备，寄存器级行为只能在 Orin 实机验证。
- 未覆盖多设备并发授权；当前只有 VM1 edu 使用该路径。
- 未实现 domain generation 与 DMA generation 的关联，Phase 124 的对应限制仍成立。

## 7. 代码位置

| 文件 | 内容 |
|---|---|
| `hypervisor/boards/virt/pci.c` | `smmu_domain` 字段、`pci_smmu_bind/unbind`、授权门控、审计不变式、`pci_smmu_authorize_guard_selftest`、调用点时序 |

## 8. 复现

```
make -B -f Makefile.virt -j2
timeout 35 qemu-system-aarch64 -M virt,virtualization=on,gic-version=3,highmem=off \
    -cpu max -smp 2 -m 1G -device edu -kernel bin_virt/kernel8.img \
    -display none -monitor none -serial file:/tmp/run.out
```

退出码 124 为正常超时。检查 `SMMU guard PASSED`、`DMA state audit PASSED`、无 `DMA state mismatch`。
