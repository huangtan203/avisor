# Phase 117：PCI 分配策略框架与桥热插拔资源预留

## 目标

在 Phase 116 的高地址 BAR 分配上统一资源池分配入口，支持 first-fit、best-fit、worst-fit 和 high-best 四种策略；为桥的 bus number、MMIO、prefetch MMIO 和 I/O 窗口静态预留扩展空间。

## 实现

- `pci_resource_pool` 增加策略字段，`pool_init()` 接受策略参数。
- 统一 `pool_alloc()`：first-fit 找到第一个可用区间即分配；best-fit 选 waste 最小的区间；worst-fit 选 waste 最大的区间；high-best 选 waste 最小区间并从高端分配。
- 提取 `pool_split()` 复用区间分裂逻辑。MMIO pool 默认 best-fit；I/O pool 默认 first-fit；64-bit MMIO BAR 在分配期间临时使用 high-best。
- 资源池自测覆盖四种策略的选择行为以及高端分配和释放合并。
- 桥 subordinate bus 在扫描完成后额外预留最多 4 个 bus number，并限制在 `PCI_MAX_BUSES - 1` 内；推进 `pci_next_bus` 防止后续桥分配冲突。
- 桥 MMIO 和 prefetch MMIO 窗口在实际 BAR 覆盖范围外尽量扩展 25%，至少预留 1 MiB，并按 1 MiB 对齐；I/O 窗口尽量扩展 25%，至少预留 1 KiB，并按 4 KiB 对齐。扩展只使用当前资源池紧邻窗口末端的可用范围，并将完整窗口从全局 pool 移除。
- 保留现有桥扫描和窗口预留失败回滚逻辑。

## 验证

- `make -f Makefile.virt -j2`：通过，无编译警告。
- 默认 QEMU 拓扑：pool selftest 通过；virtio-net 64-bit BAR4 为 `0x3efec000/0x4000`；MMIO pool 显示 best-fit、I/O pool 显示 first-fit。
- 嵌套桥拓扑：QEMU 扫描到 bus 2 的 e1000；内外桥 MMIO 窗口均扩展为 `0x10000000-0x101fffff`，I/O 窗口为 `0x0-0x1fff`；pool selftest 通过；捕获日志未见 PANIC、ASSERT、BAR allocation failure 或 reservation failure。
- QEMU 以 timeout 结束，符合 Hypervisor scheduler 持续运行方式。本次日志用于验证启动与 PCI 枚举，没有把所有 guest 测试记录为完整通过。日志文件为 `/tmp/avisor-phase117-final.log`。

## 限制

- 桥 bus reserve 受全局 `PCI_MAX_BUSES` 限制；本次嵌套 QEMU 日志没有打印 bus-number register，因此只通过实现边界和扫描行为检查，未做运行时读回断言。
- 预留是启动时静态规划，并非动态热插拔控制器；未模拟后续热插设备，也未对 pool 耗尽及回滚失败做故障注入。
- 当前 QEMU virt 资源 aperture 不能验证 4 GiB 以上的预取 BAR。
