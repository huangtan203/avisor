# Phase 17：虚拟 SGI 陷阱模拟（trap-and-emulate） 详细设计文档

## 1. 概述

### 1.1 背景

在 GICv3 中，Guest 发送软件生成中断（SGI）的标准方式是直接写 CPU Interface 系统寄存器 `ICC_SGI1R_EL1`（亲和路由模式 `ARE_NS=1`）。写该寄存器时，硬件会根据编码中的 INTID、TargetList、IRM、Aff0..Aff3 字段自动将 SGI 路由到目标 PE。

但本项目的 Guest 运行在 EL1、Hypervisor 运行在 EL2。此前（Phase 12 起）Guest 发送虚拟 SGI 走的是**半虚拟化**路径——通过 `HVC_VSGI_SEND` hypercall 由 Hypervisor 调用 `vgic_inject_virq_to_vm` 注入。这要求 Guest 的 SGI 发送代码显式调用 HVC，与标准 GICv3 编程模型不一致。

本 Phase（方向 1 第 2 条）将这一路径改为 **trap-and-emulate**：利用 `ICH_HCR_EL2.TC`（Trap Control）位，使 Guest 写 `ICC_SGI1R_EL1` 时陷入 EL2，由 Hypervisor 解析写入值并翻译成 vSGI 注入。这样 Guest 的 GICv3 SGI 发送代码**无需任何 HVC 半虚拟化**即可工作。

### 1.2 目标

- ✅ 置 `ICH_HCR_EL2.TC=1`，使 EL1 访问 `ICC_SGI0R_EL1/ICC_SGI1R_EL1/ICC_ASGI1R_EL1` trap 到 EL2
- ✅ 在 `handle_trap_system()`（EC=0x18）中识别 `ICC_SGI1R_EL1`（`S3_0_C12_C11_5`）的写 trap 并分派
- ✅ 新增 `handle_vgic_sgi1r()`，把标准 `ICC_SGI1R_EL1` 写翻译成 vSGI 注入（定向 + IRM 广播）
- ✅ Guest 侧新增标准 SGI 发送 helper `icc_sgi1r_write()` 与 Phase 17 自检，全量回归通过

### 1.3 平台与基础

- **硬件平台**：QEMU `virt` 机器，ARMv8-A，GICv3 + ITS + vGIC，2 物理核
- **Hypervisor**：aVisor（Type-1，EL2）
- **Guest**：EL1 测试程序（裸金属）
- **前序版本**：`v0.16.0-phase16-vgicr`

---

## 2. 整体架构设计

### 2.1 数据流

```
Guest 写 ICC_SGI1R_EL1（icc_sgi1r_write）
        │  ICH_HCR_EL2.TC=1 → trap 到 EL2（EC=0x18, ESR 解析）
        ▼
sync_exc.c: handle_trap_system()
        │  匹配 op0=3 op1=0 crn=12 crm=11 op2=5（ICC_SGI1R_EL1）
        ▼
handle_vgic_sgi1r(regs->regs[rt])   ← vgic.c
        │  解析 INTID / IRM / Aff1/2/3 / TargetList
        │  翻译成 virt_inject_virq_to_vcpu(vmid, vcpu_id, sgi_id)
        ▼
vgic_inject_virq_to_vm（跨核/同核/saved-state 三路分派，Phase 12 已有）
```

### 2.2 与半虚拟化路径的对比

| 维度 | Phase 12（HVC_VSGI_SEND） | Phase 17（trap-and-emulate） |
|------|---------------------------|------------------------------|
| Guest 发送方式 | `hvc_call(HVC_VSGI_SEND, ...)` | 直接写 `ICC_SGI1R_EL1` |
| 编程模型 | 半虚拟化（非标准） | 标准 GICv3 |
| 触发方式 | HVC 异常（EC=0x16） | sysreg trap（EC=0x18） |
| Hypervisor 入口 | `handle_hvc64` | `handle_trap_system` |
| 注入后端 | `vgic_inject_virq_to_vm` | `virt_inject_virq_to_vcpu`（同后端） |

