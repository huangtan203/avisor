# Phase 175：EL2 任务栈扩大到 8KB 详细设计文档

## 1. 问题

回归运行中持续出现 `EL2 task stack overflow` PANIC，自 Phase 164 起每次运行约 43900 次（日志刷屏），且出现在 `PCI: edu function reset complete` 之后。

## 2. 根因（实测）

EL2 任务栈 = `THREAD_SIZE`（原 4096），与 `task_struct` 共用一块内存：

| 项目 | 字节 |
|---|---|
| `sizeof(struct task_struct)` | 1080 (0x438) |
| `TASK_STACK_GUARD_SIZE` | 64 |
| `pt_regs`（`task_pt_regs` 保留） | 272 (0x110) |
| 可用栈预算 | 约 2680 |

单帧占用（`-O0`，`-fstack-usage` 实测）：`handle_hvc64` 1072、`handle_mem_abort` 416、`handle_trap_system` 384、`handle_sync_exception` 240、`its_setup_pci_msi` 240、`pci_edu_reinit_msi` 208、`pci_edu_function_reset` 176。

HVC -> edu reset -> MSI 重建的调用链叠加后，超过 2680 字节，越界覆写 guard 金丝雀，触发 PANIC。

注意：`pci_probe_bars`（帧 2800）在启动期 `pci_init` 运行，不在任务栈上，**不是**本次 PANIC 的来源。

## 3. 方案

将 `THREAD_SIZE` 从 4096 改为 8192，预算提升到约 6776 字节。

由于 `task_pt_regs()` 按 `THREAD_SIZE` 计算、且结构体与栈必须连续，`task_struct` 必须改为**一次分配连续 2 页**：

- 新增 `allocate_task_struct()`：调用 `allocate_block_pages(2)`，返回 `TO_VADDR(pa)`。
- 新增 `deallocate_task_struct()`：调用 `free_block_pages(TO_PADDR(tsk), 2)`。
- 原 5 处 `deallocate_page(task)` 全部替换为块释放。
- `freeze_context` 仍为单页（独立分配，不受影响）。

两次 `allocate_page()` 不保证相邻，因此不能拼接成 8KB 栈。

## 4. 为什么不是其它方案

- 仅抽取 `handle_hvc64` 分支：已尝试，帧仅从 1072 降到 1040（省 32 字节），收益不足以覆盖预算。
- 放宽 guard 或关闭检测：会掩盖真实溢出，禁止。

## 5. 副作用与检查

- `platform.c` 的 `PLATFORM_MEMORY_STACK` 预留随 `THREAD_SIZE` 翻倍（`MAX_CPUS * 8KB`）。该区间位于 `LOW_MEMORY` 之下，已确认不与镜像（起于 `DRAM_BASE`）重叠。
- `INIT_TASK` / `cpu_state[].init_task` 为静态结构，pid=0，不经过块分配，`task_stack_check` 对其提前返回。
- 所有 `task_pt_regs()` 调用点均针对 VM 任务，idle 不调用。

## 6. 诊断改进

`task_stack_check` 的 PANIC 信息增加 `sp` 与 `guard_end`，后续可直接量化栈余量，不必依赖推断。

## 7. 验证结果

- 构建：`make -B -f Makefile.virt -j2` 通过（exit 0）；`git diff --check` 通过；IDE 诊断为空。
- 回归（35 秒 QEMU）：
  - `EL2 task stack overflow` PANIC：**0 次**（修复前每次运行约 43900 次）。
  - 日志总行数：2386（修复前约 44000+，因 PANIC 刷屏）。
  - 通过：`edu MSI PASSED`、`vITS: overflow eventid rejected without ITS stall`、`vITS: PASSED`、`share vSPI 72`、`edu function reset complete`。

## 8. 已知未解决问题（不属于本阶段）

`shm` 测试失败（VM2 `timeout waiting for PING`、VM1 `timeout waiting for PONG`），`Phase 19: SOME tests FAILED`。

原因判断：两个 VM 的 shm 测试时序不同步——VM2 先等待 PING，VM1 在很久之后才写入，VM2 已超时 detach。这属于测试同步问题，与栈改动无关，但**未经基线对比证实**，因为修复前 PANIC 截断了输出，shm 测试从未在可靠环境中完整跑过。本阶段不声称修复该问题。
