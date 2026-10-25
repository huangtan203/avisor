# Phase 56：多 1GB 地址窗口的 EL2 Stage-1 页表规划器

## 1. 目标

Phase 55 的运行时映射器只替换 `PUD[0]` 下的一张 PMD 表，因此所有 DTB 外设必须位于低 1GB。Phase 56 将 Stage-1 运行时规划扩展为多个 1GB PUD window，为高地址 MMIO 和非 QEMU DRAM 布局建立页表基础。

本阶段仍保持 QEMU virt 驱动兼容，不宣称已经可以直接在 AGX Orin 上启动完整 Hypervisor。

## 2. 地址层级

当前 Stage-1 配置为 4KB granule、48-bit VA：

- L0/PGD：`PGD_SHIFT=39`，每个根表 entry 覆盖 512GB；
- L1/PUD：`PUD_SHIFT=30`，每个 entry 覆盖 1GB；
- L2/PMD：`PMD_SHIFT=21`，每个 entry 覆盖 2MB；
- L2 block descriptor 直接建立等值映射。

`TCR_T0SZ=16` 已足够覆盖本阶段的地址范围，限制来自页表存储而不是地址宽度。

## 3. 页表池布局

`pg_dir` 从 5 页扩大为 34 页：

| 页 | 用途 |
|---:|---|
| 0 | bootstrap PGD |
| 1 | bootstrap PUD |
| 2 | bootstrap DRAM PMD |
| 3 | bootstrap MMIO PMD |
| 4 | runtime shadow PGD |
| 5 | runtime shadow PUD |
| 6–33 | runtime PMD table pool |

bootstrap 页在主核开启 MMU 后保持有效，直到 shadow 页表完整构造。页表存储位于 `image_end` 之前，因此会被平台 occupied/free memory map 排除，不会进入 page pool。

## 4. 运行时规划流程

1. 保留 bootstrap 页表，继续提供早期 UART 和 C 代码执行环境；
2. 清零 shadow PGD/PUD；
3. 将运行时 DRAM、UART、GICD、GICR、ITS 和 QEMU PCI 窗口按范围切分；
4. 每遇到新的 1GB window，就从 PMD pool 分配一页并写入 PUD table descriptor；
5. 每个 2MB section 写入 Normal 或 Device block descriptor；
6. 若地址越过 48-bit Stage-1 范围或 PMD pool 耗尽，规划失败并保留 bootstrap 页表；
7. 规划成功后执行 TLBI、DSB、ISB，并切换 `TTBR0_EL2` 到 shadow PGD；
8. 更新 bootstrap PGD entry，使次核启动代码继续使用 `pg_dir` 时能访问同一 shadow PUD。

切换点安排在 CPU1 和中断子系统初始化之前。页表规划失败不会发布不完整的根表。

## 5. 安全性

不能在当前正在使用的 bootstrap PGD 上先写 invalid descriptor，否则发布代码自身可能失去地址翻译。实现先构造完整 shadow 表，再执行：

```text
DSB ISHST
TLBI ALLE2
DSB ISH
ISB
MSR TTBR0_EL2, shadow_pgd
DSB ISH
ISB
```

shadow 页表只使用尚未被 bootstrap 映射引用的页，避免构造过程中破坏当前有效的 DRAM/MMIO PMD。

## 6. 当前边界

- 目前仍只解析并使用一个 DTB memory tuple；完整多 bank DRAM 规划留待后续阶段；
- 当前运行时入口仍要求 QEMU virt 的完整 GICv3、ITS、timer 配置；
- 每个 L0 window 仍使用单个 shadow PUD，若未来需要跨 512GB L0 window，需要增加多个 PUD table 和 PGD entry；
- 当前页表池最多支持 28 个 runtime PMD table，容量不足时安全降级到 bootstrap；
- QEMU PCI 窗口仍是固定布局，因为 PCI host bridge 尚未由 DTB 解析。

## 7. 验证

强制构建通过：

```text
make -f Makefile.virt
```

QEMU 回归观察到：

```text
[platform] memory-config=valid mmio-config=valid runtime=enabled
[platform] stage1-map=dtb sections=validated
[platform] GICv3 initialized.
[4.25] ITS initialized (GICv3 MSI controller)
[vm1] virtio-net: ALL loopback tests PASSED!
[vm2] virtio-net: ALL loopback tests PASSED!
```

`run-log` 是长驻运行目标，使用 `timeout` 后返回 124 属于预期；本次日志未出现 PANIC 或 ASSERT。

## 8. 后续阶段

下一阶段应增加 DTB memory bank 数组和跨 bank 的 Normal mapping 规划，并针对 Tegra234 的真实高地址 UART、GIC、DRAM 和启动链逐项适配。当前代码仍不能视为 AGX Orin 的完整可部署版本。
