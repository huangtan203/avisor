# Phase 194：ICC_DIR_EL1 虚拟化与 DIR 语义验证

## 1. 背景

`ICH_VMCR_EL2.VEOIM = 1` 后，guest 写 `ICC_EOIR1_EL1` 只降低优先级、不去活中断，还需要额外写 `ICC_DIR_EL1` 完成去活。此前 guest 中断处理路径只写 EOIR，未写 DIR，软件 LR 停在 active 状态不释放。

本阶段补齐两件事：

1. 虚拟化 `ICC_DIR_EL1` 写：`HCR_EL2.IMO=1` 且 `ICH_HCR_EL2.TC=1` 时陷入 EL2，由 EL2 按 vINTID 清除对应的软件 LR。
2. 验证 DIR 语义本身：确认 VEOIM=1 下 EOIR 只降优先级、DIR 才去活，并区分两者对 LR 的影响。

此前缺少独立用例：既有探针在 guest handler 内连续执行 EOIR+DIR，只能看到合并结果，无法把「LR 被清」归因于 DIR。本阶段新增探针，让 handler 只做 EOIR，由测试代码在两步之间读取 LR。

## 2. 架构语义

| 操作 | VEOIM=1 时的效果 |
|---|---|
| 写 `ICC_EOIR1_EL1` | 只降低运行优先级（RPR），LR 保持 active |
| 写 `ICC_DIR_EL1` | 去活中断，释放对应的 LR |

guest 的 `ICC_DIR_EL1` 系统寄存器编码为 `S3_0_C12_C11_1`（op0=3, op1=0, CRn=12, CRm=11, op2=1）。

## 3. 实现

### 3.1 新增 VEOIM 位定义

`include/arch/aarch64/sysregs.h`：

```c
#define ICH_VMCR_VEOIM (1UL << 9)  /* ICC_CTLR_EL1.EOImode 的虚拟后端（Phase 194） */
```

### 3.2 使能 VEOIM

三处写 `ICH_VMCR_EL2` / 保存副本的位置同时置位：

- `hypervisor/boards/virt/vgic.c`：`vgic_init()` 的 VMCR 初始化。
- `hypervisor/boards/virt/virt_board.c`：`virt_initialize()` 与 `virt_attach_secondary()` 的 `tsk->virt.gic_vmcr`。

此前 `ICC_CTLR_EL1` 读路径只反映 CBPR，VEOIM 置位后必须一并反映，否则 guest 读回的 `EOImode` 与后端不一致。

### 3.3 DIR 的 TC trap 分发

`hypervisor/arch/aarch64/sync_exc.c`，在 `ICC_SGI1R_EL1` 分支之后：

```c
/*
 * Phase 194：ICC_DIR_EL1 写（TC=1 trap）。VEOIM=1 下 guest 用
 * DIR 去活中断，由 EL2 按 vINTID 清除对应软件 LR。
 */
if (op0 == 3 && op1 == 0 && crn == 12 && crm == 11 && op2 == 1) {
    vgic_handle_dir((uint32_t)(regs->regs[rt] & 0x3ff));
    goto sys_fin;
}
```

### 3.4 ICC_CTLR_EL1 读路径补 VEOIM

同一文件，CTLR 读分支在原有 `ro_mask`、`vcbpr` 基础上追加 `veoim`：

```c
unsigned long veoim = (vmcr & ICH_VMCR_VEOIM) ? 2UL : 0UL;
regs->regs[rt] = (phys & ro_mask) | vcbpr | veoim;
```

`EOImode` 位于 `ICC_CTLR_EL1` bit 1，故取值为 2。

### 3.5 vgic_handle_dir

`hypervisor/boards/virt/vgic.c`：

```c
void vgic_handle_dir(uint32_t vintid)
{
    for (int i = 1; i < NR_VGIC_LRS - 1; i++) {
        uint64_t lr = read_lr(i);
        if ((lr & ICH_LR_STATE_MASK) != ICH_LR_ACTIVE)
            continue;
        if ((lr & ICH_LR_HW) != 0)
            continue;
        if ((uint32_t)(lr & 0xffffffffULL) == vintid) {
            write_lr(i, 0);
            asm volatile("isb");
            return;
        }
    }
}
```

范围说明：

- 只扫描软件 LR（`LR1..NR-2`）。`LR0` 保留给虚拟定时器、`LR2` 用于 PCI 直通，均为 HW 映射。
- HW 映射条目的物理去活由 `virt_leaving_vm` 负责，此处跳过。
- 只匹配 active 且非 HW 的条目，pending 条目不受 DIR 影响。

## 4. guest 侧用例

