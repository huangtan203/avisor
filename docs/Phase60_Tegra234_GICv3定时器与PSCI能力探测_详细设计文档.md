# Phase 60：Tegra234 GICv3、定时器与 PSCI 能力探测

## 1. 目标

Phase 59 已能为 Tegra234 发布高地址 Stage-1 映射，但中断和 SMP 路径仍存在 QEMU 硬编码：

- EL2 物理 timer PPI 固定为 26；
- Guest 虚拟 timer PPI 固定为 27；
- CPU_ON 固定在主函数中直接执行 SMC；
- 没有统一判断 DTB 的 GIC、timer 和 PSCI 能力是否完整。

Phase 60 将这些信息收敛到平台层，为后续真实 Tegra GIC 初始化和次核启动建立安全边界。

## 2. PSCI 能力模型

新增 `platform_psci_method`：

- `PLATFORM_PSCI_SMC`；
- `PLATFORM_PSCI_HVC`；
- `PLATFORM_PSCI_UNKNOWN`。

DTB parser 识别 `/psci` 节点、`arm,psci*` compatible 和 `method` 属性。

CPU_ON 迁移到平台接口：

```c
platform_psci_cpu_on(mpidr, entry_point, context_id)
```

当前 EL2 实现只允许 SMC。DTB 声明 HVC 时会返回失败，因为 Hypervisor 自身运行在 EL2，不能直接复用 EL1 PSCI HVC 客户端路径。

QEMU 无效 DTB fallback 继续默认使用已经验证的 SMC 固件路径。平台一旦识别为 Tegra234，就清除 QEMU 默认 PSCI 状态，并以 Tegra DTB 为准。

## 3. GICv3 与 Timer 能力验证

新增 `interrupt_config_valid`，要求：

- GIC 节点存在且 compatible 为 `arm,gic-v3`；
- GIC Distributor 和 Redistributor 范围已解析；
- Redistributor stride 不小于 128KB；
- timer 节点 compatible 为 `arm,armv8-timer`；
- EL2 物理 timer 和 EL1 virtual timer 均解析为有效 PPI 16–31。

DTB 没有 `redistributor-stride` 时，使用 GICv3 标准最小 frame 间距 `0x20000`。这仅用于 frame 扫描；实际 CPU affinity 仍通过每个 `GICR_TYPER` 匹配。

## 4. 移除 Timer PPI 硬编码

以下路径改用平台 getter：

- `timer_init()` 通过 `platform_timer_hyp_ppi()` 使能 EL2 timer PPI；
- IRQ dispatcher 动态匹配 hypervisor/virtual timer PPI；
- vGIC 初始化通过 `platform_timer_virt_ppi()` 使能 Guest virtual timer 物理 PPI；
- 虚拟 timer HW LR 使用实际 DTB PPI 作为 vINTID/pINTID。

QEMU DTB 仍得到 26/27，因此现有 Guest ABI 和回归行为不变。

## 5. Bring-up 边界

Phase 60 只建立能力模型，并没有解除 Phase 59 的 Tegra bring-up-only 门控。原因包括：

- 未在真实 Tegra234 上验证 GICD/GICR 系统寄存器接口；
- Tegra CPU MPIDR 到 Redistributor frame 的真实匹配尚无运行证据；
- PSCI SMC CPU_ON 入口地址、固件 EL 状态和上下文约定尚未验证；
- UART IRQ、SMMU 和真实 Guest 内存布局尚未完成。

因此 Tegra 即使显示 `irq-config=valid` 和 `psci=smc`，当前也只表示 DTB 能力满足下一阶段试运行条件。

## 6. 验证

执行干净构建和 QEMU 回归：

```text
make -f Makefile.virt clean
make -f Makefile.virt
timeout 100s make -f Makefile.virt run-log
```

关键日志：

```text
[platform] gicr-size=... stride=... timer-ppi=26/27 psci=smc irq-config=valid
[platform] stage1-map=dtb banks=1 tables=4
INFO[0]: GICv3 initialized.
[4.25] ITS initialized (GICv3 MSI controller)
[9] SMP: PSCI CPU_ON(cpu1) returned 0
[cpu1] GICv3 initialized
[vm1] virtio-net: ALL loopback tests PASSED!
[vm2] virtio-net: ALL loopback tests PASSED!
migration: source VM reclaimed (... done=1)
```

没有 PANIC 或 ASSERT。`run-log` 为长驻任务，timeout 返回 124 属于预期行为。

构建仍报告 `platform.c` 中已有的 freestanding `strlen` 隐式声明警告，本阶段未改变该工具函数声明问题。

## 7. 后续阶段

下一阶段应增加 Tegra bring-up 的可选 GIC/timer 初始化模式：仅在 `interrupt_config_valid` 时初始化 CPU0 GIC 和 EL2 timer，验证中断心跳；在此通过之前仍不创建 Guest，也不启动 CPU1。
