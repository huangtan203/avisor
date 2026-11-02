# Phase 115：PCI 资源池加固与桥窗口预留

## 目标

在 Phase 114 best-fit allocator 上增加有序空闲区间维护、释放合并、区间预留和资源统计。桥窗口根据子树 BAR 汇总后，从全局资源池预留窗口覆盖的范围，避免其余可分配空闲地址继续落入桥转发窗口。

## 实现

- pool 分配器增加参数、计数、地址范围、算术溢出和对齐检查；按地址排序维护空闲区间。
- `pool_free()` 验证释放范围位于 pool 内且不与空闲范围重叠，按序插入并合并前后相邻区间。BAR transaction rollback 在恢复快照前释放本次已经分配的 BAR，pool 快照仍作为事务最终恢复保障。
- `pool_reserve_range()` 从空闲池中裁剪覆盖窗口的区间，支持全覆盖删除、首尾裁剪及中间分裂；保留区间的划分保持有序。桥 MMIO、预取 MMIO 和 I/O 窗口均按其 aperture 粒度向 pool 预留，预留失败会恢复桥扫描状态和 pool 快照。
- 增加 pool 自测，覆盖分配、相邻释放合并、窗口区间中段切分和跨空闲碎片预留。
- `pool_dump_stats()` 在枚举后输出 total、used、free、fragments、largest free range 和 pool base。

## 验证

- `make -f Makefile.virt -j2`：通过。
- QEMU 默认拓扑：pool selftest 通过。MMIO 统计为 total=0x2eff0000、used=0x105000、free=0x2eeeb000、fragments=2、largest=0x2edf0000；I/O 为 total=0x10000、used=0x20、free=0xffe0、fragments=1、largest=0xffe0。virtio-net loopback、edu liveness、factorial(10)、function reset 与 MSI reinit 通过。
- QEMU nested bridge + e1000 拓扑：递归枚举至 bus 2；e1000 BAR0/BAR1 分别成功分配 MMIO/I/O。两级桥窗口配置成功；预留后 MMIO used=0x200000、fragments=1，I/O used=0x1000、fragments=1。无 reserve failure、rollback warning、PANIC 或 ASSERT。
- `git diff --check -- hypervisor/boards/virt/pci.c`：通过。

## 限制

- 本阶段拓扑未触发空闲表达到 64 项、池耗尽、释放重叠拒绝或桥预留失败；这些错误路径未通过运行时注入覆盖。
- QEMU 测试设备没有预取型 BAR，因此预取窗口仍未获运行时覆盖。
- 当前 PCI allocator 在递归枚举后计算桥窗口，已分配给其他总线设备的 BAR 不会被迁移；窗口预留只从仍空闲区间扣除，不能重新定位既有资源。
- 空闲区间为固定容量 64 的静态数组；64-bit BAR 仍受低于 4GB 的 MMIO aperture 限制，DMA 隔离仍不在本阶段范围。