`guests/el1_test/main.c` 新增 `probe_dir_semantics()` 与专用 vINTID 101：

- handler 对 INTID 101 只执行 `icc_eoir1_write()` 后直接返回，跳过末尾的 `icc_dir_write()`。
- 测试流程：注入 101 → 等待 `p194_eoir_done` → `HVC_VGIC_LR_DUMP` 读 LR1 → 显式 `icc_dir_write(101)` → 再读 LR1。
- 判据：EOIR 后 LR1 状态位（bit 63:62）为 ACTIVE（2），DIR 后 LR1 清零。

两次读取之间没有任何其它路径会清除 LR，因此 LR 清零可归因于 DIR。

顺带修正 Phase 192 探针的注释：vINTID 100 走 guest 分派的兜底分支，实际执行 EOIR+DIR，原文「仅 EOIR」不准确。

## 5. 实测输出

QEMU（`-M virt,virtualization=on,gic-version=3 -smp 2`）：

```
[vm2] Probe: ICC_DIR_EL1 semantics (VEOIM=1)
[vm2]   inject_ret=0 eoir_done=1 LR1_after_EOIR=90a0000000000065 LR1_after_DIR=0000000000000000
[vm2]   PASSED: EOIR 仅降优先级（LR 保持 active），DIR 去活并清除 LR
[vm1]   inject_ret=0 eoir_done=1 LR1_after_EOIR=90a0000000000065 LR1_after_DIR=0000000000000000
[vm1]   PASSED: EOIR 仅降优先级（LR 保持 active），DIR 去活并清除 LR
```

`LR1_after_EOIR` 解码：

- bit 63:62 = `10`：ACTIVE。
- bit 61 = 1：HW 映射。
- 低 32 位 = `0x65` = 101：vINTID 正确。

`LR1_after_DIR = 0`：DIR 后 LR 被清空。

同一轮回归同时确认：

- `ICC_CTLR_EL1` 只读探针 `guest_view=0x8f02 phys_el2=0x8f02`，`readonly bits match: yes`。`0x8f02` 的 bit 1 即 VEOIM 路径写入的 EOImode。
- `PANIC=0`、`FAIL=0`、`WARN=21`。
- Test 18（冻结握手）、Test 26/27（zombie 回收 / 槽位复用）、Phase 191 pending ring 均通过。

## 6. 结论

- VEOIM=1 下，`ICC_EOIR1_EL1` 只降优先级，LR 保持 active；`ICC_DIR_EL1` 才去活并释放 LR。语义与架构一致。
- `ICC_DIR_EL1` 的 TC trap 分发、`vgic_handle_dir` 的 vINTID 匹配与 LR 清除均经实测验证。
- 此前「HW LR 的 DIR 语义未单独验证」的遗留项关闭。

## 7. 未验证的部分

- 只测了软件 LR（LR1）。HW 映射 LR（LR0 虚拟定时器、LR2 PCI 直通）的物理 DIR 由 `virt_leaving_vm` 承担，未在本阶段单独构造用例验证。
- 只测了 vINTID 101 单条 active 条目。多条同时 active 时的遍历顺序未测。
- 未测 guest 对不存在 active 条目的 vINTID 写 DIR（应为无操作）；`vgic_handle_dir` 的实现自然满足，但未加显式断言。
- QEMU 环境的虚拟 GIC 行为可能与 Orin 实机存在差异，实机未验证。

## 8. 代码位置

| 文件 | 内容 |
|---|---|
| `include/arch/aarch64/sysregs.h` | `ICH_VMCR_VEOIM` 定义 |
| `include/boards/virt/vgic.h` | `vgic_handle_dir()` 声明 |
| `hypervisor/boards/virt/vgic.c` | `vgic_handle_dir()` 实现、`vgic_init()` 置 VEOIM |
| `hypervisor/boards/virt/virt_board.c` | `virt_initialize()` / `virt_attach_secondary()` 置 VEOIM |
| `hypervisor/arch/aarch64/sync_exc.c` | DIR trap 分发、CTLR 读路径补 VEOIM |
| `guests/el1_test/guest.h` | `icc_dir_write()` 宏 |
| `guests/el1_test/main.c` | handler DIR 调用、`probe_dir_semantics()`、vINTID 101 分支 |

## 9. 复现

```
make -B -f Makefile.virt -j2
timeout 35 qemu-system-aarch64 -M virt,virtualization=on,gic-version=3,highmem=off \
    -cpu max -smp 2 -m 1G -device edu -kernel bin_virt/kernel8.img \
    -display none -monitor none -serial file:/tmp/run.out
```

退出码 124 为正常超时。检查 `Probe: ICC_DIR_EL1 semantics` 段落出现两次 PASSED。
