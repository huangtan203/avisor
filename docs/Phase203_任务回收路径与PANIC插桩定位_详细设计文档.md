# Phase203 任务回收路径与 PANIC 插桩定位 详细设计文档

## 1. 背景

Phase202 固定 vm2 vCPU0 到物理核 0 后，冻结类失败在 150 轮中为 0。但仍遗留两类未定性问题：

- **回收类失败**：`zombie task 未在切栈后回收`（Test 26，`reaped==0`）、`task 槽位未被实际复用`（Test 27）、`COMMIT 后计数未恢复推进`、`FINISH abort 未回滚到 IDLE`。150 轮中 2 轮（run6、run16）出现。
- **EL2 PANIC**：Phase202 的 150 轮中，`EL2 task stack exhausted` 3 次、`uncaught exception(SYNC_INVALID_EL2)` 3 次，与 pin 的关系未定性。本阶段继续累计的统计见第 6 节，两处数字口径不同，勿混用。

本阶段目标是**定位**，不修复。

## 2. 代码事实（只读核查）

- `hypervisor/arch/aarch64/task.c` `remove_task_slot()`：仅当 `tsk->phys_cpu == smp_processor_id()` 且 `sched_dequeue_task(tsk) == 1` 时才继续回收，否则直接返回 0，不计入 `task_reaped_count`。
- `task_reap_after_switch(prev)`：`prev->state == TASK_ZOMBIE` 时调用 `remove_task_slot(prev)`。
- `reap_zombie_tasks(cpu)`：遍历 `cpu_state[cpu].task[1..nr_tasks)`，对 `TASK_ZOMBIE` 且非 `current` 的 task 调用 `remove_task_slot()`。
- `task_reap_stats()`：`reaped` 取自 `task_reaped_count[cpu]`，只在 `remove_task_slot()` 成功路径递增。
- `hypervisor/arch/aarch64/sync_exc.c` `HVC_TASK_REAP_STATS`：返回 `reaped`，Test 26 以 `reaped==0` 判 FAIL。

**推断（未验证）**：Test 26 的 `reaped==0` 可能是 `sched_dequeue_task` 对 zombie 返回非 1，导致回收被短路。这是候选假设，需插桩确认。

## 3. 插桩方案

仅在 `hypervisor/arch/aarch64/task.c` 的 `remove_task_slot()` 入口与各返回点加打印，记录：`phys_cpu`、`smp_processor_id()`、`state`、`sched_dequeue_task` 返回值、是否成功。前缀统一为 `[RTDBG]`，便于 grep 与还原。

不改调度行为，不改 vm2 pin。

## 4. 验证方法

- 插桩后跑 30 轮 QEMU 回归（与前文一致的命令）。
- 统计 `[RTDBG]` 中短路原因分布，并与 run 中出现的 FAIL 行对齐。
- 每轮记录 Test 26 的 `reaped` 值。

## 5. 执行与结论

### 5.1 回收路径（第 2 节推断，未验证）

- 第 3 节计划的 `[RTDBG]` 插桩**未实际插入**（代码中无 `RTDBG` 字样），本阶段没有 `remove_task_slot()` 短路原因的实测数据。
- 第 2 节"sched_dequeue_task 返回非 1 导致短路"仍是**未验证假设**。

### 5.2 回收类 FAIL（run6 型）

- 窄插桩 30 轮中 `smp_txn_freeze_commit` 全部 `ret=0`、`rack=1`、`tgt=1`。
- 该批**未复现** `COMMIT 后计数未恢复推进`，run6 型 FAIL 的机制**未确定**。
- 无插桩 30 轮中 run6 曾出现三条 FAIL 且无 PANIC，说明至少有一种形态与 PANIC 无关（单轮观察）。

### 5.3 EL2 PANIC 签名分类

`type=0` 即 `SYNC_INVALID_EL2`，来自 EL2 自身异常向量（`sync_invalid_el2`），不是 guest 陷入。

