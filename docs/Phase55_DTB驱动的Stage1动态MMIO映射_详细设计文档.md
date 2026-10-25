# Phase 55：DTB 驱动的 Stage-1 动态 MMIO 映射

## 1. 阶段目标

Phase 55 将已经验证的 DTB 运行时资源进一步接入 EL2 Stage-1 页表：

- 保留可独立启动的 QEMU 固定 bootstrap 映射；
- 在 DTB 解析完成后验证 UART、GICD、GICR 和 ITS 的物理范围；
- 使用 DTB 资源构造独立的运行时 MMIO PMD；
- 通过 break-before-make 安全替换 EL2 Stage-1 映射；
- DTB 不完整或资源范围不可映射时保持原固定页表；
- 保持 Phase 18–27 和多区间 page pool 回归行为不变。

## 2. 既有启动映射

`boot_virt.S` 在进入 C 代码前建立 5 页页表：

1. PGD；
2. PUD；
3. DRAM PMD；
4. 固定 MMIO PMD；
5. 预留页。

固定映射包括：

- `0x40000000` 起的 1GB DRAM，Normal Memory；
- `0x08000000` 起的 32MB QEMU 外设窗口，Device Memory；
- `0x10000000` 的 PCI BAR section；
- `0x3f000000` 起的 16MB PCI ECAM。

DTB 地址和解析器依赖 DRAM、UART 已经可访问，因此不能在开启 MMU 前直接用 DTB 完全替代 bootstrap 页表。本阶段采用两段式启动：先用固定映射进入 C，再构造并切换经过验证的运行时映射。

## 3. DTB 资源补充

`struct platform_info` 增加：

```c
unsigned long uart_size;
int runtime_mapping_valid;
```

FDT `reg` 解析现在同时保存 PL011 UART 的基址和长度。GICD、GICR、ITS 的基址和长度沿用此前阶段已经解析的数据。

只有 `platform_runtime_config_valid()` 成立，并且四类关键 MMIO 范围都满足以下条件时才允许切换：

- 长度非零；
- `base + size` 不溢出；
- 范围位于当前 1GB PUD 窗口内；
- QEMU virt 的设备类型、GICv3、timer 和资源完整性检查已经通过。

## 4. Shadow PMD 构造

启动页表的第 5 个预留页作为 shadow MMIO PMD。切换前先离线清零并按 2MB section 对齐映射：

- UART；
- GIC Distributor；
- GIC Redistributor region；
- GIC ITS region。

页表项使用 `MMU_DEVICE_FLAGS`，保持 Device nGnRnE 属性。

PCI host bridge 尚未从 DTB 解析，因此本阶段继续在 shadow PMD 中保留：

- QEMU PCI BAR section；
- QEMU PCI ECAM 16MB 窗口。

这样动态切换不会破坏现有 edu PCI、MSI 和 vITS 测试路径。

## 5. 安全切换

`platform_apply_runtime_mappings()` 只有在完整验证和 shadow PMD 构造成功后才修改在线页表。PUD 表项替换遵循 Arm break-before-make 顺序：

1. 将原 `PUD[0]` 置为 invalid；
2. `DSB ISHST`；
3. `TLBI ALLE2`；
4. `DSB ISH` 和 `ISB`；
5. 写入指向 shadow PMD 的新 table descriptor；
6. `DSB ISHST` 和 `ISB`。

切换发生在 CPU1 启动及中断初始化之前，不存在其他 CPU 同时使用旧映射的问题。

如果前置验证失败，函数直接返回，不清除或替换在线 PUD，启动日志显示：

```text
[platform] stage1-map=fixed-layout
```

成功时显示：

```text
[platform] stage1-map=dtb sections=validated
```

## 6. 初始化顺序

主核当前顺序为：

1. 使用固定 UART bootstrap 映射初始化串口；
2. 解析并验证 DTB；
3. 构造并应用运行时 Stage-1 MMIO 映射；
4. 根据规范化 free ranges 初始化多区间 page pool；
5. 使用运行时 UART 地址重新初始化串口；
6. 初始化调度器、GICv3、ITS、Guest 和 CPU1。

这保证运行时设备驱动启动前，所需 MMIO 地址已经由新页表覆盖。

## 7. 验证结果

### 7.1 构建和静态检查

```text
git diff --check
make -B -f Makefile.virt -j$(nproc)
```

均通过。VS Code 对 `platform.c` 未报告诊断。

### 7.2 QEMU GICv3 正向路径

```text
[platform] stage1-map=dtb sections=validated
[platform] page-pool=planned ranges=2 pages=257792
[vm1] PASSED: 大页 + 内存配额正确
[vm1] PASSED: 页面回收功能正确
[vm1] PASSED: KSM 内存去重 + CoW 功能正确
[vm2] PASSED: 大页 + 内存配额正确
[vm2] PASSED: 页面回收功能正确
[vm2] PASSED: KSM 内存去重 + CoW 功能正确
migration: source VM reclaimed (pages=21 shm=0 ksm=249 done=1)
```

Phase 18–27、双 vCPU 迁移、源 VM 回收、zombie 回收和 task 槽位复用均通过，未出现新增 `FAILED`、`PANIC` 或同步异常。

### 7.3 不兼容 DTB 降级路径

使用 QEMU GICv2 DTB 时：

```text
[platform] memory-config=valid mmio-config=fallback runtime=fixed-layout
[platform] stage1-map=fixed-layout
```

证明不完整/不兼容的运行时 MMIO 配置不会切换 shadow 页表。随后 GICv3 驱动访问 GICv2 布局产生异常属于既有的平台驱动不兼容，不是动态页表切换造成。

## 8. 当前边界

- 动态映射当前只支持低 1GB 地址空间内的 2MB section；
- DRAM PMD 仍由启动汇编按 QEMU 固定 1GB 布局建立；
- PCI ECAM/BAR 尚未从 DTB host bridge 节点解析；
- AGX Orin 的 UART、GIC 和 DRAM 通常位于不同物理窗口，需要支持多个 PUD/PMD 表后才能应用；
- 当前 virt 驱动仍是 PL011 和 QEMU GICv3 实现，不能直接驱动 Tegra234 硬件。

下一阶段应扩展 Stage-1 页表规划器，使其支持多个 1GB 地址窗口和 DTB DRAM bank，为 Tegra234 高地址 MMIO 与真实启动映射建立基础。
