# Phase 59：Tegra234 高地址 Stage-1 发布与 Bring-up 门控

## 1. 目标

Phase 58 能识别 Tegra HSUART，但运行时页表发布仍只允许 QEMU virt。Phase 59 将 Tegra234 纳入 DTB 驱动的 Stage-1 映射流程，同时明确区分：

- QEMU 完整虚拟化运行态；
- Tegra234 硬件 bring-up 诊断态。

这样可以映射 Tegra 的高地址 DRAM、UART 和已发现的 GIC MMIO，又不会继续执行 QEMU 专用 PCI、ITS、Guest 和 SMP 流程。

## 2. 运行时配置条件

QEMU runtime mapping 仍要求：

- DTB 有效；
- 固定 1GB 内存布局验证通过；
- PL011、GICv3、GICR、ITS 和 timer 完整兼容。

Tegra234 runtime mapping 要求：

- DTB 有效且平台根 compatible 匹配 Tegra234/AGX Orin；
- 至少一个有效 DRAM bank；
- reserved-memory 规范化成功；
- UART 节点可用且类型为 Tegra HSUART。

Tegra bring-up 不以 ITS 和 timer 完整兼容作为 UART/DRAM 映射的前置条件。

## 3. 条件化 MMIO 映射

公共映射器总是映射：

- 全部 DRAM banks，属性为 Normal Cacheable；
- UART range，属性为 Device。

资源在 DTB 中存在时再映射：

- GIC Distributor；
- GIC Redistributor；
- ITS。

QEMU PCI BAR 与 ECAM 固定窗口只在 QEMU virt 平台加入，不再污染 Tegra234 的 Stage-1 地址空间。

映射仍使用 Phase 57 的多 PGD/PUD/PMD 规划器，因此支持高地址和跨 512GB L0 window。

## 4. Bring-up 门控

`platform_full_bringup_supported()` 当前只允许 QEMU virt 进入完整启动流程。

Tegra runtime mapping 和 UART 重新初始化完成后，主核打印：

```text
[platform] bring-up only: QEMU devices and guests are disabled
[platform] UART=<base> DRAM-banks=<count> tables=<count>
```

然后停在 EL2 `WFE` 循环，不执行：

- QEMU page pool 固定区间初始化；
- GIC/vGIC/ITS/vITS 初始化；
- QEMU PCI 枚举和 edu 直通；
- Guest 创建与 Stage-2 启动；
- QEMU PSCI CPU1 启动。

这使尚未平台化的驱动不能误访问 QEMU 地址。

## 5. Fallback 兼容性

QEMU 固定布局 fallback 必须保留。即使 QEMU DTB 不完整导致 runtime mapping 失败，只要平台仍识别为 QEMU virt，就继续使用 bootstrap 页表和既有固定设备地址执行完整回归。

因此完整 bring-up 门控以平台类型为主，不强制要求 runtime mapping 成功。

## 6. 验证

执行：

```text
make -f Makefile.virt clean
make -f Makefile.virt
timeout 100s make -f Makefile.virt run-log
```

结果：

```text
[platform] memory-config=valid mmio-config=valid runtime=enabled
[platform] stage1-map=dtb banks=1 tables=4
[platform] page-pool=planned ranges=2 pages=257792
INFO[0]: GICv3 initialized.
[4.25] ITS initialized (GICv3 MSI controller)
[cpu1] GICv3 initialized
[vm1] virtio-net: ALL loopback tests PASSED!
[vm2] virtio-net: ALL loopback tests PASSED!
migration: source VM reclaimed (... done=1)
```

未触发 bring-up-only，未发现 PANIC 或 ASSERT。`run-log` 是长驻目标，timeout 返回 124 属于预期行为。

仓库与当前环境没有可用于启动的真实 Tegra234 DTB/板卡，因此 Tegra 路径完成的是编译和控制流边界验证，尚无真实 MMIO 运行证据。

## 7. 后续阶段

下一阶段应平台化 Tegra234 GICv3 Redistributor 探测、timer PPI 和 PSCI CPU_ON，并建立真实板卡专用链接地址/启动入口。在这些完成前，AGX Orin 仍只支持受控 bring-up，不支持 Guest 运行。
