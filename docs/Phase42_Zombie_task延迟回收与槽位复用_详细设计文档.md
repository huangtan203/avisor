# Phase 42：Zombie task 延迟回收与槽位复用详细设计

## 1. 背景与目标

Phase 41 在迁移完成后释放了源 vCPU0 的辅助资源，但保留其 task page 和任务表槽位，以避免源 task 仍运行在自身 EL2 栈上时被释放。长期运行会造成 task page 和槽位泄漏。

Phase 42 的目标是：

- 在旧 task 完成切栈后安全释放 zombie task page；
- 释放 task 的辅助资源并清空任务表槽位；
- 创建 task 时优先复用空槽位；
- 将 `nr_tasks` 定义为任务表有效扫描上界，而不是存活任务数；
- 为任务回收和槽位复用提供只读统计接口。

## 2. 生命周期与安全边界

### 2.1 zombie 产生

迁移完成路径先释放源 task 的 FIFO 和 `freeze_context`，再将源 task 标记为 `TASK_ZOMBIE` 并调用 `schedule()`。此时旧 task 的 EL2 栈仍可能被当前执行流使用，不能立即释放 task page。

### 2.2 切栈后回收

`cpu_switch_to()` 保存旧 task 的上下文、恢复新 task 的上下文，并在 `mov sp, x9` 后切换到新 task 的 EL2 栈。切栈完成后调用 `task_reap_after_switch(prev)`：

1. 只处理已经离开当前 CPU 的 zombie task；
2. 先将 task 从 `task[]` 槽位摘除；
3. 收缩末端空槽对应的 `nr_tasks` 高水位；
4. 释放 FIFO、`freeze_context` 和 task page；
5. 累加回收统计。

因此释放动作不会发生在旧 task page 作为当前 EL2 栈的阶段。回收接口拒绝跨 CPU 直接操作，避免把另一个 CPU 仍可能使用的栈误判为安全。

## 3. 任务表语义

`task[0]` 永远是本核 idle task。普通 task 使用 `[1, nr_tasks)` 范围内的槽位，其中允许存在 `NULL` 空槽。创建任务时：

1. 扫描有效范围并优先选择第一个空槽；
2. 没有空槽时使用 `nr_tasks` 尾部槽；
3. `nr_tasks == NR_TASKS` 且没有空槽时返回失败；
4. 只有使用尾部槽时才提高 `nr_tasks`。

任务列表、shell、UART 和设备扫描路径均跳过 `NULL` 槽位，避免把已回收槽位当作 task 解引用。

## 4. 统计接口

新增 `HVC_TASK_REAP_STATS`（72），返回当前 CPU：

- `x0`：任务表高水位；
- `x1`：高水位范围内的空槽数量；
- `x2`：累计回收 task 数；
- `x3`：累计复用槽位次数。

Guest Test 26 在迁移完成后读取这些统计，确认源 zombie task 已在切栈后回收，并输出槽位复用观测值。

## 5. 修改范围

- `task.c`：槽位选择、切栈后回收、生命周期统计；
- `sched.S`：切换到新栈后调用回收 helper；
- `sched.c`：任务列表跳过空槽；
- shell/UART：索引 task 前检查槽位非空；
- HVC 头文件和异常处理：增加统计查询；
- `guests/el1_test`：增加 Test 26。

## 6. 验证结果

- `git diff --check` 通过；
- `make -f Makefile.virt -j$(nproc)` 通过；
- QEMU 双核命令可启动，但本轮 60 秒超时未捕获串口回归输出，因此 Test 18–26 尚不能宣称通过；
- 未恢复或覆盖用户有意修改的生成文件和 HVC 门禁修改。
