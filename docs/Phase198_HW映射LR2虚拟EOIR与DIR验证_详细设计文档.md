# Phase 198：HW 映射 LR2 的虚拟 IAR/EOIR/DIR 验证

## 1. 背景

Phase 197 验证了 HW 映射 LR0（vtimer 路径）的虚拟 IAR/EOIR/DIR 行为。LR2 对应 PCI INTx 路径，注入函数为 `vgic_inject_virq_hw_eoi()`，与 LR0 的 `vgic_inject_virq_hw()` 在写入的 LR 编号和 EOI 处理上不同。本阶段补测 LR2。

本阶段只验证虚拟侧 LR 状态，物理 GIC 的 DIR 行为不在范围内。

## 2. 实现

### 2.1 hypervisor 测试接口

- `include/common/hvc.h`：新增 `HVC_TEST_INJECT_HW_LR2 80`。
- `guests/el1_test/guest.h`：同一编号的 guest 侧定义。
- `hypervisor/arch/aarch64/sync_exc.c`：新增 `case HVC_TEST_INJECT_HW_LR2`，调用 `vgic_inject_virq_hw_eoi(102, 1000)`，写入 LR2。

HVC 80 没有修改 HVC 79，Phase 197 的接口保持不变。

### 2.2 guest 探针

`guests/el1_test/main.c` 的 `probe_hw_lr2_dir()`，在 `probe_hw_lr_dir()` 之后调用。流程与 Phase 197 相同：屏蔽 IRQ，注入（HVC 80），读 LR2，IAR，EOIR，DIR，每步读回 LR2（`HVC_VGIC_LR_DUMP` 的 x2），恢复 IRQ。

## 3. 实测输出

构建：`make -B -f Makefile.virt -j2`，`build=0`。

QEMU 两轮（退出码 124，`PANIC=0`）：

```
[vm2] Probe: HW LR2 virtual IAR+EOIR+DIR (VEOIM=1)
[vm2]   LR2_before=700003e800000066 IAR=00000066 LR2_after_IAR=b00003e800000066 LR2_after_EOIR=b00003e800000066 LR2_after_DIR=b00003e800000066
[vm1] Probe: HW LR2 virtual IAR+EOIR+DIR (VEOIM=1)
[vm1]   LR2_before=700003e800000066 IAR=00000066 LR2_after_IAR=b00003e800000066 LR2_after_EOIR=b00003e800000066 LR2_after_DIR=b00003e800000066
```

字段解读与 Phase 197 相同：注入后 `bit62=1`（pending）、`bit61=1`（HW）、pINTID=`0x3e8`、vINTID=`0x66`；IAR 返回 102；之后 `bit63=1`（active）。

## 4. 结论

- 虚拟 IAR 把 LR2 从 pending 转为 active。
- 虚拟 EOIR 后 LR2 保持 active，符合 VEOIM=1（EOIR 只降优先级）。
- 虚拟 DIR 后 LR2 仍为 active。`vgic_handle_dir()` 跳过 `ICH_LR_HW` 置位的 LR，代码与实测一致。
- LR0 与 LR2 结果一致：两条 HW 映射路径在虚拟侧行为相同。

## 5. 未验证的部分

- 物理侧 DIR 是否在 `virt_leaving_vm` 中发生，guest 无法观测，未验证。
- 本阶段 pINTID 1000 为人为选取，不是真实 PCI INTx 中断，未覆盖真实设备路径。
- 只测 QEMU 环境，未在 Orin 实机验证。
- LR2 的虚拟 DIR 后保持 active 这一结论，本身是否符合预期语义（即 HW LR 是否应由虚拟 DIR 去活）未作论证，只说明与代码一致。

## 6. 代码位置

| 文件 | 内容 |
|---|---|
| `include/common/hvc.h` | `HVC_TEST_INJECT_HW_LR2` 定义 |
| `guests/el1_test/guest.h` | 同一编号的 guest 侧定义 |
| `hypervisor/arch/aarch64/sync_exc.c` | HVC 80 分支 |
| `hypervisor/boards/virt/vgic.c` | `vgic_handle_dir()`，跳过 HW LR 的逻辑 |
| `guests/el1_test/main.c` | `probe_hw_lr2_dir()`，`main()` 调用处 |

## 7. 复现

```
make -B -f Makefile.virt -j2
timeout 35 qemu-system-aarch64 -M virt,virtualization=on,gic-version=3,highmem=off \
    -cpu max -smp 2 -m 1G -device edu -kernel bin_virt/kernel8.img \
    -display none -monitor none -serial file:/tmp/run.out
```

退出码 124 为正常超时。检查日志中 `HW LR2 virtual IAR+EOIR+DIR` 段落出现两次。
