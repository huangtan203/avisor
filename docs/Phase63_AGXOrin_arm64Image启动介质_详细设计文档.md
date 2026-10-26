# Phase 63：AGX Orin arm64 Image 启动介质

## 1. 目标

Phase 62 生成了链接到 `0x80080000` 的 Tegra raw image，但镜像起始位置直接是 aVisor 启动指令，没有标准 arm64 Linux Image header。U-Boot `booti` 和基于 extlinux 的启动流程通常依赖该头识别镜像布局。

Phase 63 为 Tegra 产物增加 64 字节 arm64 Image header、独立 `Image` 文件和构建期字段校验。QEMU 镜像保持原始入口布局不变。

本阶段支持的是 arm64 Image/`booti` 风格加载，不是可由 UEFI `LoadImage()` 直接执行的 PE/COFF EFI 应用。

## 2. Header 布局

Tegra `_start` 位于 `0x80080000`，前 64 字节布局为：

```text
0x00  跳转到真实入口的 AArch64 branch
0x04  保留字段
0x08  text_offset = 0x80000
0x10  image_size = image_payload_end - _start
0x18  flags = 0
0x20  res2 = 0
0x28  res3 = 0
0x30  res4 = 0
0x38  magic = 0x644d5241（内存字节为 ARMd）
0x3c  res5 = 0
```

第一条指令跳过完整 64 字节头，随后执行原有 EL 检查、DTB 参数保存和 MMU 初始化。

QEMU 构建未定义 `PLATFORM_TEGRA`，因此 `_start` 仍直接指向原始启动逻辑，不增加 header。

## 3. 镜像尺寸

链接脚本在已加载的代码、只读数据和初始化数据末尾定义：

```text
image_payload_end
```

header 中的 `image_size` 为：

```text
image_payload_end - _start
```

它描述文件有效载荷，不把运行时 BSS 和 128 页 Stage-1 table storage 计入磁盘镜像尺寸。运行时内存占用仍由 `image_end` 覆盖，DTB 内存规划会排除该范围。

## 4. 构建产物

执行：

```text
make -f Makefile.tegra clean
make -f Makefile.tegra verify-layout
```

生成：

```text
bin_tegra/Image
bin_tegra/kernel8.img
build_tegra/kernel8.elf
build_tegra/kernel8.list
```

`Image` 与 `kernel8.img` 内容相同。部署时应使用语义明确的 `bin_tegra/Image`，避免误选 QEMU 镜像。

## 5. 构建期校验

`verify-layout` 检查：

1. ELF `_start == 0x80080000`；
2. 文件至少包含完整 64 字节 header；
3. `text_offset == 0x80000`；
4. `image_size > 64`；
5. `image_size <= Image 文件大小`；
6. `flags == 0`；
7. `magic == 0x644d5241`。

任何字段不符都会使构建目标失败，不交付格式不完整的启动镜像。

验证输出：

```text
Tegra arm64 Image verified: entry=0x80080000 offset=0x80000 size=232788
```

镜像文件大小为 234024 字节。文件末尾可能包含由段对齐产生的 padding，因此 header `image_size` 小于等于文件大小是正常现象。

## 6. U-Boot 启动契约

预期启动方式为 `booti` 或内部最终调用 `booti` 的 extlinux 流程。加载器应：

1. 将 `Image` 加载到可用内存；
2. 加载 Tegra234 DTB；
3. 由 `booti <image_addr> - <dtb_addr>` 根据 Image header 完成布局处理；
4. 跳转时在 `x0` 传入 DTB 地址；
5. 以允许进入 EL2 的固件配置启动。

即使 header 声明 `text_offset=0x80000`，aVisor 最终仍要求运行地址为 `0x80080000`，因为当前镜像不是位置无关代码。启动加载器必须把入口安置到该链接地址。

## 7. 与 UEFI 的边界

标准 arm64 Image header 不等于 EFI application header。当前 `Image`：

- 可以作为 U-Boot `booti` 输入；
- 可能由 Jetson extlinux/U-Boot 兼容链加载；
- 不能作为 `BOOTAA64.EFI` 直接由 UEFI Boot Manager 执行；
- 不包含 PE/COFF header 或 EFI entry point；
- 不调用 EFI Boot Services。

如果目标设备只有原生 UEFI 启动且不提供 Linux Image 加载路径，后续需要增加 EFI stub/loader，而不是重命名当前文件。

## 8. 验证结果

### Tegra 静态验证

- 编译和链接成功；
- `_start = 0x80080000`；
- `ARMd` magic 位于 offset `0x38`；
- 首条 branch 跳过 64 字节 header；
- `Image` 与 `kernel8.img` 完全一致；
- IDE diagnostics 无错误。

### QEMU 回归

QEMU 干净构建及运行回归通过：

