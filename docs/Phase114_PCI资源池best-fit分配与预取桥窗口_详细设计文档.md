# Phase 114：PCI 资源池 best-fit 分配与预取桥窗口

## 目标

将 Phase 113 的线性递增 BAR 分配器改为 MMIO 与 I/O 独立空闲区间池，采用 best-fit 选择并支持分配区间前后分裂。保留 BAR 全量探测和按尺寸降序分配策略；桥在递归扫描子树后，按子设备 BAR 汇总非预取 MMIO、预取 MMIO 与 I/O 窗口。

## 实现

- `pci_resource_pool` 以最多 64 个空闲区间表示资源池。`pool_alloc()` 按请求尺寸自然对齐，在可用区间中选剩余空间最少者，并支持分配前缀、后缀或中间段。分配失败通过返回状态表示，避免将合法 I/O 端口 0 当成失败值。
- MMIO pool 覆盖 QEMU virt PCI MMIO aperture；I/O pool 覆盖 64KB I/O aperture。BAR 分配保留 Phase 113 的先探测、按尺寸降序分配流程。BAR 事务失败时恢复 BAR 配置、资源元数据和两个 pool 的快照。
- 子桥先递归枚举并分配 BAR，再基于其全部后代设备计算三种资源窗口。MMIO 窗口按 1MB 对齐，I/O 窗口按 4KB 对齐。预取窗口使用 PCI Type 1 Bridge 的 64-bit base/limit 类型编码，并写入 0x28/0x2c Upper 32-bit 寄存器；寄存器写入后进行读回比较。
- 桥子树扫描失败时恢复 allocator 快照、bus/device 计数与桥原有 bus number、command 和窗口配置。
- 在 `pci.h` 增加 `PCI_BAR_PREFETCH` 标志，桥窗口分类使用 BAR 属性位判断预取资源。

## 验证

- `make -f Makefile.virt -j2`：通过。
- 基础 QEMU virt 拓扑：通过。virtio-net 与 edu 枚举成功，I/O BAR0 分配到合法端口 0；guest virtio-net loopback、edu liveness、factorial(10)、function reset 与 MSI reinit 均通过。
- 嵌套 PCI bridge + e1000 拓扑：通过。递归扫描到 bus 2，e1000 BAR0 与 BAR1 正常分配，两级桥均配置 MMIO 和 I/O 窗口，未见 mismatch、allocation failure 或 rollback warning。
- 根目录 `make -j2` 未作为目标构建验证：该 Makefile 指向 Raspberry Pi，且现有 `boot.S` 使用汇编器不接受的 `UL` 后缀；virt 专用构建不受此问题影响。

## 限制

- 当前 QEMU 测试拓扑没有预取型 BAR，因此预取窗口编码和寄存器读回路径尚未由运行时设备覆盖。
- 没有注入 pool 耗尽或递归枚举失败，事务回滚路径未做故障注入验证。
- 空闲区间释放和相邻区间合并不用于当前枚举流程；64-bit BAR 仍受现有低于 4GB 的 MMIO aperture 限制，DMA 隔离仍不在本阶段范围。