本 Phase **不删除** `HVC_VSGI_SEND`，二者并存；新测试走 trap 路径，旧 Phase 12 测试仍走 HVC 路径，保证向后兼容。

---

## 3. ICC_SGI1R_EL1 编码

`ICC_SGI1R_EL1` 在亲和路由（ARE_NS=1）模式下的字段编码（与本项目 `gic_send_sgi` 一致）：

| 位段 | 字段 | 含义 |
|------|------|------|
| [55:48] | Aff3 | 目标亲和层级 3 |
| [40] | IRM | 1=广播给除自身外的所有 PE |
| [39:32] | Aff2 | 目标亲和层级 2 |
| [27:24] | INTID | SGI 号（0..15） |
| [23:16] | Aff1 | 目标亲和层级 1 |
| [15:0] | TargetList | bit n 对应 Aff0=n 的 PE |

本项目为**单 cluster**（所有 vCPU 的 Aff1/Aff2/Aff3 均为 0），仅 Aff0 区分 vCPU。因此翻译规则简化为：

- 若 `Aff1 || Aff2 || Aff3` 非 0：目标不在本 VM，直接忽略；
- 否则把 `TargetList` 位图翻译成 `vcpu_id = Aff0 = bit 位号`，逐位注入。

---

## 4. trap 使能：ICH_HCR_EL2.TC

`ICH_HCR_EL2`（Interrupt Controller Hyp Control Register）控制虚拟 CPU 接口行为。`TC`（Trap Control，bit 10）置 1 后，EL1 访问 `ICC_SGI0R_EL1`、`ICC_SGI1R_EL1`、`ICC_ASGI1R_EL1` 会 trap 到 EL2（EL2 自身访问不受影响）。

在 `sysregs.h` 中新增宏：

```c
/* ICH_HCR_EL2 位 */
#define ICH_HCR_EL2_EN (1 << 0)      /* 使能虚拟 CPU 接口 */
#define ICH_HCR_EL2_TC (1UL << 10)   /* Trap Control：使 EL1 访问 ICC_SGI*R_EL1 trap 到 EL2 */
```

`vgic_init()` 在原有 `ICH_HCR_EL2_EN` 基础上叠加 `TC`：

```c
WRITE_SYSREG64(ICH_HCR_EL2_EN | ICH_HCR_EL2_TC, ICH_HCR_EL2);
asm volatile("isb");
```

`TC` 只影响 EL1 的 SGI 寄存器访问，vtimer PPI 27 的 HW 直通等其他路径不受影响。

---

## 5. sysreg trap 识别与分派

`ICC_SGI1R_EL1` 的系统寄存器编码为 `S3_0_C12_C11_5`（`op0=3, op1=0, crn=12, crm=11, op2=5`）。Guest 写该寄存器陷入 EL2 时，异常类 `EC=0x18`（`ESR_EL2_EC_TRAP_SYSTEM`），`handle_trap_system()` 从 ESR 解析出 op0/op1/crn/crm/op2/rt/dir。

在 `handle_trap_system()` 的写方向（`(op0 & 2) && dir == 0`）分支开头新增识别：

```c
if ((op0 & 2) && dir == 0) {
#ifdef PLATFORM_VIRT
    if (op0 == 3 && op1 == 0 && crn == 12 && crm == 11 && op2 == 5) {
        handle_vgic_sgi1r(regs->regs[rt]);
        goto sys_fin;
    }
#endif
    DEFINE_SYSREG_MSR(actlr_el1, 0, 1, 0, 1);
    DEFINE_SYSREG_MSR(csselr_el1, 1, 0, 0, 0);
} else if ((op0 & 2) && dir == 1) {
    ...
}
```

`rt` 为目标通用寄存器索引，`regs->regs[rt]` 即 Guest 写入 `ICC_SGI1R_EL1` 的原始值。

---

## 6. 翻译实现：handle_vgic_sgi1r

新增于 `vgic.c`（`vgic_ipi_update_handle` 之后）：

