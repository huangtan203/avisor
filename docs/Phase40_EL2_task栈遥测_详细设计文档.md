# Phase 40：EL2 task 栈遥测详细设计

## 1. 背景与目标

Phase 39 已加入 task 页 canary、栈最低水位记录和越界 PANIC，但这些状态只能由 EL2 内部检查，Guest 回归无法直接确认每个 vCPU 的栈安全余量。Phase 40 增加只读遥测接口，用于验证运行中的 task 栈状态，同时不改变 Phase 39 的故障处理策略。

目标：

- 查询当前 task 的历史最小剩余 EL2 栈空间；
- 查询 64 字节 guard 是否完整；
- 保持接口只读，不允许 Guest 修改水位或 canary；
- 在既有冻结、迁移和事务测试后执行压力验证；
- canary 损坏和栈耗尽仍由 EL2 检查点直接 PANIC。

## 2. HVC 接口

新增 `HVC_TASK_STACK_STATS = 71`：

```text
输入：无
输出：x0 = 历史最小剩余栈空间（字节）
      x1 = guard 状态（1=有效，0=无效）
```

接口返回当前 HVC 调用所属 task 的数据。PID 0 或无效 task 返回 0/0；正常 Guest task 由 `task_stack_check()` 保证已初始化。

## 3. 实现

- 新增 `task_stack_guard_valid()`，集中执行 canary 校验；
- `task_stack_check()` 复用该函数，避免遥测和 PANIC 路径出现不同校验规则；
- `task_stack_min_free()` 返回 `stack_low_water - guard_end`；
- EL2 HVC handler 将两个结果写入 x0/x1；
- Guest 头文件同步编号并新增 Test 25。

遥测只暴露统计结果，不暴露 task 指针、物理地址或异常帧内容。

## 4. 安全边界

- HVC 不接受地址参数，不能读取其他 task；
- 不允许写入 canary 或重置最低水位；
- `task_stack_check()` 仍在切换任务和进入 Guest 前执行；
- `free < 256`、SP 越过 guard 或 canary 损坏仍立即 PANIC；
- 独立的 `freeze_context` 分配和 task 页静态断言保持不变。

## 5. Guest Test 25

1. 调用 `HVC_TASK_STACK_STATS`；
2. 检查 guard 返回 1；
3. 检查最低剩余栈空间不小于 256 字节；
4. 打印遥测值并报告 PASS/FAIL。

Test 25 在 Test 18–24 后执行，确保冻结和迁移压力之后仍能读取有效栈状态。

## 6. 验证

- 编译期检查 task 页布局；
- `git diff --check`；
- 双核 QEMU 回归 Test 18–25；
- 不出现 stack overflow、stack exhausted、PANIC 或旧测试 FAIL。