```text
[platform] stage1-map=dtb banks=1 tables=4
INFO[0]: GICv3 initialized.
[4.25] ITS initialized (GICv3 MSI controller)
[cpu1] GICv3 initialized
[vm1] virtio-net: ALL loopback tests PASSED!
[vm2] virtio-net: ALL loopback tests PASSED!
migration: source VM reclaimed (... done=1)
```

未发现 PANIC 或 ASSERT。

## 9. 下一阶段

下一阶段应根据实际 AGX Orin 启动环境二选一：

1. 若具备 U-Boot/extlinux：生成具体 extlinux stanza、加载地址和 DTB 路径，并进行串口首次启动；
2. 若只有 UEFI：实现最小 EFI loader 或 PE/COFF stub，由其加载 aVisor、获取 DTB/系统表并退出 Boot Services 后转入 EL2 入口。

在确认启动链类型前，不应声称当前 `Image` 可以直接从 Jetson UEFI 菜单执行。

## 10. Phase 64–65：extlinux 部署与 DTB 交付

Phase 64 在 `Makefile.tegra` 增加 `extlinux-package` 目标。Phase 65 进一步要求显式提供实际 DTB 文件，并对其 FDT magic 和 total size 做校验：

```sh
make -f Makefile.tegra extlinux-package \\
  TEGRA_DTB_SOURCE=/path/to/tegra234.dtb \\
  TEGRA_DTB_PATH=/boot/avisor/board.dtb
```

目标先运行 `verify-layout`，再生成：

```text
bin_tegra/deploy/boot/avisor/Image
bin_tegra/deploy/boot/avisor/board.dtb
bin_tegra/deploy/boot/extlinux/avisor.conf.fragment
```

配置片段仅包含 aVisor 的 `LABEL`、`LINUX` 和 `FDT` 项。应将其内容合并到现有 `extlinux.conf`，并保留原始 Jetson 启动项作为恢复入口。`TEGRA_DTB_PATH` 与 `TEGRA_IMAGE_PATH` 必须是目标系统中的绝对路径；`TEGRA_DTB_SOURCE` 必须指向存在且具有 `0xd00dfeed` magic 的 DTB 文件。

该目标是可审查的部署素材生成器，不会自动写入 SD/NVMe 分区，不会覆盖完整启动配置，也不证明当前固件实际支持 extlinux。DTB 文件名、分区挂载位置和 U-Boot 加载行为仍需在目标设备核实；没有 extlinux/U-Boot 启动路径的固件需要另行实现 EFI loader。

## 11. Phase 66：U-Boot 一次性启动脚本

`Makefile.tegra` 还提供 `uboot-package`，生成不修改环境变量的 U-Boot 命令脚本：

```sh
make -f Makefile.tegra uboot-package \\
  TEGRA_DTB_SOURCE=/path/to/tegra234.dtb \\
  TEGRA_UBOOT_INTERFACE=mmc \\
  TEGRA_UBOOT_DEVICE=0 \\
  TEGRA_UBOOT_PARTITION=1
```

默认生成：

```text
bin_tegra/deploy/boot/avisor/avisor-boot.cmd
```

内容等价于：

```text
fatload mmc 0:1 0x80000000 /boot/avisor/Image
fatload mmc 0:1 0x88000000 /boot/avisor/board.dtb
booti 0x80000000 - 0x88000000
```

该脚本只覆盖 `mmc`、`usb` 和 `nvme` 三类介质的命令模板；它不会自动执行 `saveenv`、不会写入启动分区，也不会替换恢复项。Image 加载地址 `0x80000000` 与 DTB 加载地址 `0x88000000` 是模板默认值，实际板卡必须确认没有覆盖固件、内核或其他保留内存。

## 12. Phase 67：U-Boot 加载地址安全校验

`uboot-package` 在生成命令前读取 arm64 Image header 中的 `text_offset` 和 `image_size`，计算实际占用区间：

```text
image_end = TEGRA_IMAGE_LOAD_ADDR + text_offset + image_size
```

构建期强制满足：

- `TEGRA_IMAGE_LOAD_ADDR == 0x80000000`，保证 `booti` 按 `text_offset=0x80000` 将入口放到链接地址 `0x80080000`；
- `TEGRA_DTB_LOAD_ADDR` 按 4KB 对齐；
- `TEGRA_DTB_LOAD_ADDR >= image_end`，避免 DTB 与 Image 加载、展开后的运行区间重叠。

不满足任一条件时，`uboot-package` 立即失败且不会生成新的启动脚本。该校验只覆盖 aVisor Image 与 DTB 之间的关系；目标板固件保留区、可信固件和其他动态分配仍需在实机启动前确认。

## 13. Phase 68：Tegra234 DTB 平台契约预检

新增 `tegra-dtb-check` 目标，对交付 DTB 执行 FDT header 边界检查，并要求 DTB 内容包含当前 CPU0 bring-up 所依赖的平台标记：