```c
void handle_vgic_sgi1r(uint64_t val)
{
    uint32_t sgi_id = (uint32_t)((val >> 24) & 0xF);   /* INTID [27:24] */
    int irm        = (int)((val >> 40) & 1);           /* IRM [40] */
    uint32_t aff1  = (uint32_t)((val >> 16) & 0xFF);   /* Aff1 [23:16] */
    uint32_t aff2  = (uint32_t)((val >> 32) & 0xFF);   /* Aff2 [39:32] */
    uint32_t aff3  = (uint32_t)((val >> 48) & 0xFF);   /* Aff3 [55:48] */
    uint32_t target_list = (uint32_t)(val & 0xFFFF);   /* TargetList [15:0] */

    int vmid = (int)current->vmid;
    int self_vcpu = (int)current->vcpu_id;

    /* 单 cluster 简化：目标亲和必须是 0 才算本 VM 的 vCPU */
    if (aff1 || aff2 || aff3)
        return;

    if (irm) {
        /* 广播：给本 VM 所有 vCPU，排除自身 */
        for (int v = 0; v < MAX_VCPUS_PER_VM; v++) {
            if (v == self_vcpu)
                continue;
            virt_inject_virq_to_vcpu(vmid, v, sgi_id);
        }
        return;
    }

    /* 定向：遍历 TargetList 位图，bit n -> vcpu_id = n (Aff0) */
    for (int v = 0; v < 16; v++) {
        if (target_list & (1u << v))
            virt_inject_virq_to_vcpu(vmid, v, sgi_id);
    }
}
```

设计要点：

- **翻译边界清晰**：`handle_vgic_sgi1r` 只负责"ICC_SGI1R_EL1 写 → vcpu_id 集合"，实际注入复用 `virt_inject_virq_to_vcpu`（Phase 12 已有的跨核/同核/saved-state 三路分派），不重复实现注入逻辑。
- **IRM 广播排除自身**：GICv3 语义下 `IRM=1` 广播给"除自身外的所有 PE"；定向（`TargetList`）语义下允许发给自身（GIC 会自动处理），此处与标准一致地逐位注入。
- **单 cluster 简化**：当前 MAX_VCPUS_PER_VM=4，且全部落在 Aff0 上；`Aff1/Aff2/Aff3` 非 0 的目标直接忽略（教学实现的最小够用策略）。

---

## 7. Guest 侧测试

### 7.1 新增 helper

```c
/* guest.h */
#define icc_sgi1r_write(v)  SYSREG_WRITE(S3_0_C12_C11_5, v)
#define VSGI_TRAP_TEST_SGI_ID 6
```

`icc_sgi1r_write` 是**标准 GICv3 SGI 发送**——直接写 `ICC_SGI1R_EL1`，不含任何 HVC，用于验证 trap-and-emulate 路径。

### 7.2 测试流程（`phase17_vsgi_trap_test`）

主核（vcpu0）：

1. 清零共享变量 `vsgi_received/vsgi_count/done_flag` 并 `dmb ish`
2. 构造 `ICC_SGI1R_EL1` 值：`INTID=SGI#6 [27:24]`、`TargetList bit1=vcpu1 [15:0]`，调用 `icc_sgi1r_write()`
3. 轮询 `done_flag == 5`（次核收到 SGI 后置位），超时则 FAILED
4. 校验 `vsgi_count == 1` 且 `vsgi_received` 的 bit6 置位 → PASSED

次核（vcpu1）：

1. 两阶段握手：先等 `done_flag == 0`（主核进入 Phase 17 并清零共享变量），再等 `vsgi_count >= 1`（SGI 到达）
2. 收到后置 `done_flag = 5` 通知主核

IRQ handler 复用 Phase 12 的 SGI 分支，对 SGI#6 累加 `vsgi_count` 并置 `vsgi_received` 位。

主核调用：在 `phase16_gicr_test()` 之后；次核调用：在 `phase12_vsgi_test()` 之后。

### 7.3 两阶段握手（竞态修复）

Phase 12 结束时 `vsgi_count` 遗留为 5，若次核仅用 `while (*vsgi_count < 1)` 判断，会误判"已收到 SGI"而直接退出，与主核时序错位。修复采用两阶段握手：

