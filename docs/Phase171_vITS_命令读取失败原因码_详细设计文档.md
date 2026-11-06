# aVisor Phase 171 详细设计：命令读取失败时的原因码覆盖

> 平台：QEMU virt / AArch64 EL2 / GICv3 ITS + vITS
> 前置阶段：Phase 169 vITS MAPTI vINTID 运行时校验

## 1. 背景

`vits_process_one()` 在读取命令前调用 `vits_read_cmd()`。该函数在以下情况返回 -1：

- `CBASER` 无效或 V 位为 0；
- `offset` 越界或未按 32 字节对齐；
- 命令队列 GPA 未映射（`ipa_to_pa` 失败）。

返回 -1 后 `vits_process_one()` 直接 `return -1`，由 `vits_process_cmdq()` 置 `halted`。但**原因码 `halt_reason` 没有被赋值**，会保留上一次失败命令写入的值。

## 2. 缺陷

`VITS_ERR_STATUS[15:8]` 由 `halt_reason` 填充。因此出现如下错配：

1. Guest 发一条未拥有 requester 的 MAPD → `halt_reason = VITS_ERR_UNOWNED_DEVICE (2)`；
2. Guest 修复后重新配置，命令队列某槽位所在页被撤销映射 → 读取失败；
3. 队列 halt，但 `ERR_STATUS` 仍报告原因 `2`（UNOWNED_DEVICE），与实际原因「读取失败」不符。

Guest 会据此误判失败类型。这是可触发的真实错误上报缺陷。

## 3. 目标与非目标

目标：

- 读取失败时显式覆盖原因码为 `VITS_ERR_OTHER`；
- 不改变任何既有失败路径的原因码。

非目标：

- 不新增原因码（「命令读取失败」暂归入 OTHER）；
- 不修改 `vits_read_cmd()` 的内部判断。

## 4. 设计

```c
if (vits_read_cmd(vits, offset, cmd) < 0) {
    vits->halt_reason = VITS_ERR_OTHER;
    return -1;
}
```

改动位于 `vits_process_one()` 入口，是唯一未设置原因码的失败出口。其余失败路径（MAPC/MAPD/MAPTI/DISCARD/未知 opcode）均已在各自函数内赋值。

## 5. 失败路径原因码矩阵

| 失败来源 | `halt_reason` |
|---|---|
| 命令读取失败 | `OTHER`（本阶段修复） |
| 未知 opcode | `UNKNOWN_OPCODE` |
| MAPC 不支持 | `UNSUPPORTED` |
| MAPD/MAPTI/DISCARD 未拥有 | `UNOWNED_DEVICE` |
| MAPTI 非法 vINTID | `OTHER` |
| 队列排空成功 / CBASER 重配 | `OTHER`（重置） |

## 6. 验证

- `make -B -f Makefile.virt -j2`：退出码 0；
- `git diff --check`：通过；
- QEMU（35 秒 timeout，退出码 124），既有全部回归保持通过：

```
249: edu: MSI interrupt PASSED
294: WARN[1]: vITS: MAPTI invalid vINTID 5
296: vITS: invalid vINTID MAPTI halted
350: vITS: PASSED! Received vSPI 72 (vLPI 8200)
377: vITS: edu MSI and vITS share vSPI 72, both paths still deliver
383: PCI: edu function reset complete for VM1
```

## 7. 限制（必须明确）

- **本修复没有运行时证据**。现有 guest 回归从未构造出「命令读取失败」：要让 `ipa_to_pa` 失败，需要让命令队列所在 GPA 解除映射，而 guest 当前测试流程不会这样做。
- 该改动的正确性仅由代码审查支撑：确认 `vits_process_one()` 的读取失败分支此前唯一缺失 `halt_reason` 赋值，且 `VITS_ERR_STATUS` 读取时确实使用该字段。
- 归入 `VITS_ERR_OTHER` 使 guest 无法区分「读取失败」与其它失败，属于既有原因码粒度的限制。

## 8. 文件映射

| 文件 | 职责 |
|---|---|
| `hypervisor/boards/virt/vits.c` | 读取失败分支显式设置原因码 |
| 本文件 | 缺陷分析、验证、限制 |

## 9. 总结

修复了命令读取失败时原因码残留的问题，使 `VITS_ERR_STATUS` 不再上报上一条失败命令的原因。改动极小，但没有运行时覆盖，需在后续有映射撤销测试能力时补充验证。
