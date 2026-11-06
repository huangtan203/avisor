# aVisor Phase 173 详细设计：vINTID 合法上界接受验证

> 平台：QEMU virt / AArch64 EL2 / GICv3 ITS + vITS
> 前置阶段：Phase 172 vINTID 上界负面验证

## 1. 背景

Phase 172 证明了 vINTID=9148（换算 vSPI 1020）被拒绝，但只证明「越界被拒」。如果校验条件写错成拒绝了所有 MAPTI，9148 的 WARN 同样会出现，该用例无法区分。Phase 172 文档第 6 节已把这一点列为限制。

## 2. 目标与非目标

目标：

- 证明合法最大值 vINTID=9147（vSPI 1019）被接受；
- 与 9148 的拒绝形成对照，排除「一律拒绝」的误判。

非目标：

- 不验证 9147 的中断实际送达（超出范围校验本身的范围）；
- 不修改 hypervisor。

## 3. 设计

在 vINTID 负面用例之后插入：

1. `vits_mapti(edu_requester_id, 3, 9147, 0)` —— 用 eventid 3（现有映射占用 1、2，避免走替换路径）；
2. `vits_sync()`；
3. 断言 `CREADR == CWRITER` 且 `ERR_STATUS.halted == 0`；
4. 成功后立即 `vits_discard(edu_requester_id, 3)` 并 `vits_sync()`，撤销映射，避免占用反查表项影响后续 MAPD V=0 用例。

### 3.1 为什么必须撤销

MAPTI 9147 会分配一个 pLPI 并占用反查表槽位。若不撤销，后续 MAPD V=0 用例遍历该设备的全部事件时范围变化，可能改变用例行为。撤销后状态与插入前一致。

## 4. 验证

- `make -B -f Makefile.virt -j2`：退出码 0；
- `git diff --check`：通过；
- QEMU（35 秒 timeout，退出码 124）：

```
294: WARN[1]: vITS: MAPTI invalid vINTID 5
296: vITS: invalid vINTID 5 MAPTI halted
302: WARN[1]: vITS: MAPTI invalid vINTID 9148
304: vITS: invalid vINTID 9148 MAPTI halted
313: vITS: legal max vINTID 9147 accepted
369: vITS: PASSED! Received vSPI 72 (vLPI 8200)
396: vITS: edu MSI and vITS share vSPI 72, both paths still deliver
402: PCI: edu function reset complete for VM1
```

9147 无 WARN、无 halt，9148 有 WARN 且 halt，形成明确对照。全程无 `command timeout`。

## 5. 限制

- 只验证「被接受」，未验证注入到 guest 的 vSPI 实为 1019；
- 9147 的映射在用例内撤销，因此该号未被长期持有，未验证多次使用；
- 原因码仍不能区分下界与上界越界。

## 6. 文件映射

| 文件 | 职责 |
|---|---|
| `guests/el1_test/main.c` | 新增合法上界接受用例，含撤销 |
| 本文件 | 设计、验证、限制 |

## 7. 总结

与 Phase 172 的拒绝用例构成边界对照，证明 vINTID 范围校验是精确的区间判断，而非一律拒绝。该用例同时保证自身不污染后续反查表状态。
