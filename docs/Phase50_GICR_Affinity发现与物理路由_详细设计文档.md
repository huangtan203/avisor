# Phase 50：GICR Affinity 发现与物理路由

## 1. 阶段目标

移除 GICv3 驱动对 Redistributor frame 顺序的假设：

```text
GICR(cpu) = region_base + logical_cpu_id * stride
```

改为读取每个 frame 的 `GICR_TYPER.Affinity_Value`，与物理 CPU MPIDR affinity 匹配。同时让 SGI 和 SPI 路由使用完整物理 affinity，为 Tegra234 等多 cluster 平台做准备。

本阶段保持以下边界不变：

- QEMU 固定启动页表；
- `MAX_CPUS=2`；
- 逻辑 CPU ID 和 per-CPU 调度结构；
- Guest vCPU MPIDR；
- Guest timer ABI；
- AGX Orin 硬件驱动仍不启用。

## 2. 问题分析

Phase 49 已从 DTB 获取 GICR region 和 stride，但驱动仍按逻辑 CPU ID 计算 frame。该模型只适用于 frame 顺序恰好等于逻辑 CPU 顺序的平台。

真实 GICv3 平台通过 `GICR_TYPER[63:32]` 描述 Redistributor 对应的 PE affinity。多 cluster SoC 的 Aff1/Aff2/Aff3 可能非零，frame 顺序也不保证与软件逻辑 CPU ID 一致。

项目中的 `smp_processor_id()` 读取 `TPIDR_EL2`。该寄存器只保存调度器使用的逻辑 CPU ID，不能代替真实 MPIDR affinity。并且启用虚拟化后直接读取 `MPIDR_EL1` 可能受到虚拟 MPIDR 状态影响，因此必须在启动早期保存物理值。

## 3. 物理 MPIDR 保存

新增：

```c
unsigned long platform_cpu_mpidr[MAX_CPUS];
```

主核在 BSS 清零前读取物理 `MPIDR_EL1`，清零完成后写入数组。

次核在早期 EL2 路径读取完整 MPIDR，并暂存于 callee-saved 寄存器。收到主核启动信号后，BSS 已完成初始化，此时再发布到数组，避免被主核清零覆盖。

`TPIDR_EL2` 继续只保存逻辑 CPU ID，既有调度和 per-CPU 数据结构不变。

## 4. Redistributor affinity 扫描

GIC 驱动对目标 CPU 执行以下流程：

1. 从 `platform_cpu_mpidr[cpu]` 提取 Aff3:Aff2:Aff1:Aff0；
2. 从 DTB 或固定回退配置取得 GICR region base、size 和 stride；
3. 逐个读取 `GICR_TYPER`；
4. 比较 `GICR_TYPER[63:32]` 与目标 affinity；
5. 匹配后缓存该 CPU 的 Redistributor 基址；
6. 遇到 `GICR_TYPER.Last` 或 region 边界后停止；
7. 未找到目标 frame 时触发 PANIC，禁止初始化错误 CPU 的 SGI/PPI frame。

扫描边界至少要求一个标准 128KB Redistributor frame，且不会越过 Phase 49 已验证的 GICR region。

## 5. SGI 与 SPI 物理路由

### 5.1 SGI

`ICC_SGI1R_EL1` 不再把逻辑 CPU ID 直接当作 Aff0，而是使用目标 CPU 的物理 MPIDR：

- Aff3 → `[55:48]`；
- Aff2 → `[39:32]`；
- INTID → `[27:24]`；
- Aff1 → `[23:16]`；
- Aff0 → TargetList。

当前未启用 RSS，若 Aff0 大于等于 16，则拒绝发送并输出警告，避免路由到错误 PE。

### 5.2 SPI

`GICD_IROUTER` 使用从物理 MPIDR 生成的完整 32-bit affinity，不再只写 Aff0。QEMU 的 Aff1/Aff2/Aff3 为零，因此现有行为保持不变。

## 6. 安全回退

GICv2 或不完整 DTB 仍由 Phase 49 降级为固定 QEMU GICv3 布局：

```text
GICR base   = 0x080A0000
GICR size   = MAX_CPUS * 0x20000
GICR stride = 0x20000
```

Phase 50 即使在固定布局下也读取 `GICR_TYPER` 并执行 affinity 匹配，而不是恢复逻辑 ID 乘 stride 的旧路径。

## 7. 验证结果

强制构建通过：

```text
make -B -f Makefile.virt -j$(nproc)
```

GICv3 DTB 正向路径：

```text
GICv3: CPU0 MPIDR=0x80000000 Redistributor=0x80a0000
GICv3: CPU1 MPIDR=0x80000001 Redistributor=0x80c0000
```

Phase 18–27 全部通过，包括 SGI、vSGI、GICD/GICR、Stage-2、冻结协议、双 vCPU 迁移和 task 槽位复用。

GICv2 DTB 回退路径：

```text
memory-config=valid mmio-config=fallback runtime=fixed-layout
GICv3: CPU0 MPIDR=0x80000000 Redistributor=0x80a0000
GICv3: CPU1 MPIDR=0x80000001 Redistributor=0x80c0000
```

固定布局下 GICv3、ITS 和次核初始化正常，无 PANIC。

## 8. 后续工作

下一阶段应从“地址发现”推进到真实硬件启动映射：

- 解析 DTB `reserved-memory` 和固件 carveout；
- 建立可验证的动态 DRAM/MMIO 映射描述；
- 为 Tegra234 增加 early UART 映射和驱动；
- 处理非连续 GICR region 或多个 Redistributor region；
- 根据固件 CPU 启动协议完善 PSCI affinity 到逻辑 CPU ID 的映射。
