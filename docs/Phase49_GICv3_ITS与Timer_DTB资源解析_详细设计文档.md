# Phase 49：GICv3、ITS 与 Timer DTB 资源解析

## 1. 阶段目标

在保持 QEMU 固定启动页表不变的前提下，补全 GICv3 相关 DTB 资源解析和运行时校验：

- 解析 GICD/GICR 的基址和区域大小；
- 解析可选 `redistributor-stride`，缺失时使用架构最小步长 `0x20000`；
- 精确识别 `arm,gic-v3` 与 `arm,gic-v3-its`；
- 解析 ITS 基址和区域大小；
- 解析 `arm,armv8-timer` 的 virtual/hypervisor PPI；
- 只有完整资源与现有 QEMU ABI 一致时才启用运行时配置；
- 保持 Phase 18–27 行为不变。

## 2. QEMU GICv3 DTB 布局

本阶段使用 QEMU `virt,gic-version=3` 生成的 DTB 验证绑定格式。

GIC 主节点：

```text
compatible = "arm,gic-v3"
reg = <GICD base, GICD size>, <GICR base, GICR region size>
```

QEMU 双核配置的关键值为：

```text
GICD = 0x08000000 + 0x10000
GICR = 0x080A0000 + 0xF60000
```

ITS 子节点：

```text
compatible = "arm,gic-v3-its"
reg = <0x08080000 0x20000>
```

架构 timer 节点按标准顺序提供四个 PPI：

1. secure physical timer；
2. non-secure physical timer；
3. virtual timer；
4. hypervisor physical timer。

QEMU 对应 INTID 为 `29/30/27/26`，本项目使用后两项。

## 3. 精确 compatible 匹配

原有 `text_has()` 是任意字节子串匹配，会把：

```text
arm,gic-v3-its
```

误认为：

```text
arm,gic-v3
```

本阶段增加 DTB string-list 精确匹配，仅在完整字符串相等时识别：

- `arm,gic-v3`；
- `arm,gic-v3-its`；
- `arm,armv8-timer`。

GIC 主节点、ITS 子节点和 timer 节点分别维护解析状态，避免子节点 `reg` 覆盖 GICD/GICR 信息。

## 4. 运行时资源校验

运行时 MMIO 配置必须同时满足：

1. UART、GICv3、GICR、ITS 和 timer 属性均由 DTB 提供；
2. GICR stride 不小于 `0x20000`；
3. GICR region size 至少为 `cpu_count * stride`；
4. ITS size 覆盖 `GITS_TRANSLATER` 末端 `0x10044`；
5. UART、GICD、GICR、ITS 完整区域位于当前启动页表映射的 MMIO 窗口；
6. hypervisor timer PPI 为 26；
7. virtual timer PPI 为 27。

timer PPI 当前只用于验证，不改变 Guest ABI。Guest virtual timer 仍使用 INTID 27，EL2 tick 仍使用 INTID 26。

任一条件不满足时统一恢复以下 QEMU 固定值：

```text
GICR stride = 0x20000
ITS base    = 0x08080000
ITS size    = 0x20000
timer PPI   = 26/27
```

## 5. 驱动接入

GIC Redistributor 地址由平台配置计算：

```c
platform_gicr_base(cpu_id)
```

不再在 GIC 驱动中直接乘固定 `VIRT_GICR_STRIDE`。

ITS 寄存器访问改用运行时 `GITS_BASE`。MSI doorbell 返回物理地址：

```c
platform_gits_base() + 0x10040
```

当前 QEMU 启动映射仍为 VA=PA；接口明确保留 MSI 消息使用物理地址的语义。

## 6. 验证结果

强制构建通过：

```text
make -B -f Makefile.virt -j$(nproc)
```

GICv3 DTB 正向路径：

```text
[platform] gicr-size=0xF60000 stride=0x20000
[platform] its=0x08080000+0x20000 timer-ppi=26/27
[platform] memory-config=valid mmio-config=valid runtime=enabled
```

Phase 18–27 全部通过，包括：

- Stage-2 内存管理；
- KSM/CoW；
- EL2 task 栈保护；
- SMP 冻结协议；
- VM-wide freeze；
- 双 vCPU live migration；
- zombie task 回收和槽位复用。

GICv2 DTB 负向路径：

```text
[platform] memory-config=valid mmio-config=fallback runtime=fixed-layout
```

系统继续使用固定 GICv3/GICR/ITS 地址，GICv3 和 ITS 初始化成功，未发生 EL2 同步异常。

## 7. 当前边界与后续工作

本阶段仍不改变：

- 启动汇编页表；
- DRAM 和 MMIO 映射窗口；
- page pool；
- Guest timer IRQ ABI；
- CPU affinity 到 Redistributor frame 的映射方式。

当前 Redistributor 仍按 `CPU ID * stride` 定位。面向 Tegra234 的后续阶段需要遍历 GICR frame，并依据 `GICR_TYPER` affinity 匹配 CPU，而不能假设 frame 顺序与逻辑 CPU ID 一致。之后还需实现动态 MMIO 页表、Tegra234 early UART 和固件保留内存处理。