- 主核先清零 `vsgi_count`，再清零 `done_flag`；
- 次核先等 `done_flag == 0`，再等 `vsgi_count >= 1`。

由于主核清零 `done_flag` 前已先清零 `vsgi_count`，次核看到 `done_flag==0` 时 `vsgi_count` 必为干净值，消除了 Phase 12 遗留变量的干扰。

### 7.4 预期输出

```
vsgi-trap: master (vcpu0) writing ICC_SGI1R_EL1 -> SGI#6 to vcpu1...
vsgi-trap: secondary (vcpu1) waiting for SGI#6...
vsgi-trap: secondary received SGI! count=1
vsgi-trap: secondary received count = 1
vsgi-trap: PASSED (ICC_SGI1R_EL1 trap-and-emulate delivered SGI#6)
Phase 17: ALL tests done
```

---

## 8. 改动文件清单

| 文件 | 改动 |
|------|------|
| `include/arch/aarch64/sysregs.h` | 新增 `ICH_HCR_EL2_TC(1<<10)` 宏 |
| `include/boards/virt/vgic.h` | 新增 `handle_vgic_sgi1r` 声明 |
| `hypervisor/boards/virt/vgic.c` | `vgic_init` 叠加 `TC`；新增 `handle_vgic_sgi1r` 实现 |
| `hypervisor/arch/aarch64/sync_exc.c` | `handle_trap_system` 写分支识别 `ICC_SGI1R_EL1` 并分派 |
| `guests/el1_test/guest.h` | 新增 `icc_sgi1r_write` / `VSGI_TRAP_TEST_SGI_ID` |
| `guests/el1_test/main.c` | 新增 `phase17_vsgi_trap_test` + 主/次核调用 |

---

## 9. 验证与回归

### 9.1 构建

```bash
make -f Makefile.virt all
```

### 9.2 运行回归

```bash
timeout 40 make -f Makefile.virt run
```

退出码 124（timeout 正常终止）表示 VM 未崩溃。关键断言：

- 两个 VM 均输出 `vsgi-trap: PASSED (ICC_SGI1R_EL1 trap-and-emulate delivered SGI#6)`
- Phase 1~16 既有测试全部通过（含 Phase 12 的 HVC 半虚拟化 SGI 测试，验证 trap 与 HVC 两条路径并存）
- 进入主循环后持续输出 `tick N` 心跳，证明 `TC` 使能未破坏正常中断流（vtimer PPI 27 的 HW 直通不受影响）

### 9.3 回归结果（实际输出摘录）

```
vsgi-trap: master (vcpu0) writing ICC_SGI1R_EL1 -> SGI#6 to vcpu1...
vsgi-trap: secondary (vcpu1) waiting for SGI#6...
vsgi-trap: secondary received SGI! count=1
vsgi-trap: secondary received count = 1
vsgi-trap: PASSED (ICC_SGI1R_EL1 trap-and-emulate delivered SGI#6)
Phase 17: ALL tests done
guest: all tests done, entering main loop
```

---

## 10. 后续规划（方向 1 剩余子项）

1. ~~虚拟 SGI 的 trap-and-emulate~~（本 Phase 完成）
2. **优先级与 pending 状态的完整建模**：实现 `GICR_IPRIORITYR/GICD_IPRIORITYR` 写、`ISPENDR/ICPENDR`、虚拟中断优先级排序，使 vGIC 更接近真实硬件语义。
3. **vGIC 注入路径统一**：把 `ivshmem doorbell` / `shm_notify` 的"直写 saved state"老式注入统一为 `vgic_inject_virq` 或 `vgic_inject_virq_to_vm`，消除分散的门控逻辑。
4. **per-PE GICR 状态**：当前 per-VM 单份 GICR，若要完整支持多 vCPU 私有中断，可将 `struct vgic_rdist` 扩展为 per-vCPU 数组。

---

## 11. 版本信息

- **Phase**：17
- **主题**：虚拟 SGI trap-and-emulate（捕获 ICC_SGI1R_EL1）
- **Tag**：`v0.17.0-phase17-vsgi-trap`
- **前序 Tag**：`v0.16.0-phase16-vgicr`
