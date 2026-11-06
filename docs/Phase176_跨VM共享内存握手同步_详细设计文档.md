# Phase 176：跨 VM 共享内存握手同步 详细设计文档

## 1. 问题

回归中 `shm` 跨 VM 测试稳定失败：

- VM2：`timeout waiting for PING`
- VM1：`timeout waiting for PONG`
- `Phase 19: SOME tests FAILED`

## 2. 根因

测试流程是 VM1 写 `PING` 等 `PONG`，VM2 等 `PING` 后回 `PONG`。两侧都用 50000 次 `HVC_SCHED_YIELD` 作为超时。

问题在于**两个 VM 的测试阶段没有同步点**。实测时间线（`/tmp/phase174A.log`）：

- VM2 在 Test 5.3 入口即开始等待 PING。
- VM1 仍在 Test 5.1 调度测试，远未到达 Test 5.3。
- VM2 的 50000 次 yield 在 VM1 到达前耗尽，VM2 超时并 detach。
- VM1 之后才 attach，此时已无对端，PING 无人应答。

`yield` 次数不是根因，加大上限只是延长窗口，不能保证同步。修复必须是显式的同步握手。

另已核实：共享页在 refcount 归 0 时**不销毁**（`shm_detach` 的持久化语义），因此两 VM 通过 `SHM_TEST_KEY` 拿到的是同一 `shmid=0` 与同一物理页。问题不在共享页本身。

## 3. 方案

在共享页偏移 8 增加 READY 标志，与 PING/PONG（偏移 0–4）互不冲突：

1. VM2 attach 后置 `shm_ptr[8] = 'R'`，然后进入 PING 等待。
2. VM1 attach 后先等 `shm_ptr[8] == 'R'`（最多 1000000 次 yield），再写 PING。
3. VM2 看到 PING 后回 PONG。

VM1 在等待 READY 超时时以明确的 `VM1 timeout waiting for VM2 READY` 失败，并正确 detach，不留下悬空映射。

## 4. 为什么选此方案

- **不改语义**：仍是 PING/PONG 的双向验证，只是加入了就绪确认。
- **不依赖时序巧合**：READY 保证 VM2 已 attach，PING 不会早于对端。
- **不改 hypervisor**：`shmem.c` 的持久化语义与 attach/detach 不变，修复局限在 guest 测试。

## 5. 放弃的方案

- 仅加大 yield 上限：延长窗口但不保证同步，且会让失败时的等待时间变长。
- 改用中断通知（`HVC_SHM_NOTIFY`）：文档 5.3 的原设计明确选择轮询以“避免两个 VM 启动时序不一致导致 notify 丢失”，改回通知会推翻该设计决策。

## 6. 验证

- 构建 `make -B -f Makefile.virt -j2` 通过（exit 0）。
- 35 秒 QEMU 回归，三次运行结果一致：
  - `shm: PASSED` 出现 2 次（VM1、VM2），`shm: FAILED` 为 0。
  - `Phase 19: SOME tests FAILED` 为 0。
  - `EL2 task stack overflow` 为 0。
- 握手顺序（首次运行）：
  ```
  [vm2] shm: VM2 ready, waiting for 'PING'...
  [vm1] shm: VM1 wrote 'PING', waiting for 'PONG'...
  [vm2] shm: VM2 received 'PING'
  [vm2] shm: VM2 replied 'PONG'
  [vm1] shm: VM1 received 'PONG' from VM2
  ```

## 7. 修正记录

实现过程中一次编辑误把 PING 写了两次，且第一次写入位于 READY 等待之前，违背设计。已删除前一段写入，并重新核对 `main.c` 确认仅剩一次 PING 写入。
