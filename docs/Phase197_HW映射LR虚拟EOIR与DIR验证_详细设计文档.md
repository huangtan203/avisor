# Phase 197：HW 映射 LR 的虚拟 IAR/EOIR/DIR 行为验证

## 1. 背景

Phase 196 只验证了软件 LR 上的 DIR 不匹配语义。Phase 194 第 7 节仍有一项未测：HW 映射 LR（LR0/LR2）在 VEOIM=1 下经虚拟 EOIR 与 DIR 后的状态。

HW 映射 LR 只由 hypervisor 注入（vtimer 注入 LR0，PCI INTx 注入 LR2），guest 无法直接触发，因此本阶段新增测试接口。

本阶段只验证虚拟侧 LR 状态。物理 GIC 的 DIR 行为不在本阶段范围内，guest 无法直接观测。

## 2. 实现

### 2.1 hypervisor 测试接口

- `include/common/hvc.h`：新增 `HVC_TEST_INJECT_HW_LR0 79`。
- `guests/el1_test/guest.h`：同步新增同一编号，供 guest 使用。
- `hypervisor/arch/aarch64/sync_exc.c`：新增 `case HVC_TEST_INJECT_HW_LR0`，调用 `vgic_inject_virq_hw(102, 1000)`。

`vgic_inject_virq_hw` 只写 LR0（`ICH_LR_HW=1`、pINTID 1000、pending），不修改物理 GIC 的使能或路由。pINTID 1000 为 SPI 范围内的编号，本平台无对应设备，不会被真实中断触发。

### 2.2 guest 探针

`guests/el1_test/main.c` 的 `probe_hw_lr_dir()`，在 `probe_dir_nomatch()` 之后调用：

1. `daifset #2` 屏蔽 IRQ。
2. HVC 79 注入 LR0，HVC 读取 LR0 得到 `LR0_before`。
3. `icc_iar1_read()` 得到 `IAR`。
4. HVC 读取 `LR0_after_IAR`。
5. `icc_eoir1_write(102)`，HVC 读取 `LR0_after_EOIR`。
6. `icc_dir_write(102)`，HVC 读取 `LR0_after_DIR`。
7. `daifclr #2` 恢复 IRQ。

## 3. 实测输出

构建：`make -B -f Makefile.virt -j2`，`build=0`。

QEMU 两轮（`timeout 35 qemu-system-aarch64 -M virt,virtualization=on,gic-version=3,highmem=off -cpu max -smp 2 -m 1G -device edu -kernel bin_virt/kernel8.img -display none -monitor none`，退出码 124，`PANIC=0`）：

```
[vm2] Probe: HW LR0 virtual IAR+EOIR+DIR (VEOIM=1)
[vm2]   LR0_before=700003e800000066 IAR=00000066 LR0_after_IAR=b00003e800000066 LR0_after_EOIR=b00003e800000066 LR0_after_DIR=b00003e800000066
[vm1] Probe: HW LR0 virtual IAR+EOIR+DIR (VEOIM=1)
[vm1]   LR0_before=700003e800000066 IAR=00000066 LR0_after_IAR=b00003e800000066 LR0_after_EOIR=b00003e800000066 LR0_after_DIR=b00003e800000066
```

两轮结果一致。

字段解读：
- `LR0_before = 0x700003e800000066`：bit63（ACTIVE）=0，bit62（PENDING）=1，bit61（HW）=1，pINTID=`0x3e8`（1000），vINTID=`0x66`（102）。即 pending。
- `IAR = 0x66`：返回 vINTID 102，说明 pending 中断被虚拟 ack。
- `LR0_after_IAR = 0xb00003e800000066`：bit63=1、bit62=0，即 active。
- `LR0_after_EOIR`、`LR0_after_DIR` 均与 `LR0_after_IAR` 相同。

## 4. 结论

- 虚拟 IAR 把 LR0 从 pending 转为 active（`LR0_before` → `LR0_after_IAR`）。
- 虚拟 EOIR 后 LR0 保持 active。这与 VEOIM=1 语义一致，EOIR 只降优先级。
- 虚拟 DIR 后 LR0 仍保持 active。这与代码一致：`vgic_handle_dir()`（`hypervisor/boards/virt/vgic.c`）遇到 `ICH_LR_HW` 置位的 LR 时直接跳过，注释说明 HW LR 的物理 DIR 由 `virt_leaving_vm` 负责。

## 5. 未验证的部分

- 物理侧 DIR 是否在 `virt_leaving_vm` 中发生，guest 无法观测，本阶段未验证（对应 Phase 194 第 7 节的物理侧项）。
- LR2（PCI INTx）的 HW 映射未测，本阶段只测 LR0。
- 本阶段 pINTID 1000 为人为选取，实际中断路径未覆盖。
- 只测了 QEMU 环境，未在 Orin 实机验证。

## 6. 代码位置

| 文件 | 内容 |
|---|---|
| `include/common/hvc.h` | `HVC_TEST_INJECT_HW_LR0` 定义 |
| `guests/el1_test/guest.h` | 同一编号的 guest 侧定义 |
| `hypervisor/arch/aarch64/sync_exc.c` | HVC 79 分支 |
| `hypervisor/boards/virt/vgic.c` | `vgic_handle_dir()`，跳过 HW LR 的逻辑 |
| `guests/el1_test/main.c` | `probe_hw_lr_dir()`，`main()` 调用处 |

## 7. 复现

```
make -B -f Makefile.virt -j2
timeout 35 qemu-system-aarch64 -M virt,virtualization=on,gic-version=3,highmem=off \
    -cpu max -smp 2 -m 1G -device edu -kernel bin_virt/kernel8.img \
    -display none -monitor none -serial file:/tmp/run.out
```

退出码 124 为正常超时。检查日志中 `HW LR0 virtual IAR+EOIR+DIR` 段落出现两次，且 `LR0_after_DIR` 为 `b00003e800000066`。
