# Phase 174：vITS 越界 EventID 拒绝与物理 ITS 队列恢复 详细设计文档

## 1. 问题

Guest 通过 vITS 下发 `MAPTI` 时，`vits_cmd_mapti()` 只校验 vINTID，**不校验 EventID**。EventID 被原样透传到物理 ITS。

物理 ITT 只有 `ITS_ITT_MAX_EVENTS = 16` 项（MAPD `size=3`）。EventID ≥ 16 时，物理 ITS 停在处理该命令的过程中不再推进 `CREADR`，命令队列**永久挂死**：

```
vITS: MAPTI vdevid=16 veventid=20 vintid=8300 vicid=0
ITS: command queue stalled (CWR=192 CRR=161); future commands rejected
vITS: command at 0x40 failed; halting, CREADR retained
```

后果不止单个 Guest：之后**所有**物理 ITS 命令（包括其它 VM 的）都被拒绝，VM2 的 shm 测试超时、VM1 的 DISCARD 断言失败。一条 Guest 可控命令即可拖垮整个物理 ITS。

## 2. 根因

两层缺陷：

1. **vITS 层无 EventID 范围校验**。`vits_cmd_mapti()` 从 `cmd[1]` 取出 `veventid` 后直接使用，没有与 ITT 容量比较。
2. **物理队列挂死后无恢复路径**。`its_send_cmd()` 超时后置 `cq_stalled = 1` 并永久拒绝后续命令；`cq_stalled` 只在 `its_init()` 清零，运行期不会恢复。

## 3. 修复

### 3.1 共享 ITT 容量常量

把 `ITS_ITT_SIZE_SHIFT` / `ITS_ITT_MAX_EVENTS` 从 `its.c` 内部移到 `include/boards/virt/its.h`，让物理 ITS 与 vITS 使用同一份容量定义，避免两侧规则漂移。

### 3.2 vITS 层拒绝越界 EventID

在 `vits_cmd_mapti()` 中，紧跟 vINTID 校验之后、任何副作用之前：

```c
if (veventid >= ITS_ITT_MAX_EVENTS) {
    if (!vits->halted || vits->halted_at != cmd_offset)
        WARN("vITS: MAPTI eventid %u exceeds ITT capacity %u",
             veventid, ITS_ITT_MAX_EVENTS);
    vits->halt_reason = VITS_ERR_OTHER;
    return -1;
}
```

越界 EventID 现在以 fail-stop 方式在 vITS 层被拒绝，**不会到达物理 ITS**，因此队列不会挂死。

### 3.3 物理 ITS 队列恢复

新增 `its_reset_cmdq()`：清 `CTLR.Enabled` → 重写 `CBASER` → `CWRITER=0` → 重新使能。ITS 使能期间 `CBASER` 只读，必须先清 `CTLR`，这与 `its_init()` 的编程顺序一致。

`its_send_cmd()` 超时分支改为调用该恢复函数，恢复成功后清除 `cq_stalled`，后续命令可继续下发。若恢复失败（`CTLR` 未回 1），保持 `cq_stalled = 1` 并告警。

## 4. 为什么两处都要改

- 只做 3.2：能防住本次具体触发路径，但物理 ITS 若因其它原因挂死仍无法恢复，且`cq_stalled` 的永久封锁语义本身就是缺陷。
- 只做 3.3：能恢复队列，但每次都让物理 ITS 进入挂死状态再恢复，期间其它 VM 的命令全部失败，代价过大。
- 两者结合：正常路径不再触发挂死（3.2），异常路径可自愈（3.3）。

## 5. 验证

构建 `make -B -f Makefile.virt -j2` 通过；`git diff --check` 通过。

35 秒 QEMU 回归（`/tmp/phase174A.log`）：

| 检查项 | 结果 |
|---|---|
| `EL2 task stack overflow` PANIC | 0 |
| `ITS: command queue stalled` | 无 |
| `vITS: MAPTI eventid 20 exceeds ITT capacity 16` | 出现（校验生效） |
| `vITS: overflow eventid MAPTI rejected without ITS stall` | 通过 |
| `vITS: command timeout!` | 无 |
| `edu: MSI interrupt PASSED` | 通过 |
| `vITS: PASSED! Received vSPI 72` | 通过 |
| `edu MSI and vITS share vSPI 72` | 通过 |
| `PCI: edu function reset complete` | 通过 |

guest 侧新增用例：手工写 MAPTI 槽（EventID=20，vINTID 合法），断言 `CREADR` 停在失败槽、`ERR_STATUS` 的 halted 位与失败槽号正确、随后队列可复位。写法与既有 vINTID 负面用例一致（手工写槽，不走 `vits_sync()`，因为队列已 halt）。

## 6. 已知未解决问题

`shm` 跨 VM 测试失败（VM2 `timeout waiting for PING`、VM1 `timeout waiting for PONG`），`Phase 19: SOME tests FAILED`。

判断为两个 VM 的测试时序不同步：VM2 先进入等待，VM1 很久之后才写入，此时 VM2 已超时 detach。与本次改动无关，但**未经基线对比证实**。本阶段不声称修复该问题。