```sh
make -f Makefile.tegra tegra-dtb-check \\
  TEGRA_DTB_SOURCE=/path/to/tegra234.dtb
```

预检要求：

- FDT magic、版本、结构块和字符串块边界合法；
- 包含 `nvidia,tegra234`；
- 包含 `arm,gic-v3`；
- 包含 `arm,armv8-timer`；
- 包含 `nvidia,tegra194-hsuart`；
- 包含 PSCI 节点标记。

`extlinux-package` 和 `uboot-package` 都会自动依赖该预检。它只检查 DTB 的静态交付契约，不替代运行时对 `reg`、interrupt cells、GICR stride、PSCI method 和实际串口地址的解析；这些仍需使用真实 AGX Orin DTB 启动验证。

## 14. Phase 69：CPU0 bring-up DTB 审计日志

DTB 映射成功并启用 Tegra HSUART 后，bring-up-only 路径输出一组只读审计信息：

- CPU 数量；
- UART、GICD、GICR 基址和大小；
- GICR stride；
- hypervisor/virtual timer PPI；
- PSCI 方法；
- reserved-memory 数量；
- page-pool candidate 是否安全。

该日志不会启用 Guest、PCI、ITS、CPU1 或完整 scheduler 路径，也不会改变已有的 GIC/timer 门控。真实板卡首次启动时，应先保存这些审计行，再根据 `irq-config`、`runtime` 和 timer heartbeat 判断是否继续扩展硬件支持。

## 15. Phase 70：固件交接诊断

启动汇编现在记录 CPU0 进入入口时的异常级别，以及 DTB 参数来源：

```text
entry-el=2 source=x0
entry-el=3 source=x0
entry-el=2 source=x2
```

其中 `entry-el=3` 表示固件把 CPU0 交给 aVisor 时仍在 EL3，随后由入口代码降到 EL2；`entry-el=2` 表示直接从 EL2 进入。`source=x0` 是 AGX Orin 的正式 DTB 契约，`source=x2` 仅保留兼容诊断。DTB 无效时会显示 `source=none` 或 `fallback`，仍按现有平台安全路径处理，不会启用错误的 Tegra MMIO。

## 16. Phase 71：Tegra 交付前一键预检

新增：

```sh
make -f Makefile.tegra tegra-preflight \\
  TEGRA_DTB_SOURCE=/path/to/tegra234.dtb
```

该目标统一执行：

1. arm64 Image 入口和 header 校验；
2. Tegra234 DTB 静态契约校验；
3. Image/DTB U-Boot 加载地址冲突校验；
4. U-Boot `fatload`/`booti` 脚本生成和校验；
5. extlinux 配置片段生成和校验。

成功后输出 Image、DTB、入口地址和 U-Boot 介质摘要，并在 `bin_tegra/deploy` 下生成待审查素材。该目标不会复制到真实设备、修改 U-Boot 环境或覆盖恢复启动项。

## 17. Phase 72：部署清单与 SHA-256 校验

`tegra-preflight` 还生成：

```text
bin_tegra/deploy/boot/avisor/manifest.sha256
```

清单覆盖四个交付文件：

- `boot/avisor/Image`；
- `boot/avisor/board.dtb`；
- `boot/avisor/avisor-boot.cmd`；
- `boot/extlinux/avisor.conf.fragment`。

在部署目录中可执行：

```sh
cd bin_tegra/deploy
sha256sum -c boot/avisor/manifest.sha256
```

清单只用于交付审计，不是启动时安全验证，也不会自动复制到目标设备。

## 18. Phase 73：部署目录复核

新增：

```sh
make -f Makefile.tegra tegra-verify-deploy
```

该目标要求先存在 `manifest.sha256`，再检查四个清单条目、对应文件和全部 SHA-256 值。任何文件缺失、清单条目变化或内容被替换都会失败。它只读取 `bin_tegra/deploy`，不会访问真实设备，也不会修改部署目录。

## 19. Phase 74：自定义部署路径一致性

`TEGRA_IMAGE_PATH` 和 `TEGRA_DTB_PATH` 可以覆盖目标系统中的默认路径。Phase 74 让部署文件的相对路径、SHA-256 清单和 `tegra-verify-deploy` 使用同一组配置，不再硬编码默认路径；同时拒绝 Image、DTB、U-Boot 脚本或 extlinux 片段使用重复的部署路径。

## 20. Phase 75：部署路径遍历防护

新增 `tegra-path-check`，统一要求 Image 和 DTB 目标路径为绝对 POSIX 路径、互不相同且不包含 `..`。同时拒绝与生成的 U-Boot 脚本和 extlinux 片段路径冲突，避免自定义参数将文件写出 `bin_tegra/deploy` 或覆盖启动元数据。
