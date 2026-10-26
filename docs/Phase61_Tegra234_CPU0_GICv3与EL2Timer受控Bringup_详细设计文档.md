# Phase 61：Tegra234 CPU0 GICv3 与 EL2 Timer 受控 Bring-up

## 1. 目标

Phase 60 建立了 GICv3、timer PPI 和 PSCI 的 DTB 能力模型，但 Tegra234 仍停留在 UART-only 模式。Phase 61 增加最小 CPU0 中断心跳路径：

- 发布 DTB GICD/GICR 运行时基址；
- 安装 EL2 exception vectors；
- 初始化 CPU0 GIC Distributor、Redistributor 和 CPU interface；
- 初始化 DTB 指定 PPI 的 EL2 physical timer；
- 开启 CPU0 IRQ，并在 WFE 循环中等待 timer heartbeat；
- 不初始化 scheduler、vGIC、ITS、PCI、Guest 或 CPU1。

## 2. 双重安全门控

Tegra 中断 bring-up 同时要求：

```text
interrupt_config_valid == true
runtime_mapping_valid == true
```

第一个条件确保 DTB 的 GICv3、GICR 和 timer PPI 信息完整；第二个条件确保相关高地址 MMIO 已实际进入当前 Stage-1 页表。

任一条件失败时只打印 `irq-config=unavailable`，然后进入 UART-only WFE，不访问 GIC MMIO。

## 3. GIC 运行时地址发布

GIC 驱动通过 `virt_gicd_base` 和 `virt_gicr_base` 访问硬件。此前这些变量只在 QEMU 完整 MMIO 校验成功时更新，Tegra 会错误保留 QEMU 默认地址。

Phase 61 将 GIC 地址发布条件改为 `interrupt_config_valid`：

```text
virt_gicd_base = VA_START + dtb.gicd_base
virt_gicr_base = VA_START + dtb.gicr_base
```

QEMU ITS 地址仍由 QEMU 完整 MMIO 配置路径发布；Tegra bring-up 不初始化 ITS。

## 4. 最小初始化顺序

Tegra bring-up-only 路径执行：

1. runtime Stage-1 映射发布；
2. Tegra HSUART 重新初始化；
3. 安装 `VBAR_EL2`；
4. `gic_init()` 初始化 CPU0；
5. `timer_init()` 配置 EL2 physical timer；
6. 打开 PSTATE IRQ；
7. 进入 `WFE` 循环。

不调用 `vgic_init()`，也不使能 Guest virtual timer PPI。

## 5. 无 Scheduler 的 Timer Handler

现有 `handle_timer_irq()` 在每次 tick 后调用 `timer_tick()`，但 Tegra bring-up-only 没有初始化 scheduler。

Phase 61 保留：

- 重新装载 `CNTHP_TVAL_EL2`；
- 每 CPU tick 计数；
- 每秒 UART heartbeat。

仅当 `platform_full_bringup_supported()` 为真时调用 `timer_tick()`。因此 QEMU 调度行为不变，Tegra 只执行安全心跳。

## 6. 当前风险边界

本阶段代码具备受控运行路径，但当前环境没有 AGX Orin 板卡或真实 DTB，以下内容没有硬件证据：

- GICD/GICR 地址是否与 boot firmware 暴露状态一致；
- CPU0 `GICR_TYPER` affinity 扫描是否成功；
- `ICC_SRE_EL2` 是否可写；
- DTB EL2 timer PPI 是否能在当前安全状态下路由到 EL2；
- Tegra HSUART 能否持续输出中断心跳。

如果 GIC 初始化中的 MMIO 或 affinity 假设不成立，真实板卡可能在首次 GIC 访问时异常或 PANIC。部署前必须准备可恢复启动介质和串口日志。

## 7. QEMU 回归

执行：

```text
make -f Makefile.virt clean
make -f Makefile.virt
timeout 100s make -f Makefile.virt run-log
```

结果：

```text
[platform] timer-ppi=26/27 psci=smc irq-config=valid
[platform] stage1-map=dtb banks=1 tables=4
[platform] page-pool=planned ranges=2 pages=257792
INFO[0]: GICv3 initialized.
[4.25] ITS initialized (GICv3 MSI controller)
[cpu1] GICv3 initialized
[vm1] virtio-net: ALL loopback tests PASSED!
[vm2] virtio-net: ALL loopback tests PASSED!
migration: source VM reclaimed (... done=1)
```

QEMU 未进入 Tegra 分支，完整回归通过，没有 PANIC 或 ASSERT。`run-log` timeout 返回 124 属于长驻任务预期行为。

## 8. 后续阶段

下一阶段应准备真实 AGX Orin 构建和加载产物：独立链接地址、固件入口约定、DTB 传递方式及串口验证清单。获得硬件心跳后，再考虑 PSCI CPU1 启动。
