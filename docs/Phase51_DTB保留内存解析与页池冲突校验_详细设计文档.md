# Phase 51：DTB 保留内存解析与页池冲突校验

## 1. 阶段目标

为后续动态内存布局和 AGX Orin 移植补全固件保留内存探测：

- 解析 FDT header 的 memory reservation block；
- 解析 `/reserved-memory` 子节点的 `reg`；
- 限制保留区数量并检测非法、溢出范围；
- 判断保留区是否与当前固定 QEMU page pool 冲突；
- 输出当前布局下可连续使用的 page pool 前缀；
- 冲突时禁止启用 DTB 动态运行时配置；
- 不修改现有 allocator、启动页表和链接地址。

## 2. 当前固定内存布局

QEMU virt DRAM：

```text
0x40000000–0x80000000  1GB
```

Hypervisor 固定保留前 16MB：

```text
0x40000000–0x41000000
```

当前 page pool：

```text
LOW_MEMORY  = 0x41000000
HIGH_MEMORY = 0x80000000
size        = 1008MB
```

Phase 51 只检查 DTB 保留区是否与该固定 page pool 相交，不改变 `virt_page_pool`。

## 3. 支持的 DTB 保留区来源

### 3.1 Memory reservation block

FDT header 的 `off_mem_rsvmap` 指向一组大端 64-bit 元组：

```text
(address, size)
```

列表由 `(0, 0)` 终止。解析器验证：

- reservation block 偏移位于 `totalsize` 内；
- 每个元组完整；
- 必须存在终止元组；
- 地址加大小不能溢出。

### 3.2 `/reserved-memory`

解析器识别根节点下的 `/reserved-memory`，读取其：

```text
#address-cells
#size-cells
```

随后解析直接子节点的全部 `reg` 元组。每个属性允许包含多个保留区。

## 4. 有界记录结构

`platform_info` 最多记录 16 个保留范围：

```c
struct platform_reserved_range {
    unsigned long base;
    unsigned long size;
};
```

同时记录：

- `reserved_count`：成功保存数量；
- `reserved_overflow`：超出容量或地址范围非法的数量；
- `page_pool_conflict`：是否与固定 page pool 相交；
- `usable_memory_base/size`：当前固定 page pool 起点开始、首个冲突前的连续前缀；
- `reserved_config_valid`：保留区描述是否可安全用于当前布局。

有限数组避免在 early probe 阶段依赖动态内存分配。

## 5. 冲突和可用区间规则

只对位于 DTB DRAM 范围内的保留区执行 page pool 冲突判断。

若保留区与：

```text
[LOW_MEMORY, HIGH_MEMORY)
```

相交，则：

```text
page_pool_conflict = yes
reserved_config_valid = false
memory_config_valid = false
runtime = fixed-layout
```

`usable_memory_size` 表示从 `LOW_MEMORY` 到首个冲突起点的连续空间，仅用于诊断和后续设计输入，当前 allocator 不使用该值。

这意味着系统不会声称 DTB 内存配置安全，但仍按原固定 QEMU 布局启动，便于暴露配置矛盾。本阶段不承诺在冲突 DTB 下长期安全分配；真实部署前必须让 allocator 排除所有保留区。

## 6. 启动观测

新增输出：

```text
[platform] reserved=<count> overflow=<count> pool-conflict=<yes|no> usable=<base>+<MB>
```

标准 QEMU GICv3 DTB 没有保留项：

```text
reserved=0 overflow=0 pool-conflict=no usable=0x41000000+1008MB
memory-config=valid mmio-config=valid runtime=enabled
```

## 7. 验证结果

### 7.1 正向回归

强制构建通过。使用标准 QEMU GICv3 DTB：

```text
reserved=0 overflow=0 pool-conflict=no usable=0x41000000+1008MB
```

Phase 18–27 全部通过，包括 Stage-2、KSM/CoW、SMP 冻结、VM-wide freeze、双 vCPU live migration、zombie task 回收和槽位复用。

### 7.2 负向测试

在 FDT reservation map 注入：

```text
0x42000000 + 0x200000
```

结果：

```text
reserved=1 overflow=0 pool-conflict=yes usable=0x41000000+16MB
memory-config=fallback mmio-config=valid runtime=fixed-layout
```

系统正确识别该范围位于固定 page pool 内，并拒绝启用 DTB 动态运行时配置。GICv3、ITS 和 CPU1 仍可按固定布局初始化。

## 8. 当前边界

本阶段尚未实现：

- 从多个空洞生成真正的可分配区集合；
- 动态调整 `virt_page_pool`；
- 排除 Hypervisor 镜像、DTB 自身、Guest 镜像和页表的精确范围；
- 解析 `alloc-ranges`、`size` 等动态 reserved-memory 分配请求；
- 动态 DRAM 页表；
- Tegra234 固件 carveout 的实际部署验证。

下一阶段应建立规范化物理内存区间表，将 DRAM、保留区、Hypervisor 自占区和 DTB 区域做排序、裁剪与合并，再选择安全 page pool，而不是继续依赖单个连续固定范围。
