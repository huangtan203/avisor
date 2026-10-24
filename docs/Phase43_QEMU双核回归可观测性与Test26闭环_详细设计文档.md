# Phase 43：QEMU 双核回归可观测性与 Test 26 闭环详细设计

## 1. 背景与目标

Phase 42 已实现 zombie task 延迟回收和任务槽位复用，但原有 `make run` 使用 `-nographic -serial mon:stdio`。在自动化终端中，QEMU 进程能够运行，却可能看不到串口文本，容易将“输出捕获方式不匹配”误判为 Hypervisor 启动失败。

Phase 43 的目标是：

- 提供稳定的 QEMU 串口文件回归入口；
- 明确区分 QEMU 外部 timeout 与系统运行失败；
- 通过实际双核回归闭环验证 Phase 18–26；
- 固化 Test 26 的 zombie task 回收观测结果。

## 2. 回归入口

新增 Make 目标：

```bash
make -f Makefile.virt run-log
```

默认输出文件为：

```text
/tmp/avisor-qemu.log
```

也可以指定日志路径：

```bash
QEMU_SERIAL_LOG=/tmp/avisor-phase43.log make -f Makefile.virt run-log
```

该入口使用：

- `-display none`；
- `-monitor none`；
- `-serial file:<path>`；
- 与原有 `make run` 相同的 virt、GICv3、双 CPU、1GB 内存和 EDU 设备参数。

## 3. 验证方法

由于 Guest 进入长期运行循环后不会主动退出，回归使用外部 timeout 结束 QEMU。timeout 只表示测试进程被停止，不表示测试失败。判定依据是串口日志中的：

- Test 18 SMP 冻结握手 PASS；
- Test 19 事务冻结 PASS；
- Test 20 VM 级事务冻结 PASS；
- Test 21 双 vCPU 迁移 PASS；
- Test 22 源 VM 回收 PASS；
- Test 23 CANCEL 回滚 PASS；
- Test 24 FINISH abort 回滚 PASS；
- Test 25 EL2 task 栈遥测 PASS；
- Test 26 zombie task 回收 PASS。

## 4. 实际回归结果

本阶段使用双核 QEMU 运行完整回归，结果如下：

- Test 18：双 vCPU 冻结/释放握手 100 轮通过；
- Test 19：COMMIT、ROLLBACK 和边界拒绝通过；
- Test 20：VM 级冻结、现场回卷和重放通过；
- Test 21：迁移后 vCPU1 持续推进通过；
- Test 22：源 VM 回收完成，释放 18 页；
- Test 23：CANCEL 回滚通过；
- Test 24：FINISH 失败原子回滚通过；
- Test 25：最小剩余栈空间 1376 bytes，guard 有效；
- Test 26：`high-water=4 holes=1 reaped=1 reused=0`，zombie task 已在切栈后安全回收。

QEMU 最终由外部 timeout 终止，Guest 在终止前已进入主循环并持续输出 tick，未观察到 PANIC 或新增 FAIL。

## 5. 约束

- 不改变原有 `make run` 的交互式串口行为；
- 不把运行日志或生成目录纳入 Git；
- 不修改 Phase 42 的用户 HVC 门禁和有意保留的生成文件；
- Test 26 的 `reused=0` 表示本次启动拓扑没有在回收后再次创建 task，不影响本次回收安全性验证；槽位复用逻辑由创建路径和统计接口保留供后续创建场景验证。
