# Phase 178：补全 MAPTI 各失败分支的 halt_reason 详细设计文档

## 1. 背景

Phase 177 修复了 `vits_cmd_mapti()` 中 `its_send_mapti()` 物理下发失败的 `halt_reason`，但同函数内还有多个失败分支只 `return -1`，不设置原因码。

`halt_reason` 不随 `halted` 清除（`halted = 0` 时保持原值），只在下一次失败时被覆写。因此这些分支失败后，软寄存器 ERR_STATUS[15:8] 报告的是**上一次失败的原因码**，而不是本次的真实原因。

## 2. 修复清单

`vits_cmd_mapti()` 中补上 `vits->halt_reason = VITS_ERR_OTHER;` 的分支：

| 分支 | 位置说明 |
|---|---|
| `veventid == 0` 且 `irq_domain_register` 失败 | PCI MSI 专用 LPI 注册失败 |
| `veventid == 0` 且 `vits_lpi_audit_owned_range` 失败 | 上述路径的审计失败（登记后回退） |
| 反查表满 | 无空 evt_map 槽位 |
| 无空闲物理 LPI | `vits_reserve_plpi` 失败 |
| `irq_domain_register` 失败 | 常规路径的 LPI 归属登记失败 |
| `vits_lpi_audit_owned_range` 失败 | 常规路径的审计失败（走 rollback） |
| 反查表槽位消失 | 防御性分支，前置检查已确认有空位仍失败 |

均选用 `VITS_ERR_OTHER`：这些失败是 Hypervisor 内部资源或一致性错误，没有更具体的 vITS 原因码可映射。

## 3. 范围

本阶段**只改 MAPTI**。以下同类问题已确认存在但未修改，留待独立阶段：

- `vits_cmd_discard()` → `vits_unmap_evt()`：物理 DISCARD 失败、路由撤销失败两处均未设置 `halt_reason`。

未一并修改是为了让本阶段的修改范围保持单一主题（MAPTI），便于独立回滚与审查。

## 4. 验证

- 构建 `make -B -f Makefile.virt -j2` 通过（exit 0）。
- `git diff --check` 通过。
- 35 秒 QEMU 回归（`/tmp/phaseM.log`）：
  - `EL2 task stack overflow`：0
  - `shm: PASSED`：2，`shm: FAILED`：0
  - `Phase 19: SOME tests FAILED`：0
  - `vITS: FAILED`：0
  - `vITS: PASSED! Received vSPI 72`、`share vSPI 72, both paths still deliver`：通过

## 5. 局限

QEMU 回归无法触发这些失败分支（反查表满、LPI 耗尽、注册/审计失败都需要构造异常条件），因此修改本身未经运行时验证，仅由代码路径审查确认。回归的作用是确认改动没有破坏正常路径。
