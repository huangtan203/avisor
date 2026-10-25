# Phase 53：物理内存规划结果与页池安全接入评估

## 1. 阶段目标

在 Phase 52 规范化区间的基础上增加来源类型和页池接入判定，但不直接替换现有单区间 page allocator。

原因是当前 `struct page_pool` 只有：

```c
start_addr + page_nr + memap
```

所有分配页都由 `start_addr + index * PAGE_SIZE` 计算，无法表示多个不连续空闲区。如果直接把最大候选区替换为 page pool，可能改变现有物理地址布局并遗漏其他早期占用区。

## 2. 内存来源类型

新增区间来源：

```text
PLATFORM_MEMORY_IMAGE    Hypervisor 镜像
PLATFORM_MEMORY_STACK    启动栈
PLATFORM_MEMORY_DTB      DTB blob
PLATFORM_MEMORY_FIRMWARE FDT 或 /reserved-memory 保留区
```

占用区继续经过 DRAM 裁剪、排序、重叠合并，并生成页对齐的空闲区列表。

## 3. 页池接入判定

新增两个诊断字段：

- `candidate_pool_covers_fixed`
- `candidate_pool_safe`

只有同时满足以下条件，规划结果才会被标记为可安全接入：

1. DTB 和保留区描述有效；
2. 区间数组没有溢出；
3. 没有保留区与固定 page pool 冲突；
4. 候选区覆盖当前固定 page pool 的完整范围；
5. 候选区非空。

当前 QEMU DTB 中 DTB 位于 page pool 内，因此最大空闲区从 DTB 之后开始：

```text
candidate = 0x48205000 + 893MB
covers-fixed = no
safe = no
```

这正是预期结果：规划器不会误将不连续或被 DTB 分割的区域声明为可直接替换固定 page pool。

## 4. 运行时策略

本阶段不修改：

- `virt_page_pool.start_addr`；
- `virt_page_pool.page_nr`；
- `get_free_page()` 的索引模型；
- 启动页表；
- Guest 镜像加载地址；
- ITS、GIC、task 和页表分配路径。

规划结果只用于诊断和下一阶段的 allocator 设计输入。

## 5. 验证结果

强制构建通过：

```text
make -B -f Makefile.virt -j$(nproc)
```

标准 QEMU GICv3 DTB 输出：

```text
reserved=0 overflow=0 pool-conflict=no
memory-map occupied=3 free=4 overflow=0
image=0x40080000+660KB dtb=0x48000000+2067KB
candidate=0x48205000+893MB covers-fixed=no safe=no
memory-config=valid mmio-config=valid runtime=enabled
```

Phase 18–27 全部回归通过，GICR affinity、vSGI、Stage-2、内存回收、KSM/CoW、冻结协议、迁移、task 回收和槽位复用均正常。

日志中已有的 EDU 和 vITS 设备能力失败属于既有测试环境/设备能力结果，不是本阶段引入的错误；核心回归和 Hypervisor 初始化未受影响。

## 6. 后续工作

下一阶段应实现支持多个空闲区的物理页池，至少需要：

- `page_pool_range[]` 多区间描述；
- 每区间独立 bitmap 和 cursor；
- 跨区间分配与释放校验；
- 在所有早期分配完成前冻结内存规划；
- 明确 Guest image、页表、ITS command queue 和 task 辅助页的占用来源；
- 用负向 DTB 测试验证每个 carveout 都不会被 allocator 返回。

在这些条件完成前，固定连续 page pool 是唯一启用路径。
