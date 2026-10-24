# Phase 44：Zombie task 槽位实际复用验证详细设计

## 1. 阶段目标

Phase 42/43 已验证 zombie task 能在切栈后安全回收，但此前回归统计中的 `reused=0` 仅表示启动拓扑没有在回收后继续创建 task。本阶段增加受限的 Hypercall 自测，实际完成“创建—标记 zombie—回收—再次创建”的闭环，确认空槽位能够被重新分配。

## 2. 实现内容

### 2.1 复用计数

`create_task_on_cpu()` 在成功将 task 放入已有空槽时递增 per-CPU `task_reused_count`。只有 task 页、辅助资源和任务表插入全部完成后才更新统计，分配失败不会产生虚假计数。

### 2.2 HVC 73

新增：

```c
#define HVC_TASK_SLOT_REUSE_TEST 73
```

Hypercall 仅允许以下身份调用：

- VMID：3；
- vCPU：0；
- 物理 CPU：0。

该限制避免普通 Guest 创建任意 Hypervisor task。

自测流程：

1. 创建一个不带 loader 和设备初始化的探针 task；
2. 将探针标记为 `TASK_ZOMBIE`；
3. 调用本地 deferred reap 回收；
4. 再创建第二个探针 task；
5. 再次回收第二个探针；
6. 返回两次分配得到的 PID。

探针不进入调度器，不关联 Guest VM，不改变 vCPU 注册表或设备模型。

### 2.3 Guest Test 27

迁移完成后由 VM3 vCPU0 调用 HVC 73，要求：

```text
ret == 0
first_pid == second_pid
```

满足条件即证明回收后的相同 task 表槽位被再次使用。

## 3. 回归结果

双核 QEMU 回归日志确认：

```text
Test 26: high-water=4 holes=1 reaped=1 reused=0
Test 27: first-pid=2 second-pid=2
PASS: 回收后的 task 槽位已被重新分配
```

Test 26 验证安全回收，Test 27 验证真实槽位复用。QEMU 因 Guest 主循环没有主动退出而由外部 timeout 终止，未观察到 PANIC 或相关 FAIL。

## 4. 兼容性与边界

- 保留 Phase 42 的 HVC 72 只读统计接口；
- 保留已有 VM2/CPU0 HVC 身份门禁；
- HVC 73 不向普通 VM 开放；
- 不修改用户有意保留的生成文件；
- Test 27 为 Hypervisor 生命周期自测，不模拟真实 Guest 创建完整 VM task。
