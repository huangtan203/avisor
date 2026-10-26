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
