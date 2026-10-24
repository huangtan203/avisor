# Phase 39：EL2 task 栈安全加固详细设计

## 1. 背景与目标

每个 task 使用一个 4KB 页同时保存 `task_struct`、EL2 调用栈和页尾异常帧。随着冻结上下文和虚拟化状态增长，结构体扩大会压缩调用栈，严重时会覆盖 vCPU 身份字段或异常现场。

Phase 39 建立静态和运行时双重防护：

- 编译期保证 task 元数据、guard、调用栈和异常帧有明确余量；
- task 初始化时写入 canary；
- 调度切换和进入 Guest 前检查 canary 与 SP；
- 记录历史最低 SP，支持分析栈水位；
- 发生越界时立即 PANIC，而不是继续运行损坏状态。

## 2. 页布局约束

从 task 页低地址到高地址依次为：

```text
task_struct | 64-byte canary | EL2 C stack reserve | pt_regs at page end
```

编译期断言要求：

```text
sizeof(task_struct) + guard + 2048 + sizeof(pt_regs) <= 4096
```

`freeze_context` 不再嵌入 task_struct，而是独立页分配，避免大对象侵占 task 页。

## 3. Canary 检查

初始化阶段使用固定 64 位模式填充 64 字节 guard。检查阶段逐字校验；任一字被修改即报告 VMID、vCPU ID 和 PID 并终止运行。

检查点：

- `switch_to()` 切换前检查当前 task；
- `switch_to()` 切换目标 task 前检查目标 guard；
- `vm_entering_work()` 进入 Guest 前检查当前 task。

## 4. 栈水位

初始化时将最低水位设为异常帧地址。检查当前 task 时读取硬件 SP：

- 更新历史最低 SP；
- SP 越过 guard 或剩余空间低于 256 字节时 PANIC；
- `task_stack_min_free()` 返回当前 task 的历史最小可用空间。

该检查不依赖调试构建，避免损坏元数据后继续执行。

## 5. 验证

- 构建与 `git diff --check` 通过；
- 双核 QEMU Test 18–24 全部通过；
- 100 轮冻结未触发 guard、SP 或 canary 错误；
- 迁移、事务冻结和源 VM 回收行为保持不变。