| 签名 | 现场 | 次数 |
|---|---|---|
| A 栈耗尽 | `EL2 task stack exhausted`，`free=0` | 4 |
| B 小常量 PC | `elr=far=x30∈{1,8}`，`sp` 在合法栈内 | 3 |
| C 野地址 | `esr=0x97810005`，`elr` 在 `vgic_pending_pop` 内，`far` 为未映射地址 | 1 |
| D 任务基址 | `elr=far=<task_struct 基址>`，`x30 = elr + 0x100000000` | 2 |

签名 D 两次的 `elr` 分别是 `0x41096000` 与 `0x4108a000`。同轮插桩日志显示这两个地址分别为 vm2 vCPU1 与 vm1 vCPU1 的 `task_struct` 基址。此对应关系已核对，但**原因未知**。

签名 B 的 `cpu_context.pc` 映射到 `switch_to` 中 `cpu_switch_to` 的返回点，属合法值。

### 5.4 已排除的假设

- **切入时 `next->cpu_context.pc` 非法**：60 轮 `[XDBG] bad-next` 为 0，**否定**。
- **`vgic_pending_pop` 入口状态越界**：30 轮 `[PDBG] bad` 为 0；签名 C 仅 1 次，未复现。
- **栈溢出覆盖返回地址**：签名 B 中 `sp` 与 guard 校验均正常，**不支持**。

### 5.5 未验证

- 签名 B 中小常量（1、8）的写入点未定位。它表明某个栈上保存的 `x30` 槽位被写为小常量。
- 签名 D 中 `elr` 等于 `task_struct` 基址的原因未知。
- 签名 A/D 中 `sp=0` 的打印与合法帧链矛盾，判为插桩读数不可信；已加 `sp2` 对照，但未在新批次复现。
- 四类签名是否同源、与回收类 FAIL 的关系、与 pin 的关系，均未验证。

## 6. 批次统计

| 批次 | 轮数 | PANIC（轮） | 签名分布 |
|---|---|---|---|
| p203d | 30 | 1 | C×1 |
| p203e | 30 | 1 | B×1（值 1） |
| p203f | 30 | 2 | A×2 |
| p203g | 30 | 1 | A×1 |
| p203h | 30 | 1 | D×1 |
| p203i | 30 | 1 | B×1（值 8） |
| p203j | 30 | 0 | — |
| p203k | 60 | 2 | A×1、D×1 |
| p203l | 60 | 1 | B×1（值 1） |
| 合计 | 330 | 10 | A4 / B3 / C1 / D2 |

`p203l` 去掉了高频 `[PDBG] ipi` 打印后，PANIC 形态为 B（`elr=1`），与 `p203k` 的 A+D 不同。由于两批轮数少且形态分散，无法判断该打印是否影响复现率，**未验证**。

## 7. 状态

### 7.1 插桩已还原

- 7 个插桩文件已用 `git checkout` 还原：`task.c`、`irq.c`、`vgic.c`、`migration.c`、`sched.c`、`smp.c`、`guests/el1_test/main.c`。
- 还原后代码中已无 `[SDBG]`/`[XDBG]`/`[EDBG]`/`[FDBG]`/`[PDBG]`/`[CMDBG]`/`[RTDBG]` 任何标记。
- `make -B -f Makefile.virt -j2` 通过（`build=0`）。
- `git diff -- hypervisor guests` 为空。
- 未还原的用户改动（`Makefile.tegra`、`docs/Phase63_AGXOrin_*`）保持原样。

### 7.2 gdb 工具链可行性（已实测）

