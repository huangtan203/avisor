# Phase 48：DTB 内存与 MMIO 配置安全校验

## 1. 阶段目标

在动态页表和真实硬件驱动尚未完成前，为 DTB 运行时配置建立明确的安全边界：

- 验证 DTB 内存范围是否与当前 QEMU 固定启动布局一致；
- 验证 UART、GICD、GICR 是否位于已映射 MMIO 窗口；
- 只有 DTB 确实提供了完整属性时才允许覆盖运行时基址；
- 验证 GIC compatible，禁止把 GICv2 的 `reg` 布局当作 GICv3；
- 配置不匹配时保留固定布局，不访问未映射地址；
- 保持 Phase 18–27 行为不变。

## 2. 配置发现与有效性

`platform_info` 新增以下状态：

```c
int memory_config_valid;
int mmio_config_valid;
int memory_discovered;
int uart_discovered;
int gic_discovered;
int gicv3_discovered;
```

解析器只有在实际发现相应 DTB 属性后才设置 `discovered`。默认 QEMU 地址不再被误认为来自 DTB。

运行时配置启用条件为：

1. DTB 有效；
2. 平台为 QEMU virt；
3. 内存节点已发现；
4. 内存基址和大小与当前固定页表一致；
5. UART 和 GIC 节点已发现；
6. GIC compatible 包含 `arm,gic-v3`；
7. UART、GICD、GICR 均位于启动页表已映射的 MMIO 窗口。

任一条件不满足，驱动继续使用固定 QEMU 地址。

## 3. GIC reg 解析

GICv3 DTB 的 `reg` 包含多个地址/大小元组：

- 第一个元组为 GICD；
- 第二个元组为 GICR。

解析器按照根节点的 `#address-cells` 和 `#size-cells` 计算元组长度，不再固定假设地址单元数量。

测试中发现默认生成的旧 DTB 描述 GICv2，其第二个 `reg` 元组不是 GICR。若直接使用，会访问 `0x08010014` 并触发 EL2 同步异常。增加 `arm,gic-v3` compatible 校验后，该 DTB 会安全降级：

```text
memory-config=valid mmio-config=fallback runtime=fixed-layout
```

## 4. 内存安全边界

本阶段不动态改变以下内容：

- 启动汇编中的 DRAM 块映射；
- MMIO 块映射；
- Hypervisor 链接地址；
- `LOW_MEMORY/HIGH_MEMORY`；
- page pool 起始地址和页数。

因此 DTB 内存必须严格等于当前 QEMU 配置：

```text
base = 0x40000000
size = 1GB
```

不匹配时仅报告探测结果，不允许其影响 allocator 或驱动。

## 5. 验证结果

### GICv3 匹配路径

```text
[platform] QEMU virt, dtb=0x48000000 (valid)
[platform] cpu=2 memory=0x40000000+1024MB
[platform] uart=0x09000000 gicd=0x08000000 gicr=0x080A0000
[platform] memory-config=valid mmio-config=valid runtime=enabled
```

Phase 18–27 全部通过，未发现新增 `PANIC` 或 `FAIL`。

### GICv2 不匹配路径

```text
[platform] memory-config=valid mmio-config=fallback runtime=fixed-layout
```

系统保持固定 GICv3 地址，不再访问错误的 GICR 地址，也未触发同步异常。

## 6. 后续工作

下一阶段可继续实现：

- 从 DTB 解析 GICR region size、stride 和 ITS；
- 解析架构 timer PPI；
- 基于 DTB 生成动态 MMIO 映射；
- 根据镜像、DTB、reserved-memory 和固件 carveout 建立动态页池；
- 为 Tegra234 增加独立 early UART 和启动页表。
