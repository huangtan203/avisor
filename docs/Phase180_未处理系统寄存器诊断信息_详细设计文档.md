# Phase 180：未处理系统寄存器访问的诊断信息补全 详细设计文档

## 1. 问题

`handle_trap_system()` 在遇到未处理的系统寄存器访问时，只打印：

```
WARN("system register access is not handled.");
```

这条日志不含任何寄存器编码。回归日志中出现该告警时（且与 guest 串口输出交织，形如 `[vm2] =W1ARN[2]: ...`），无法判断 guest 访问的是哪个寄存器，也就无法判断这是合法访问遗漏了模拟，还是非法访问。

这是可观测性缺陷：故障已发生，但信息不足以定位。

## 2. 修复

把 ESR_EL2 中已解析出的编码字段打印出来：

```c
WARN("system register access is not handled: op0=%u op1=%u crn=%u crm=%u op2=%u dir=%u rt=%u",
     op0, op1, crn, crm, op2, dir, rt);
```

这些变量在函数开头已从 `esr` 解析，无需新增计算。`dir=0` 表示 MSR（写），`dir=1` 表示 MRS（读）。

## 3. 定位结果

补全后回归日志给出唯一编码：

```
system register access is not handled: op0=3 op1=0 crn=4 crm=6 op2=0 dir=0 rt=0
```

即 `S3_0_C4_C6_0` 的写入，对应 **`ICC_PMR_EL1`**（CPU 接口优先级屏蔽寄存器）。与 `include/boards/virt/gicv3.h` 中的定义 `#define ICC_PMR_EL1 S3_0_C4_C6_0` 一致。

guest 侧在 `guests/el1_test/guest.h` 中以 `SYSREG_WRITE(S3_0_C4_C6_0, v)` 写该寄存器，用于初始化中断优先级掩码。

## 4. 为什么本阶段不改行为

`ICC_PMR_EL1` 属于 GICv3 虚拟 CPU 接口的优先级控制。按项目现有设计（`include/boards/virt/vgic.h` 的说明），`ICC_*_EL1` 由硬件自动虚拟化，`ICH_HCR_EL2` 只置 `TC` 位捕获 `ICC_SGI1R_EL1`。PMR 被 trap 说明这条路径与既有假设不一致，属于需要单独评估的语义问题：

- 若改为写入 `ICH_VMCR_EL2.VPMR`，会改变虚拟中断的优先级过滤行为，可能影响 vITS/edu MSI 的投递。
- 若简单忽略（RAZ/WI），则 guest 的优先级屏蔽设置静默失效。

两种做法都涉及中断语义，不应与诊断增强混在同一个提交里。本阶段只交付可观测性改进，把语义决策留给独立阶段。

## 5. 验证

- 构建 `make -B -f Makefile.virt -j2` 通过（exit 0）。
- 35 秒 QEMU 回归（`/tmp/phaseX.log`）：
  - `EL2 task stack overflow`：0
  - `shm: PASSED`：2，`shm: FAILED`：0
  - `Phase 19: SOME tests FAILED`：0
  - `vITS: FAILED`：0
  - `vITS: PASSED` 与 `share vSPI 72, both paths`：均出现
  - 新告警按预期给出完整编码，行为与修改前一致（仍为不处理）

## 6. 后续

`ICC_PMR_EL1` 的处理方式（映射到 ICH_VMCR.VPMR，或明确忽略）需独立阶段评估，本阶段不涉及。
