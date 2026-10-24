# Phase 45：平台抽象、DTB 解析与 AGX Orin 启动探针

## 1. 目标

本阶段为真实硬件移植建立最小平台探针，同时保持 QEMU `virt` 现有行为不变：

- 增加统一平台信息接口；
- 增加基础 Flattened Device Tree（FDT/DTB）解析；
- 识别 QEMU virt 与 NVIDIA Tegra234/Jetson AGX Orin；
- 输出 CPU、内存、UART、GIC 基础信息；
- 不启用 AGX GPU、PCI 直通或完整 Guest；
- 在 QEMU 上回归 Phase 18–27。

## 2. 平台信息接口

新增 `include/common/platform.h` 和 `hypervisor/common/platform.c`。

`struct platform_info` 保存：

- 平台类型和名称；
- DTB 地址及有效性；
- CPU 数量；
- 内存基址和大小；
- UART、GICD、GICR 基址。

当前探针只报告配置，不改变已有 `virt` 驱动的固定地址，保证 QEMU 行为兼容。

## 3. DTB 解析

实现了无 libc 依赖的基础 FDT 解析：

- 校验 FDT magic；
- 读取 totalsize、结构块和字符串块；
- 遍历 BEGIN_NODE、END_NODE、PROP、END；
- 读取 root `compatible`；
- 识别 `linux,dummy-virt`；
- 识别 `nvidia,tegra234` 和 `jetson-agx-orin`；
- 统计 CPU 节点；
- 读取 memory 节点的 `reg`。

当前 DTB 地址通过裸机启动约定从主核入口 `x0` 传入并保存到 `platform_dtb_address`。如果地址为 0 或 magic 无效，则使用 QEMU 固定布局 fallback。

## 4. AGX Orin 识别边界

识别 AGX Orin 只表示检测到 Tegra234/Jetson 兼容字符串，不会自动启用任何 Tegra 外设驱动。启动日志会明确提示：

```text
AGX Orin recognized: hardware drivers not enabled
```

因此本阶段不会误把 QEMU virt 的 UART、GIC、PCI 地址用于 AGX Orin。

## 5. QEMU 回归

构建：

```bash
make -B -f Makefile.virt -j$(nproc)
```

QEMU 双核回归通过，启动探针输出：

```text
[platform] QEMU virt (fixed-layout fallback), dtb=0 (fallback)
[platform] cpu=2 memory=0x40000000+1024MB uart=0x09000000 gicd=0x08000000 gicr=0x080A0000
```

Phase 18–27 均到达既有测试路径，未观察到新增 PANIC 或 FAIL；Test 27 继续报告：

```text
first-pid=2 second-pid=2
```

使用 QEMU `-dtb` 参数时，当前 `-kernel` 裸机入口没有把 DTB 地址传入 `x0`，因此该运行仍按 fallback 处理。这是启动链路限制，不是 DTB 解析器把无效地址误判为有效。真实硬件启动器需要按照 AArch64 启动约定传入 DTB 地址。

## 6. 下一阶段边界

Phase 45 之后仍需实现：

- 从 DTB 动态配置 GIC、UART、DRAM 和 timer；
- Tegra234 UART、GICR/ITS 和 PSCI 适配；
- AGX Orin 链接地址和保留内存处理；
- UEFI/NVIDIA boot chain 集成；
- SMMU/IOMMU 和 DMA 隔离。

在这些工作完成前，AGX Orin 仅完成平台识别准备，不能直接启动当前完整 Guest 栈。
