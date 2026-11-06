# aVisor Phase 172 详细设计：vINTID 上界运行时负面验证

> 平台：QEMU virt / AArch64 EL2 / GICv3 ITS + vITS
> 前置阶段：Phase 169 vITS MAPTI vINTID 运行时校验

## 1. 背景

Phase 169 在 MAPTI 入口加入 vINTID 范围校验：`vintid < 8192` 或 `vintid_to_vspi(vintid) >= 1020` 时 fail-stop。但其 guest 负面用例只覆盖下界（vINTID=5），上界分支从未被运行时触发。Phase 169 文档第 7 节已将此列为限制。

## 2. 目标与非目标

目标：

- 用运行时证据覆盖上界分支；
- 精确验证边界：合法最大值与第一个非法值。

非目标：

- 不修改 hypervisor 校验逻辑；
- 不新增原因码。

## 3. 边界计算

换算函数 `vintid_to_vspi(v) = 64 + (v - 8192)`（v ≥ 8192），上限 `1020`：

| vINTID | vSPI | 结果 |
|---|---|---|
| 9147 | 1019 | 合法（最大值） |
| 9148 | 1020 | 非法（第一个越界值） |

因此上界负面用例取 9148。9147 是合法边界，不放入负面用例，避免误判。

## 4. 设计

guest 中原有的单次负面用例改为对 `{5, 9148}` 的循环，复用同一段 MAPTI 构造、halt 断言、原因码断言与队列复位逻辑。每轮打印具体 vINTID，确保两轮都真实执行。

用循环而非复制代码，避免两份几乎相同的回归段在将来分叉。

## 5. 验证

- `make -B -f Makefile.virt -j2`：退出码 0；
- `git diff --check`：通过；
- QEMU（35 秒 timeout，退出码 124）：

```
294: WARN[1]: vITS: MAPTI invalid vINTID 5
296: vITS: invalid vINTID 5 MAPTI halted
302: WARN[1]: vITS: MAPTI invalid vINTID 9148
304: vITS: invalid vINTID 9148 MAPTI halted
358: vITS: PASSED! Received vSPI 72 (vLPI 8200)
385: vITS: edu MSI and vITS share vSPI 72, both paths still deliver
391: PCI: edu function reset complete for VM1
```

两个边界均由 hypervisor 的 WARN 证实真实拒绝，且后续回归不受影响。

## 6. 限制

- 合法最大值 9147 未被运行时验证为「被接受」。负面用例只证明非法值被拒绝，正向接受由 edu/vITS 常规回归间接覆盖（它们使用 8200）；
- 原因码仍为 `VITS_ERR_OTHER`，guest 无法区分两种越界。

## 7. 文件映射

| 文件 | 职责 |
|---|---|
| `guests/el1_test/main.c` | 非法 vINTID 负面用例扩展为下界 + 上界循环 |
| 本文件 | 边界计算、验证、限制 |

## 8. 总结

Phase 169 遗留的上界验证缺口已补齐。9148 作为第一个越界值被运行时拒绝，证明换算上限校验真实生效。
