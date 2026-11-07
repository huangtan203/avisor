# Phase 179：DISCARD 路径失败时设置 halt_reason 详细设计文档

## 1. 背景

Phase 177、178 已补全 `vits_cmd_mapti()` 的失败原因码。同类缺陷仍存在于 DISCARD 路径：`vits_unmap_evt()` 的两处失败分支只 `return -1`，不设置 `vits->halt_reason`。

`halt_reason` 不随 `halted` 清除，只在下一次失败时被覆写。因此这两处失败后，软寄存器 ERR_STATUS[15:8] 仍报告上一次失败的原因码。

## 2. 修复

在 `vits_unmap_evt()` 的两处失败分支补上 `VITS_ERR_OTHER`：

| 分支 | 说明 |
|---|---|
| `its_send_discard()` 失败 | 物理 DISCARD 下发失败 |
| `vits_lpi_revoke_owned()` 失败 | 路由撤销失败 |

选择在 `vits_unmap_evt()` 内部设置，而不是在各调用点设置，因为它有两个调用者：

- `vits_cmd_discard()`：DISCARD 命令（第 650 行附近）
- `vits_cmd_mapd()`：MAPD V=0 的批量解映射（第 327 行附近）

两者都是 fail-stop 语义（失败即停止队列），失败原因同为 Hypervisor 内部错误，在共享函数内统一设置可避免调用点重复或遗漏。

## 3. 范围说明

修改点在共享函数内，因此 MAPD V=0 路径也一并获得正确的原因码。这是有意为之：两者失败语义相同，且 MAPD V=0 的失败本身就是 `vits_unmap_evt()` 的转发，此前同样缺失原因码。

不在本次范围：

- `vits_read_cmd()` 的失败分支（Phase 171 已处理）
- `vits_reserve_plpi()` 内部返回 -1（由 Phase 178 在调用点覆盖）

## 4. 验证

- 构建 `make -B -f Makefile.virt -j2` 通过（exit 0）。
- `git diff --check` 通过。
- 35 秒 QEMU 回归（`/tmp/phaseD.log`）：
  - `EL2 task stack overflow`：0
  - `shm: PASSED`：2，`shm: FAILED`：0
  - `Phase 19: SOME tests FAILED`：0
  - `vITS: FAILED`：0
  - `vITS: PASSED` 与 `share vSPI 72, both paths`：均出现
  - DISCARD 正常路径日志可见：`DISCARD vdevid=16 veventid=3`

## 5. 局限

这两处失败分支在 QEMU 中未被触发（需要让物理 DISCARD 或路由撤销失败），修改未经运行时验证，仅由代码路径审查确认。回归的作用是确认正常 DISCARD 路径未被破坏。
