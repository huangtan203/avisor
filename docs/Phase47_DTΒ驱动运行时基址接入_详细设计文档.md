# Phase 47：DTB 驱动运行时基址接入

## 1. 阶段目标

将 Phase 46 探测出的平台信息接入 QEMU virt 驱动初始化，同时保留固定地址兼容路径：

- UART 使用 DTB 探测出的运行时基址；
- GICD/GICR 使用 DTB 探测出的运行时基址；
- timer 继续使用 ARM 架构定时器系统寄存器；
- 内存探测结果继续用于诊断，启动页表和页池保持 QEMU 固定布局；
- AGX Orin 识别后不自动启用 QEMU 专用外设驱动。

## 2. 运行时地址配置

新增平台访问接口：

```c
unsigned long platform_uart_base(void);
unsigned long platform_gicd_base(void);
unsigned long platform_gicr_base(int cpu_id);
int platform_runtime_config_valid(void);
```

C 驱动中的 `UART_BASE`、`GICD_BASE` 和 `GICR_BASE` 改为运行时变量。启动顺序为：

1. 先用 QEMU 默认基址初始化 UART，保证早期诊断可输出；
2. 执行 `platform_early_probe()`；
3. 若 DTB 是 QEMU virt 且字段有效，则覆盖 UART/GIC 运行时基址；
4. 再次初始化 UART；
5. 初始化 GIC、vGIC、ITS 和 timer。

汇编阶段仍使用固定 QEMU 地址，因为启动页表创建发生在 C 代码之前。

## 3. 安全边界

当前只允许 `PLATFORM_KIND_QEMU_VIRT` 的有效 DTB 覆盖运行时驱动基址。AGX Orin 即使被识别，也不会把 Tegra234 地址直接交给 PL011/GIC virt 驱动。

内存方面，本阶段不直接用 DTB 覆盖启动页表和页池：

- 启动页表仍映射 QEMU virt 的固定 DRAM/MMIO 范围；
- 页池仍使用 QEMU 固定 `LOW_MEMORY/HIGH_MEMORY`；
- DTB 内存信息用于探针输出和后续移植准备。

这样可以避免在尚未完成动态页表、保留内存和链接地址处理前误启动真实硬件。

## 4. QEMU 验证

构建：

```bash
make -B -f Makefile.virt -j$(nproc)
```

带 DTB 运行后输出：

```text
[platform] QEMU virt, dtb=0x48000000 (valid)
[platform] cpu=2 memory=0x40000000+1024MB uart=0x09000000 gicd=0x08000000 gicr=0x080A0000
```

Phase 18–27 相关测试均通过，包括：

- SMP/事务冻结；
- VM 级冻结和回滚；
- 双 vCPU migration；
- 源 VM 回收；
- FINISH abort；
- EL2 栈遥测；
- zombie task 回收；
- task 槽位复用。

未发现新增 `PANIC` 或 `FAIL`。

注意：QEMU `virt` 启动时通常会自动通过启动寄存器提供 DTB，即使命令行没有显式 `-dtb` 参数，也可能进入 `valid` 路径。因此本阶段保留了 fallback，但未在当前 QEMU 默认启动链中人为制造无效 DTB 进行单独验证。

## 5. 下一阶段边界

后续仍需完成：

- 动态启动页表和 MMIO 映射；
- 从 DTB 读取并配置 GICR/ITS；
- DTB timer 和中断号解析；
- 动态内存保留区与页池；
- Tegra234 UART 驱动；
- PSCI/CPU 启动；
- AGX Orin 启动链和 SMMU/IOMMU。
