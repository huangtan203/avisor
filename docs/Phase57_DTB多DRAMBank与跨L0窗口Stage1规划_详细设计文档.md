# Phase 57：DTB 多 DRAM Bank 与跨 L0 窗口 Stage-1 规划

## 1. 目标

Phase 56 已能在一个 512GB L0 window 内按多个 1GB PUD window 建立 Stage-1 映射，但仍只保存一个 DTB memory tuple，并固定使用 `PGD[0]`。Phase 57 扩展为：

- 解析并保存最多 8 个 DTB DRAM bank；
- 对 bank 排序并合并相邻或重叠区间；
- occupied/free memory map 分别在每个 bank 内计算，不把 bank 间空洞作为 RAM；
- Stage-1 按需创建多个 PGD/L0 entry、PUD/L1 table 和 PMD/L2 table；
- 保持 QEMU 固定 1GB page pool 策略和启动 fallback。

本阶段建立 Tegra234 高地址 DRAM 的页表基础，但不包含真实 Orin UART、GIC、PSCI、SMMU 和启动固件适配。

## 2. 多 DRAM Bank 数据模型

`platform_info` 增加：

```c
struct platform_reserved_range memory_banks[PLATFORM_MAX_MEMORY_BANKS];
unsigned int memory_bank_count;
unsigned int memory_bank_overflow;
```

`memory_base/memory_size` 继续表示规范化后的第一个 bank，用于兼容现有 QEMU 校验和诊断。DTB memory 节点的全部 `reg` tuples 会逐项进入 bank 数组。

规范化执行：

1. 按物理基址升序排列；
2. 合并重叠或相邻 bank；
3. 更新兼容字段；
4. bank 超过 8 个或范围溢出时标记 overflow，并禁止发布 page-pool plan。

## 3. 多 Bank 内存规划

occupied ranges 先分别与每个 DRAM bank 求交，因此 image、stack、DTB 和 reserved-memory 只会占用真实 RAM 内的部分。

free ranges 按 bank 独立生成：

```text
for each bank:
    cursor = bank.base
    subtract occupied ranges intersecting this bank
    emit [cursor, bank.end)
```

bank 之间的物理空洞不会成为 free range。QEMU allocator 仍在 `virt_page_pool_init()` 中把这些 free ranges 裁剪到 `[LOW_MEMORY, HIGH_MEMORY)`，因此本阶段不会改变现有 1GB 页池容量。

## 4. 跨 L0 Window 的 Stage-1 规划

Stage-1 使用 4KB granule 和 48-bit VA：

- PGD/L0 index：`address >> 39`，每项覆盖 512GB；
- PUD/L1 index：`(address >> 30) & 511`，每项覆盖 1GB；
- PMD/L2 index：`(address >> 21) & 511`，每项覆盖 2MB。

运行时映射器使用通用的 `runtime_get_next_table()`：

1. 在 shadow PGD 中按 L0 index 获取或创建 PUD；
2. 在 PUD 中按 L1 index 获取或创建 PMD；
3. 在 PMD 中写入 2MB Normal 或 Device block descriptor。

因此不同 DRAM bank 或 MMIO range 可以位于不同 512GB L0 window。

## 5. 静态页表池

`pg_dir` 扩大到 128 页：

| 页 | 用途 |
|---:|---|
| 0–3 | bootstrap PGD/PUD/PMD |
| 4 | runtime shadow PGD |
| 5–127 | 按需分配的 runtime PUD/PMD table pool |

页表池属于 Hypervisor image occupied range，不进入普通 page pool。容量耗尽时运行时规划失败，并继续使用 bootstrap 固定页表。

## 6. 发布与次核一致性

主核先完整构造 shadow PGD，再执行 TLB 维护并切换 `TTBR0_EL2`。切换完成后，将完整 shadow PGD 复制回 bootstrap 根页。

这样：

- 主核不会在当前执行路径上清空正在使用的根表；
- CPU1 仍可按原启动汇编把 `pg_dir` 装入 `TTBR0_EL2`；
- bootstrap 根页包含全部 L0 entries，而不是只转发 `PGD[0]`。

## 7. QEMU 验证

强制构建通过：

```text
make -f Makefile.virt clean
make -f Makefile.virt
```

运行日志：

```text
[platform] memory-map banks=1 bank-overflow=0 occupied=3 free=4 overflow=0
[platform] stage1-map=dtb banks=1 tables=4
[platform] page-pool=planned ranges=2 pages=257792
INFO[0]: GICv3 initialized.
[4.25] ITS initialized (GICv3 MSI controller)
[cpu1] GICv3 initialized
[vm1] virtio-net: ALL loopback tests PASSED!
[vm2] virtio-net: ALL loopback tests PASSED!
migration: source VM reclaimed (... done=1)
```

QEMU 单 bank 使用 4 页运行时页表：shadow PGD、一个 PUD、MMIO PMD 和 DRAM PMD。`run-log` 使用 timeout 后返回 124 属于长驻任务的预期行为，日志没有 PANIC 或 ASSERT。

当前环境未安装 `dtc/fdtoverlay`，因此没有生成额外合成 DTB；跨 L0 路径通过通用索引计算、静态容量边界和 QEMU 基础路径完成代码级验证。

## 8. 后续阶段

下一阶段应开始 Tegra234 真实 early-console 与中断控制器资源适配，并准备由固件传入 DTB 的真实板卡启动链。当前实现仍不能视为 AGX Orin 完整可部署版本。
