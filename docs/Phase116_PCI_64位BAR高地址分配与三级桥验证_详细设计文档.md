# Phase 116：PCI 64-bit BAR 高地址分配与三级桥验证

## 目标

在 Phase 115 的 PCI 资源池上，为 64-bit MMIO BAR 从空闲区间高端分配资源；32-bit MMIO BAR 仍从低端分配。通过三级桥嵌套拓扑验证 BAR 分配、递归枚举和窗口配置。

## 实现

- 新增 `pool_alloc_high()`。best-fit 选择满足尺寸的空闲范围，并将分配地址按 BAR 尺寸对齐到不超过范围末端的最高合法地址。
- 复用 pool 的区间分裂、排序和释放规则；保持资源池容量限制和失败返回语义。
- MMIO 分配按 BAR 类型分流：64-bit BAR 使用高端分配，32-bit BAR 使用原低端分配；I/O BAR 策略不变。
- 扩展 pool 自测，覆盖高端分配后范围剩余及释放合并。

## 验证

- `make -f Makefile.virt -j2`：构建、链接通过。
- 使用 `edu`、两级嵌套 `pci-bridge` 和桥下 `e1000` 启动 QEMU；串口输出记录于 `/tmp/avisor-phase116-nested.log`。
- pool selftest 通过。
- 64-bit virtio-net BAR4 分配为 `0x3efec000/0x4000`；32-bit BAR1 分配为 `0x10000000/0x1000`，edu BAR0 为 `0x10100000/0x100000`。
- e1000 在 bus 2 被发现，BAR0 为 `0x10020000/0x20000`；两级桥窗口均为 `0x10000000-0x100fffff`，覆盖下游设备。
- 日志没有 PANIC、ASSERT 或 BAR allocation failure。QEMU 由 timeout 正常结束；本次只检查到启动和部分 guest 测试输出，没有将所有 guest 测试作为完整通过结果记录。

## 限制

QEMU virt 的当前 MMIO aperture 仍在 32-bit 地址范围内，因此本次只验证 64-bit BAR 的高端分配策略，没有验证 4 GiB 以上地址。仅验证历史定义中的两级 bridge 设备链（bus 0→1→2）；更深层级和 pool 容量边界未做压力测试。
