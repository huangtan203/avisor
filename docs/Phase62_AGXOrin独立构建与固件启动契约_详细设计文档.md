# Phase 62：AGX Orin 独立构建与固件启动契约

## 1. 目标

Phase 61 已提供 Tegra234 CPU0 GICv3 与 EL2 timer 的受控 bring-up 路径，但工程仍只生成链接到 QEMU DRAM 的镜像。Phase 62 增加独立 Tegra 构建产物，并明确固件加载契约，同时保持 QEMU 行为不变。

本阶段只完成构建与交付准备，不代表镜像已经在 AGX Orin 实机启动成功。

## 2. 独立构建目标

执行：

```text
make -f Makefile.tegra clean
make -f Makefile.tegra verify-layout
```

输出：

```text
bin_tegra/kernel8.img
build_tegra/kernel8.elf
build_tegra/kernel8.list
```

`Makefile.tegra` 复用现有 virt 源码，但设置：

```text
PLATFORM_TEGRA=1
BUILD_DIR=build_tegra
OUTDIR=bin_tegra
LINKER_FILE=hypervisor/boards/virt/linker_tegra.ld
```

因此 Tegra 与 QEMU 的对象、ELF 和 raw image 不会互相覆盖。

## 3. 固件启动契约

Tegra raw image 必须满足以下契约：

1. 固件将 `bin_tegra/kernel8.img` 原样加载到物理地址 `0x80080000`；
2. 跳转目标为 `0x80080000`；
3. CPU 以 AArch64 EL2 或 EL3 进入，EL3 路径由现有入口降级到 EL2；
4. `x0` 提供有效、可直接访问的 DTB 物理地址；
5. CPU0 首先进入，其他 CPU 保持离线或由固件等待；
6. 镜像使用从 `0x80000000` 开始的 bootstrap DRAM window；
7. DTB 必须描述 Tegra234、DRAM、HSUART、GICv3、timer 和 PSCI。

当前入口也保留对若干兼容 DTB 参数位置的探测，但实机交付应以 `x0` 传递 DTB 为准。

## 4. 链接布局

Tegra 链接脚本将 `_start` 固定在：

```text
0x80080000
```

验证结果：

```text
_start    = 0x80080000
pg_dir    = 0x80123000
image_end = 0x801a3000
```

QEMU 镜像保持：

```text
_start    = 0x40080000
pg_dir    = 0x40123000
image_end = 0x401a3000
```

两者均保留相对 DRAM base 的 512KB 加载偏移。

## 5. Bootstrap 页表适配

启动汇编此前固定写入 `PUD[1] -> DRAM PMD`，只适用于 `0x40000000`。Phase 62 根据 `VIRT_DRAM_BASE >> PUD_SHIFT` 选择 PUD entry：

- QEMU `0x40000000` 使用 `PUD[1]`；
- Tegra `0x80000000` 使用 `PUD[2]`。

PMD block 仍按 `VIRT_DRAM_BASE` 建立初始 1GB Normal Memory 映射，随后由 DTB runtime Stage-1 planner 发布最终映射。

## 6. 早期 MMIO 安全

Tegra 构建在 DTB 解析和 runtime mapping 发布之前不会调用 QEMU PL011 初始化。

如果 runtime mapping 构造失败，Tegra 直接进入静默 `WFE`，不会退回 QEMU 固定 UART/GIC 地址。成功后才初始化 DTB 发现的 Tegra HSUART。

该策略牺牲了映射失败时的串口诊断，但避免因访问不存在或未映射的 QEMU MMIO 导致同步异常。

## 7. 构建系统参数化

`Makefile.virt` 的以下变量允许板级包装 Makefile 覆盖：

```text
PLATFORM_DEFINES
BUILD_DIR
OUTDIR
LINKER_FILE
```

QEMU 默认值不变。`verify-layout` 使用 ELF symbol table 检查 `_start` 必须等于 `0x80080000`，避免链接脚本配置错误后生成不可启动镜像。

## 8. 验证结果

### 8.1 Tegra 静态构建

```text
make -f Makefile.tegra clean
make -f Makefile.tegra verify-layout
```

结果：

```text
Image: bin_tegra/kernel8.img
Tegra image entry verified: 0x80080000
```

镜像大小约 229KB。这里只验证编译、链接和入口地址，不验证硬件执行。

### 8.2 QEMU 回归

QEMU 干净构建和长驻运行回归通过：

```text
[platform] stage1-map=dtb banks=1 tables=4
INFO[0]: GICv3 initialized.
[4.25] ITS initialized (GICv3 MSI controller)
[cpu1] GICv3 initialized
[vm1] virtio-net: ALL loopback tests PASSED!
[vm2] virtio-net: ALL loopback tests PASSED!
migration: source VM reclaimed (... done=1)
```

未发现 PANIC 或 ASSERT。`run-log` 返回 124 是 timeout 终止长驻 QEMU 的预期结果。

## 9. 实机首次验证清单

在可恢复的 AGX Orin 环境中，首次运行应按顺序确认：

1. 固件确实加载到 `0x80080000`；
2. 跳转时 `CurrentEL` 为 EL2 或 EL3；
3. `x0` 指向有效 DTB；
4. 串口首先出现平台、memory map 和 Stage-1 日志；
5. 出现 `Tegra CPU0 GICv3 initialized`；
6. 出现 `Tegra EL2 timer enabled; heartbeat active`；
7. 心跳持续输出且没有同步异常。

应保留原启动介质、串口控制台和恢复方式。首次测试不启动 CPU1，不加载 Guest，不初始化 PCI/ITS。

## 10. 已知限制与下一阶段

- 尚未提供适配 CBoot/UEFI/U-Boot 的实际加载配置；
- 尚未验证固件是否允许非安全 EL2 接管 GIC system registers；
- 尚未验证 AGX Orin DTB 中 HSUART/GIC/timer 节点的实际格式；
- 尚未处理 cache、SMMU、PCIe、设备隔离和 Guest 内存布局；
- 尚未在真实板卡观察到 UART 或 timer heartbeat。

下一阶段应面向用户实际启动链，选择 UEFI、U-Boot 或 NVIDIA 固件加载方式，生成对应加载命令和 DTB 获取流程，并以串口日志完成首次硬件验证。
