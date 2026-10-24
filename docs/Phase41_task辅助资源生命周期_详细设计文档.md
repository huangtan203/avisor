# Phase 41：task 辅助资源生命周期详细设计

## 1. 背景与目标

Phase 37–39 将冻结上下文从 `task_struct` 移到独立页，并为每个 task 创建输入/输出控制台 FIFO。此前这些辅助页只在创建时分配，缺少统一释放路径；迁移完成后源 vCPU0 进入 ZOMBIE，但其 task 页仍被调度表保留，辅助页会持续占用页池。

Phase 41 补齐辅助资源生命周期，同时保持 task 页本身由调度表持有，避免 zombie 槽位悬空。

## 2. 资源模型

每个非 idle task 包含：

```text
task page       调度表持有，直到任务槽位统一回收
freeze_context  独立页，与 task 生命周期一致
console.in_fifo 独立页，与 task 生命周期一致
console.out_fifo 独立页，与 task 生命周期一致
```

`destroy_task_aux()` 只释放后三类辅助资源，不释放 task page，也不从调度表删除 task。

## 3. 创建失败回滚

任务创建按以下顺序分配：

1. task page；
2. freeze_context page；
3. board/device 初始化；
4. console input/output FIFO；
5. 写入调度表。

FIFO 分配失败时，释放已经成功分配的 FIFO、freeze_context 和 task page，并回退 `nr_tasks` 与 task 槽位。`create_fifo()` 在页池耗尽时返回 NULL，避免空指针解引用。

## 4. 迁移完成释放

FINISH 已完成源 VM 资源回收、vCPU 身份切换和事务提交后：

1. 释放源 vCPU0 的 console FIFO；
2. 释放源 vCPU0 的 freeze_context；
3. 保留源 vCPU0 task page 和 ZOMBIE 槽位；
4. 唤醒目标 VM3 vCPU0 并调度切换。

失败 abort、CANCEL 和正常迁移重试路径不调用该释放函数，因此源 task 仍可继续使用其辅助资源。

## 5. 安全约束

- `destroy_task_aux(NULL)` 安全返回；
- 已释放指针立即置 NULL，重复调用不会重复释放；
- 不释放 current task page，避免 `schedule()` 前访问失效的 task 元数据；
- vCPU1 就地转换为目标 VM 时保留其辅助页，因为该 task 继续运行；
- idle task 没有动态辅助资源。

## 6. 验证

- FIFO 与 task 生命周期代码通过构建和 `git diff --check`；
- 双核 QEMU Test 18–25 全部通过；
- 正式迁移后源 VM 回收仍成功；
- FINISH abort 后重试仍成功；
- 不出现空 FIFO 解引用、PANIC 或 stack 检查失败。
