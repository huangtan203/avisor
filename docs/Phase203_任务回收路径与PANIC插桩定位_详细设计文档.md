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

**推断（已验证为可达但未复现）**：Test 26 的 `reaped==0` 可能是 `sched_dequeue_task` 对 zombie 返回非 1，导致回收被短路。插桩已确认该短路分支可达（见第 5.1 节反例），但 30 轮实测中 0 次触发，故**不是已确认根因**。

## 3. 插桩方案

仅在 `hypervisor/arch/aarch64/task.c` 的 `remove_task_slot()` 入口与各返回点加打印，记录：`phys_cpu`、`smp_processor_id()`、`state`、`sched_dequeue_task` 返回值、是否成功。前缀统一为 `[RTDBG]`，便于 grep 与还原。

不改调度行为，不改 vm2 pin。

## 4. 验证方法

- 插桩后跑 30 轮 QEMU 回归（与前文一致的命令）。
- 统计 `[RTDBG]` 中短路原因分布，并与 run 中出现的 FAIL 行对齐。
- 每轮记录 Test 26 的 `reaped` 值。

## 5. 执行与结论

### 5.1 回收路径（已实测，结论：短路路径可达但未复现）

**插桩内容**（已还原）：在 `remove_task_slot()` 的 `sched_dequeue_task(tsk) != 1` 分支加计数与 `WARN("[RTDBG] short dq=%d ...")`；`cpu != smp_processor_id()` 分支加计数；`task_reap_after_switch()` 的 ZOMBIE 分支打印 `remove_task_slot()` 返回值。

**30 轮实测**：

| 项目 | 计数 |
|---|---|
| 轮数 | 30 |
| `PANIC` | 1（`run14`） |
| `[RTDBG] short`（`dq != 1` 短路） | 0 |
| `reap-after-switch ret=0` | 0 |
| `reap-after-switch` 总行数 | 30（全部 `ret=1`） |
| `cpu_mis` 非零 | 0 |

即 30 轮内每次进入 ZOMBIE 回收都成功，**未观察到短路**。

**反例构造（`[RTDBG-NEG]`，已还原）**：在 `task_slot_reuse_selftest()` 末尾新建一个探针任务，先 `sched_dequeue_task()` 把它摘出 runqueue，再调用 `remove_task_slot()`。实测输出：

```
WARN[3]: [RTDBG-NEG] pre-dequeue ret=1 (expect 1)
WARN[3]: [RTDBG-NEG] remove after dequeue ret=0 (expect 0)
```

该轮 `PANIC` 0 次。结论：**`dq != 1` 短路分支可达，`return 0` 路径确实会被执行**。

**限制**：反例为人为构造（先摘再回收），与真实竞态下的调用顺序不必相同；它只证明分支可达，不证明真实运行会出现。

**因此**："`sched_dequeue_task` 返回非 1 导致短路"是**可达但 30 轮未复现**的路径，**不能定为根因**。

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

### 7.5 Test 23 前驱假设的撤回与修正

**撤回**：上一轮曾提出"四次 PANIC 都紧跟在 Test 23 开头，Test 23 是共同前驱"。该判断**不成立**。

**依据**（`[DBG23]` 插桩，40 轮，已还原）：
- Test 23 的 `enter`、`step1`–`step4`、`exit` 六处标记全部按序输出，且 Test 23 PASS。
- 唯一 PANIC（`r22.log`）出现在 Test 23 `exit` 之后、Test 24 `PASS: FINISH 失败已回滚` 之后。
- PANIC 签名：`esr=86000005, elr=0xa, far=0xa, x30=0x20000000a, sp=0x4108bdf0`。`x30` 与 `elr` 不相等，与第 5.3 节签名 B（三者相等）形态不同，**归为新变体，未定性**。

**修正后的结论**：
- Test 23 不能作为 PANIC 的必要前驱。
- 早前 PANIC 日志在 Test 23 开头即被截断，因此"PANIC 位于 Test 23"这一观察是**日志截断造成的假象**，不能反映真实触发位置。
- 样本仅 1 次 PANIC，无法建立统计关系；Test 24 与 PANIC 的关系**未验证**。

**插桩状态**：`[DBG23]` 标记已于本轮还原（`git diff -- guests` 为空，产物中 `DBG23` 计数为 0）。

### 7.6 把 PANIC 收敛到 HVC_MIGRATE_START 内部

第 7.5 节遗留的问题是"Test 24 与 PANIC 的关系未验证"。本节用 `[DBG24]` 插桩把它收敛到具体 hypercall。

**静态核实（先于插桩）**：
- `migration_finish`（`hypervisor/common/migration.c:487`）的失败分支会执行 `mig_src = NULL; mig_state = MIGRATION_IDLE; mig_finish_abort_count++;` 并恢复 vCPU 注册与 vCPU1 的 mm/board_data/name。因此 Test 24 的 FINISH 失败不会留下阻止后续 START 的状态残留，可以排除"状态残留导致 START 被拒"。
- 39 个非 PANIC 轮全部输出 `PASS: FINISH 失败已回滚源/目标状态，可重试`（39/39），说明 Test 24 本身不会导致 PANIC。

