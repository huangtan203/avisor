# Phase 52：规范化物理内存区间与候选页池

## 1. 阶段目标

在不改变现有 page allocator、启动页表和链接地址的前提下，建立可供后续动态内存布局使用的物理区间描述：

- 记录 Hypervisor 镜像范围；
- 记录启动栈范围；
- 记录 DTB 实体范围；
- 纳入 FDT 和 `/reserved-memory` 保留区；
- 对占用区排序、裁剪、重叠合并；
- 生成页对齐的空闲区列表；
- 选择最大候选连续页池并输出诊断信息。

本阶段的候选页池仅用于探测和验证，当前 `virt_page_pool` 仍使用固定 `LOW_MEMORY/HIGH_MEMORY`。

## 2. 新增边界

链接脚本新增 `image_end` 符号：

```text
image_base = 0x40080000
image_size = image_end - image_base
```

平台状态新增：

- `occupied[]`：规范化后的占用区；
- `free[]`：页对齐空闲区；
- `occupied_count/free_count`；
- `memory_map_overflow`；
- `image_base/image_size`；
- `dtb_size`；
- `candidate_pool_base/candidate_pool_size`。

所有数组均为固定容量，避免 early probe 依赖动态内存。

## 3. 占用区来源

当前纳入以下范围：

1. Hypervisor 镜像：`image_base .. image_end`；
2. 启动栈：`LOW_MEMORY - MAX_CPUS * THREAD_SIZE .. LOW_MEMORY`；
3. DTB：DTB 地址和 header `totalsize`；
4. FDT memory reservation block；
5. `/reserved-memory/*/reg`。

每个范围在加入前裁剪到 DTB 声明的 DRAM 窗口内，非法范围或数组溢出会增加 `memory_map_overflow`，最终使内存动态配置失效。

## 4. 归一化算法

### 4.1 排序

使用固定数组上的插入排序，按物理基址升序排列。

### 4.2 合并

相邻或重叠区间合并：

```text
next.base <= current.base + current.size
```

合并结果不会跨越 DTB 声明的 DRAM 边界。

### 4.3 空闲区

从 DRAM 起点开始扫描规范化占用区，生成间隙：

```text
free[i] = [previous_end, occupied[i].base)
```

每个间隙向上、向下按 `PAGE_SIZE` 对齐。最大间隙被记录为候选 page pool。

## 5. QEMU 验证结果

标准 QEMU GICv3 DTB 运行输出：

```text
reserved=0 overflow=0 pool-conflict=no
usable=0x48205000+893MB
memory-map occupied=3 free=4 overflow=0
image=0x40080000+660KB dtb=0x48000000+2067KB
memory-config=valid mmio-config=valid runtime=enabled
```

结果说明：

- Hypervisor 镜像和启动栈被排除；
- DTB 所在的 `0x48000000` 区间被排除；
- 候选区从 DTB 结束后的页边界开始；
- 当前固定 page pool 未被修改；
- Phase 18–27 全部通过；
- CPU0/CPU1 GICR affinity 发现正常。

## 6. 安全边界

本阶段仍不把 `candidate_pool_*` 写入 `virt_page_pool`，原因是以下范围还没有完全纳入：

- Guest 镜像装载区的所有运行时副本；
- 页表运行时扩展区；
- Hypervisor task 页和辅助页的动态生命周期；
- 固件平台上的多个 DRAM bank；
- `reserved-memory` 的 `size/alloc-ranges` 动态分配语义；
- DTB 自身是否会在启动后被固件复用。

因此现阶段候选区只用于观测和下一阶段设计输入，不代表已经可以安全替换 allocator。

## 7. 后续工作

下一阶段应将规范化区间接入一个独立的物理内存规划器：

1. 增加区间类型和来源标记；
2. 精确登记 Hypervisor、页表、Guest image、DTB 和固件 carveout；
3. 为多个 DRAM bank 生成可分配区集合；
4. 通过规划结果初始化 page pool；
5. 在改变启动页表前增加完整的映射覆盖验证。
