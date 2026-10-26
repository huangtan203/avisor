# Phase 58：Tegra234 Early UART 与平台资源适配

## 1. 阶段目标

本阶段在 Phase 57 多 DRAM bank 和跨 L0 Stage-1 页表基础上，开始处理 NVIDIA Tegra234 / Jetson AGX Orin 的真实启动资源差异：

- 识别 Tegra234 DTB 中的 UART 节点；
- 区分 QEMU PL011 和 Tegra HSUART；
- 解析 UART `reg`、`reg-shift`、`clock-frequency`、`current-speed`；
- 为 Tegra HSUART 增加 8250 风格轮询收发和基本波特率初始化；
- QEMU 继续使用原 PL011 路径；
- AGX Orin 在资源尚未完成 Stage-1 发布时安全降级，不误访问固定 QEMU UART。

本阶段不宣称已经可以在真实 AGX Orin 上完成完整启动。

## 2. UART 类型模型

`platform_info` 增加 `platform_uart_kind`：

- `PLATFORM_UART_PL011`：QEMU virt 当前使用；
- `PLATFORM_UART_TEGRA_HSUART`：匹配 `nvidia,tegra194-hsuart`；
- `PLATFORM_UART_UNKNOWN`：不支持的 UART 类型。

同时保存：

```text
uart_reg_shift
uart_clock_hz
uart_baudrate
uart_compatible
```

DTB 节点名同时兼容 `pl011`、`serial`、`uart` 和 `hsuart`，但最终类型以 `compatible` 属性为准。

## 3. Tegra HSUART 后端

Tegra HSUART 使用 8250 风格寄存器，而不是 PL011 的 `UARTFR/UARTCR` 布局。驱动根据 UART 类型分派：

- 发送：等待 `LSR.THRE`，写 `THR`；
- 接收：等待 `LSR.DR`，读 `RBR/THR`；
- 状态查询：读取 `LSR.DR`；
- 初始化：关闭中断，设置 DLAB 和除数，配置 8N1/FIFO。

QEMU 的 PL011 初始化、收发和中断处理保持原路径不变。

## 4. 启动安全边界

当前启动汇编在 DTB 解析前使用固定 QEMU bootstrap 页表和固定 early UART。真实 Tegra 高地址 UART 不能在此阶段直接访问。

因此：

1. 初始 `uart_init()` 仍使用 bootstrap 的 QEMU 兼容路径；
2. DTB 解析后识别 Tegra HSUART；
3. 如果 runtime Stage-1 映射尚未发布，Tegra UART 后端保持静默；
4. 不向 Tegra 物理地址发起未经映射的 MMIO 访问；
5. QEMU runtime mapping 成功后继续使用 PL011。

这避免了把真实板卡地址误当作 QEMU PL011 地址，也避免早期地址转换未准备好时触发异常。

## 5. 当前限制

- `platform_runtime_config_valid()` 仍只允许 QEMU virt 的完整 GICv3/ITS/timer 配置；
- Tegra HSUART 尚未获得独立的真实板卡 Stage-1 发布路径；
- GICD/GICR/ITS/PSCI/SMMU 仍未完成 Tegra234 适配；
- Tegra UART 中断尚未接入平台化 GIC IRQ 路由，当前仅实现轮询后端；
- 当前启动汇编仍是 QEMU virt 专用入口，尚未建立 Orin 固件/启动链入口。

因此 Phase 58 的结果是“资源识别和驱动边界准备”，不是 AGX Orin 可部署版本。

## 6. 验证

构建：

```text
make -f Makefile.virt clean
make -f Makefile.virt
```

QEMU 回归：

```text
[platform] memory-config=valid mmio-config=valid runtime=enabled
[platform] stage1-map=dtb banks=1 tables=4
INFO[0]: GICv3 initialized.
[4.25] ITS initialized (GICv3 MSI controller)
[cpu1] GICv3 initialized
[vm1] virtio-net: ALL loopback tests PASSED!
[vm2] virtio-net: ALL loopback tests PASSED!
migration: source VM reclaimed (... done=1)
```

`run-log` 是长驻目标，使用 `timeout` 后返回 124 属于预期行为。日志未出现 PANIC 或 ASSERT。

## 7. 后续阶段

下一阶段应建立 Tegra234 独立 runtime mapping 策略，允许 DTB 提供的 UART/GIC/DRAM 资源在高地址下发布，并加入真实 GICv3 中断号、PSCI 启动和 UART IRQ 支持。
