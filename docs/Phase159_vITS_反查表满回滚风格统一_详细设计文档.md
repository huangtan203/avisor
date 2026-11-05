# aVisor Phase 159 详细设计：反查表满分支回滚风格统一

> 平台：QEMU virt / AArch64 EL2 / GICv3 ITS + vITS
> 前置阶段：Phase 158 MAPTI 回滚路径统一
> 本阶段目标：让「反查表满」分支复用 `vits_mapti_rollback()`，消除展开式重复代码。
> 性质：纯风格统一，不改变行为。

## 1. 背景

Phase 158 把审计失败与物理 MAPTI 失败两个点统一到 `vits_mapti_rollback()`。第三个失败点「反查表满」仍直接展开回滚代码：

```c
irq_domain_unregister_owned(plpi, vits->vm->vmid);
lpi_map_acquire();
lpi_map[plpi - LPI_MAP_BASE].vmid = 0;
lpi_map[plpi - LPI_MAP_BASE].vintid = 0;
lpi_map_release();
```

内容与辅助函数的「新分配」分支逐行相同，属于重复代码，且修改回滚语义时需要同步两处。

## 2. 目标与非目标

目标：

- 「反查表满」分支改为调用 `vits_mapti_rollback(vits, plpi, 0, NULL)`；
- 行为与修改前完全一致。

非目标：

- 不改变表满时的物理侧处理（见第 5 节，这是一个已存在的遗留问题，本阶段只记录，不修复）；
- 不扩大反查表容量。

## 3. 可达性确认

该分支位于 `if (evt_idx < 0)` 内。`evt_idx` 只在替换路径（已有映射）时 >= 0，因此该分支只在新分配路径可达。传入 `have_prev=0` 与 `prev_route=NULL` 是正确的：辅助函数在 `have_prev == 0` 时不访问 `prev_route`。

## 4. 改动

```c
if (evt_idx < 0) {
    /* 新分配路径：无旧路由可恢复，prev_route 不被访问。 */
    vits_mapti_rollback(vits, plpi, 0, NULL);
    WARN("vITS: MAPTI reverse map full for devid=%u eventid=%u",
         vdevid, veventid);
    return -1;
}
```

日志保留两条：辅助函数输出「回滚的是哪个 pLPI」，调用方输出「为什么回滚」。二者含义不同，不属于重复。

## 5. 已知遗留问题：表满时物理侧未回滚

表满发生在 `its_send_mapti()` **成功之后**。此时物理 ITS 已为 (devid, eventid) 绑定新分配的 pLPI，但：

- 反查表无项，无法通过 DISCARD 定位该 pLPI；
- pLPI 不回收（Phase 154 的设计），因此该 pLPI 在物理侧永久有效但无软件记录。

这与 Phase 158 第 6 节的「物理侧不回滚」结论不同。那一节的论证依赖「替换复用同一 pLPI」，而表满发生在**新分配**路径，物理侧确实新增了一个映射。因此该论证不适用于本分支。

实际后果：物理 ITS 仍可能投递该 pLPI 的中断，而软件侧无路由、无 shadow，中断会被丢弃。这是无害的丢弃，但物理资源有泄漏。

本阶段不修复：正确的修复需要先发一条物理 DISCARD 再回滚，但此时若 DISCARD 失败，又会产生新的不一致。该权衡需要单独设计，应在后续阶段处理。

## 6. 验证

- `make -B -f Makefile.virt -j2`：通过，`vits.c` 无警告；
- `git diff --check`：通过；
- IDE diagnostics（`vits.c`）：无诊断；
- QEMU（35 秒 timeout，退出码 124），回归与 Phase 158 逐行一致：

```
314: MAPTI done: vINTID=8201 -> pLPI=9000
322: MAPTI done: vINTID=8200 -> pLPI=9001   ← 替换仍复用
330: PASSED! Received vSPI 72 (vLPI 8200)
334: DISCARD done: pLPI=9001 (pLPI retained)
346: MAPTI done: vINTID=8200 -> pLPI=9002
349: remap after DISCARD delivered again
```

**未验证**：表满分支在 QEMU 中不可达，因为 64 项表在回归中远未填满。本阶段是等价的代码重构，验证依据为代码对比与编译，而非运行时证据。

## 7. 限制

- 表满分支无运行时验证；
- 表满时物理侧 pLPI 泄漏（第 5 节）；
- `irq_domain_unregister_owned()` 与 `irq_domain_register()` 返回值在回滚中仍被忽略；
- `edu function reset FAILED` 仍存在。

## 8. 文件映射

| 文件 | 职责 |
|---|---|
| `hypervisor/boards/virt/vits.c` | 表满分支改为调用 `vits_mapti_rollback()` |
| 本文件 | 风格统一记录、可达性分析、表满时物理侧遗留问题 |

## 9. 总结

Phase 159 完成三个 MAPTI 失败点的回滚统一，行为不变。同时确认了一个此前未记录的问题：表满时物理 ITS 已绑定新 pLPI，软件回滚无法撤销该绑定，形成 pLPI 泄漏。这一问题与 Phase 158 的物理侧论证不同，需要后续单独设计。