**插桩设计**（3 处，`guests/el1_test/main.c`，已还原）：
1. Test 24 块结束后、重写页模式之前：`[DBG24] after-pass`
2. `HVC_MIGRATE_START` 调用之前：`[DBG24] before-start`
3. `HVC_MIGRATE_START` 返回之后：`[DBG24] after-start`

**结果**（40 轮，`/tmp/dbg24_runs/`，PANIC 1 次）：
- 39 个非 PANIC 轮：`after-pass → before-start → after-start` 三条标记全部按序出现。
- 唯一 PANIC 轮（`r39.log`）：只有 `after-pass` 与 `before-start`，**缺 `after-start`**。
- PANIC 行紧接在 `before-start` 之后，中间无任何其他 guest 输出。

**结论**：
- PANIC 发生在 `HVC_MIGRATE_START` 执行期间，而不是 Test 24 内部、也不是 START 之后的代码。范围已从"Test 24 附近"收敛到单个 hypercall 内。
- 该次签名：`esr=97810005, elr=400b0f6c, far=a00b1e3c, x30=400b0f30, sp=4108bc90`。`esr` 的 EC 为 `0x25`（数据中止），`elr != far`，与此前 B/D 形态均不同，**归为新签名，未定性**。

**未验证**：
- 样本仅 1 次，无法建立统计关系。
- 本轮 `esr=0x97810005` 与第 2 节签名 C 的 `esr` 数值相同；但 `elr=0x400b0f6c` 是否落在 `vgic_pending_pop` 内**未核对**，不能据此把本轮并入签名 C。
- `far=0xa00b1e3c` 指向何处**未核对**。

**插桩状态**：`[DBG24]` 三处标记已于本轮还原（`git diff -- guests hypervisor` 为 0 行，`bin_virt/kernel8.img` 中 `DBG24` 计数为 0）。`guests/el1_test/` 下的中间产物（`el1_test.bin`、`el1_test.elf`、`main_c.o`）为构建缓存，非源码，不影响仓库。

### 7.7 签名 C 的访存位置与 vgic_pending 结构体破坏的首次命中

**符号映射**（`build_virt/kernel8.elf`，重建后核实）：
- `elr=0x400b0f6c` = `vgic_pending_pop+84`：`ldr w1, [x0,#12]`，读取 `virt->vgic_pending[head]`。
- `x30=0x400b0f30` = `vgic_pending_pop+24`：函数内 `if (!virt->vgic_pending_count)` 的入口，是返回地址而非访存点。
- 因此签名 C 的访存位于 `vgic_pending_pop` 读取 pending 槽位处，`far` 由 `virt` 基址与 `head` 共同决定。

**VPDBG 守卫（40 轮，`/tmp/vpdbg_runs/`）**：在 `vgic_pending_pop` 入口检查 `head`/`tail` 是否越界（`>= VGIC_PENDING_CAPACITY=32`）。
- 唯一一次命中（`r36.log`）：`WARN[1]: [VPDBG] bad idx head=1091091760 tail=0 count=1091091760 virt=4108a000`。
- 随后同一轮 PANIC：`esr=97810005, elr=400b1088, far=452b951c, x30=400b104c`。
- `head` 与 `count` 数值相同，均为 `0x4108BD30`（`printf '%x'` 换算核实）。该值与同批 PANIC 的 `sp` 同属 `0x4108bxxx` 栈区，**可能**是栈地址被写入 `head`/`count` 字段，**未验证**。`virt=0x4108a000` 本身为合法地址。
- `head`（`virt+80`）与 `count`（`virt+88`）相隔 8 字节，**可能**由同一越界写入覆盖，**未验证**。

**同批其他 PANIC**：
- `r17.log`：`esr=97c08006, elr=400b4708, far=11e93c8c`，且同一轮出现 `EL2 task stack` 耗尽。该轮无 VPDBG 输出，归为签名 A，与 `vgic_pending` 破坏无关。

**未验证与限制**：
- 样本仅 1 次命中，无法建立统计关系。
- `r36` 的 `elr=400b1088` 与前次 `400b0f6c` 不同，是否仍在 `vgic_pending_pop` 内**未核对**。
- 谁写入了 `head`/`count` 的越界值，**未定位**。需在 `vgic_pending_push`/`pop` 的写入处或对 `virt+80`/`virt+88` 加写入记录才能确认。

**插桩状态**：`[VPDBG]` 守卫已于本轮还原（`git diff -- hypervisor guests` 为 0 行，`bin_virt/kernel8.img` 中 `VPDBG` 计数为 0）。