| 项目 | 结论 | 依据 |
|---|---|---|
| `gdb-multiarch` | 可用 | `apt-get download` 后 `dpkg-deb -x` 解包到 `/tmp/gdbx/root/usr/bin/gdb-multiarch`，`set architecture aarch64` 生效 |
| 系统 `gdb` | 不可用于 aarch64 | `GNU gdb 12.1`，`configure --target=x86_64-linux-gnu`，`set architecture aarch64` 报 `Undefined item` |
| `sudo apt install` | 不可用 | 沙箱设置了 `no new privileges`，`sudo -n true` 失败；故采用本地解包方式 |
| QEMU 版本 | 6.2.0 | `qemu-system-aarch64 --version` |
| QEMU gdbstub | 可用 | `-S -gdb tcp:127.0.0.1:1234`，`gdb-multiarch` 连接成功，读到 `PC=0x40000000` |
| 硬件 watchpoint | 可用且已触发 | 监视 `cpu_state[0].curr`（`0x40147750`），gdb 报 `Hardware watchpoint 1 hit`，`Old value = 0`，`New value = 0x40147980`；触发点 `0x400c8140` 紧随 `0x400c813c` 的 `str x2,[x0]`，该指令位于 `sched_init_primary` 中，正是对 `curr` 的首次写入 |

**实测流程**（已跑通）：
1. `make -B -f Makefile.virt -j2`，确认 `cpu_state` 地址为 `0x40147750`。
2. QEMU `-S -gdb tcp:127.0.0.1:1234` 启动并挂起。
3. `gdb-multiarch` 连接后 `watch *(unsigned long long*)0x40147750`，`continue`，命中后读取 `pc`、寄存器。
4. 用 `aarch64-linux-gnu-objdump` 核对触发点指令。

**断点机制验证（已实测）**：
- 脚本 `.sigb_gdb.sh`（临时，未提交）连接后直接 `hbreak *ADDR` + `continue`，**不需要** `interrupt`。早期的 `Cannot execute this command while the target is running` 是外部 `timeout` 杀掉阻塞在 `continue` 的 gdb 所产生的副产物，不是断点失效。
- `sched_init_primary`（`0x400c80d8`）无条件断点：稳定命中（3/3 对照）。
- 条件断点 `hbreak *0x400c80d8 if $x2 == 0x21`：条件为真时命中；`if $x2 == 0x12345`：条件为假时不停。两组对照一致。
- `show_invalid_entry_message`（`0x4009cd2c`）在真实 PANIC 中可被命中：第一批 50 轮的第 23 轮命中一次，`STOPPED pc=4009cd2c`，寄存器与栈可读（`x1=esr=0x86000005`，`x5=sp=0x41097df0`，`x30=0x400cd7f8`）。该次为签名 D，**不是签名 B**。

**仍未验证的部分**：
- 条件 `if $x2 == 1 || $x2 == 8`（只抓签名 B）在真实 PANIC 中的筛选效果未验证。第二批 50 轮没有任何 PANIC，无法检验。
- 签名 B 的栈上 `x30` 写入点仍未定位。
- 挂载 gdb 是否改变 PANIC 频率，未验证（见 7.4）。

### 7.3 待决事项与结束状态

- 签名 B（`elr=far=x30` 为小常量 1/8）的栈上 `x30` 写入点**未定位**。
- 追查路径（条件断点、watchpoint）的成本与收益已评估：签名 B 频率约 1/60 至 1/100，挂 gdb 后可能更低；按当前频率要抓到一次签名 B 并完整回溯写入点，需要数百轮以上。
- **本阶段按观察项结束**：签名 B 记为未定位观察项。若后续需要继续，建议先在不挂 gdb 的前提下统计签名 B 频率，再决定是否重启追查。

### 7.4 挂载 gdb 的轮次统计（本阶段记录）

| 批次 | 断点 | 轮数 | PANIC | 签名 | 命中 |
|---|---|---|---|---|---|
| 第一批 | 无条件 `show_invalid_entry_message` | 50 | 1 | D | 1（第 23 轮） |
| 第二批 | 条件 `$x2 == 1 \|\| $x2 == 8` | 50 | 0 | — | 0 |

两批合计 100 轮仅 1 次 PANIC，低于不挂 gdb 时的频率（见第 6 节）。样本太小，无法判断是否由 gdb 挂载造成，**未验证**。

