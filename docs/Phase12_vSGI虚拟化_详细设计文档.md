# Phase 12：vSGI 虚拟化（虚拟核间中断）详细设计文档

## 1. 概述

### 1.1 背景

Phase 11 实现了虚拟 SMP（多 vCPU），让单个 VM 拥有多个 vCPU。然而，多 vCPU 之间缺乏标准的核间通信机制——真实多核系统通过 SGI（Software Generated Interrupt，软件生成中断）进行核间通信（IPI），但虚拟环境下 vCPU 之间的 SGI 需要 hypervisor 介入。

Phase 12 的目标是实现 **vSGI 虚拟化**，让 Guest 可以向同 VM 的其他 vCPU 发送虚拟 SGI，实现 vCPU 间的核间通信。

### 1.2 目标

- ✅ 实现 vCPU 查找机制（vmid + vcpu_id → task_struct）
- ✅ 新增 HVC_VSGI_SEND hypercall：Guest 向指定 vCPU 发送虚拟 SGI
- ✅ 新增 HVC_VSGI_GET_CPUID hypercall：Guest 查询当前 vCPU ID
- ✅ Guest 侧 vSGI 测试（单次发送 + 多次发送，均验证通过）
- ✅ 同物理核多 vCPU 场景下 vSGI 正确性验证
- ⏳ 跨物理核 vCPU + IPI（留待 Phase 13，需先解决 CPU1 启动问题）

### 1.3 平台与基础

- **硬件平台**：QEMU `virt` 机器，ARMv8-A，GICv3 + ITS + vGIC
- **Hypervisor**：aVisor（Type-1，EL2）
- **Guest**：EL1 测试程序（裸金属）
- **前序版本**：`v0.11.0-phase11-vsmp`

---

## 2. 整体架构设计

### 2.1 vSGI 模型

```
Guest VM1 (vmid=1)
┌──────────────────────────────────────────────┐
│  vcpu0 (主核)          vcpu1 (次核)          │
│  ┌───────────┐        ┌───────────┐         │
│  │  Guest OS │        │  Guest OS │         │
│  └─────┬─────┘        └─────┬─────┘         │
│        │  hvc #0 (VSGI_SEND) │              │
│        ▼                    ▲                │
└─────── EL2 ─────────────────┼───────────────┘
                               │
        ┌──────────────────────┘
        │  vGIC (LR 注入)
        ▼
   vcpu1 的 LR 中挂起
   虚拟 SGI 中断
```

### 2.2 设计选择：HVC 半虚拟化 vs Trap-and-Emulate

实现 vSGI 有两种主流方案：

| 方案 | 原理 | 优点 | 缺点 |
|------|------|------|------|
| **HVC 半虚拟化**（本方案） | Guest 通过 HVC 调用发送 vSGI | 实现简单、可控性强、调试方便 | 不符合 GIC 标准规范，需要 Guest 适配 |
| Trap-and-Emulate | 捕获 Guest 对 ICC_SGI1R 的写入，模拟路由 | 符合 GIC 规范，Guest 无需修改 | 实现复杂，需要解析亲和性路由，性能开销略高 |

**选择理由**：aVisor 是教学型 hypervisor，优先考虑实现简洁性和可理解性。HVC 方式逻辑清晰，便于学习理解 vSGI 的核心原理。未来可以扩展 Trap-and-Emulate 作为进阶特性。

### 2.3 关键组件

1. **vCPU 查找表**（板级）：`vm_vcpus[vmid-1][vcpu_id]` → task_struct
2. **Hypercall 接口**：`HVC_VSGI_SEND` + `HVC_VSGI_GET_CPUID`
3. **vGIC 注入**：复用 `vgic_inject_virq_to_vm()`，向目标 vCPU 的 LR 注入虚拟 SGI
4. **Guest 驱动**：`vsgi_send()` / `vsgi_get_cpuid()` 封装函数

---

## 3. vCPU 查找机制

### 3.1 数据结构

在 virt 板级维护一个二维数组，用于快速查找 vCPU：

```c
/* 最大 VM 数 = 4，每个 VM 最大 vCPU 数 = 4 */
static struct task_struct *vm_vcpus[MAX_VMS_PER_BOARD][MAX_VCPUS_PER_VM];
```

- `vm_vcpus[vmid - 1][vcpu_id]` → 对应 vCPU 的 task_struct 指针
- vmid 从 1 开始（0 是 hypervisor idle），所以数组下标 = vmid - 1

### 3.2 接口

```c
/* 注册 vCPU（创建 vCPU 时调用） */
void virt_register_vcpu(struct task_struct *tsk, int vmid, int vcpu_id);

/* 查找 vCPU（返回 NULL 表示不存在） */
struct task_struct *virt_find_vcpu(int vmid, int vcpu_id);

/* 向指定 vCPU 注入虚拟中断 */
int virt_inject_virq_to_vcpu(int vmid, int vcpu_id, uint32_t vintid);
```

### 3.3 注册时机

在 `main_virt.c` 中，每个 vCPU 创建并设置好 vmid/vcpu_id 后，调用 `virt_register_vcpu` 注册：

```c
/* VM1 vcpu0 */
cpu0_tasks[pid]->vmid = 1;
cpu0_tasks[pid]->vcpu_id = 0;
virt_register_vcpu(cpu0_tasks[pid], 1, 0);

/* VM1 vcpu1 */
cpu0_tasks[pid]->vmid = 1;
cpu0_tasks[pid]->vcpu_id = 1;
virt_register_vcpu(cpu0_tasks[pid], 1, 1);

/* VM2 vcpu0 */
virt_register_vcpu(vm2_master, 2, 0);
```

---

## 4. Hypercall 接口

### 4.1 HVC_VSGI_SEND

**功能**：向同 VM 的指定 vCPU 发送虚拟 SGI

**调用约定**：
- `x0` = target_vcpu_id（目标 vCPU 编号，0~MAX_VCPUS_PER_VM-1）
- `x1` = sgi_id（SGI 中断号，0~15）
- 返回值 `x0` = 0（成功）/ -1（失败：vCPU 不存在或 LR 已满）

**实现**（`sync_exc.c`）：

```c
case HVC_VSGI_SEND: {
    int target_vcpu = (int)(regs->regs[0] & 0xff);
    uint32_t sgi_id = (uint32_t)(regs->regs[1] & 0xf);
    if (sgi_id > 15) {
        regs->regs[0] = -1;
        break;
    }
    regs->regs[0] = virt_inject_virq_to_vcpu(
        (int)current->vmid, target_vcpu, sgi_id);
    break;
}
```

**注入流程**：
1. 从 ESR/x8 获取 hypercall 号和参数
2. 校验 sgi_id 范围（0~15）
3. 调用 `virt_inject_virq_to_vcpu(vmid, vcpu_id, vintid)`
4. 内部通过 `virt_find_vcpu` 找到目标 vCPU
5. 调用 `vgic_inject_virq_to_vm(target, vintid)` 注入
   - 目标正在运行 → 直接写硬件 LR
   - 目标未运行 → 修改 saved state 的 gic_lr[]

### 4.2 HVC_VSGI_GET_CPUID

**功能**：查询当前 vCPU 的 ID

**调用约定**：
- 无参数
- 返回值 `x0` = 当前 vCPU ID

**实现**：
```c
case HVC_VSGI_GET_CPUID:
    regs->regs[0] = current->vcpu_id;
    break;
```

---

## 5. Guest 侧实现

### 5.1 vSGI 驱动

```c
/* guest.h */
static inline int vsgi_send(int target_vcpu, int sgi_id)
{
    return (int)hvc_call2(HVC_VSGI_SEND, (u64)target_vcpu, (u64)sgi_id);
}

static inline int vsgi_get_cpuid(void)
{
    return (int)hvc_call(HVC_VSGI_GET_CPUID, 0);
}
```

### 5.2 中断处理

在 `guest_irq_handler` 中添加 SGI 处理（INTID 0-15）：

```c
} else if (intid < 16) {
    /* SGI：记录收到的 SGI 号和次数 */
    volatile u64 *vsgi_received = ...;
    volatile u64 *vsgi_count = ...;
    *vsgi_received |= (1ULL << intid);
    (*vsgi_count)++;
}
```

### 5.3 vSGI 测试用例

**共享内存布局**（复用 SMP 测试页面 0x40300000）：

| 偏移 | 字段 | 类型 | 说明 |
|------|------|------|------|
| 0x20 | vsgi_received | u64 | 收到的 SGI 号 bitmap |
| 0x28 | vsgi_count | u64 | 收到 SGI 的总次数 |
| 0x30 | vsgi_sender | u64 | 发送者 vcpu_id（预留） |

**测试 1：单次发送**
- 主核（vcpu0）调用 `vsgi_send(1, 5)` 向 vcpu1 发送 SGI#5
- 次核（vcpu1）收到中断后置 done_flag = 3
- 主核验证：count == 1 且 received_bitmap 包含 SGI#5

**测试 2：多次发送**
- 主核连续发送 5 次 SGI#5
- 次核累计收到 5 次后置 done_flag = 4
- 主核验证：count >= 5

---

## 6. 测试结果

### 6.1 测试环境

- **物理平台**：QEMU virt，2 CPU，GICv3 + ITS
- **配置**：VM1 双 vCPU（都在 CPU0），VM2 单 vCPU（CPU0）
- **命令**：`make -f Makefile.virt run`

### 6.2 测试矩阵

| 测试项 | VM1 | VM2 | 结果 |
|--------|-----|-----|------|
| virtio-net loopback | ✅ | ✅ | 通过 |
| ivshmem ping-pong | ✅ | ✅ | 通过 |
| edu MSI interrupt | ✅ | - | 通过 |
| vITS vLPI 注入 | ✅ | - | 通过 |
| Phase 10 综合 | ✅ | ✅ | 通过 |
| Phase 11 vSMP | ✅ | - | 通过 |
| **Phase 12 vSGI（单次）** | ✅ | - | 通过 |
| **Phase 12 vSGI（多次）** | ✅ | - | 通过 |

### 6.3 vSGI 测试详情

```
[vm1] vsgi: PASSED (master->secondary vSGI correct)
[vm1] vsgi: PASSED (multiple vSGI correct)
[vm1] Phase 12: ALL tests done
```

- **单次发送**：主核发 SGI#5 → 次核收到，count = 1，bitmap 正确 ✅
- **多次发送**：主核发 5 次 SGI#5 → 次核收到 5 次 ✅

---

## 7. 同核 vSGI 与跨核 vSGI

### 7.1 同核 vSGI（当前已实现）

当前 Phase 12 验证的是**同物理核上的多 vCPU**场景：

```
物理 CPU0
┌───────────────────────────────────┐
│  vm1-cpu0  ←vSGI←  vm1-cpu1       │
│   (运行中)         (待调度)       │
└───────────────────────────────────┘
     vGIC LR 切换（调度器上下文切换）
```

- 发送时：目标 vCPU 不在运行 → 写入 saved state 的 gic_lr[]
- 调度切换时：vgic_restore_state 将 LR 加载到硬件
- 目标 vCPU 运行时：自动收到虚拟中断

### 7.2 跨核 vSGI（待 Phase 13）

跨物理核的 vSGI 需要硬件 IPI 支持：

```
物理 CPU0                    物理 CPU1
┌──────────────┐            ┌──────────────┐
│  vm1-cpu0    │──IPI(SGI)──▶│  vm1-cpu1    │
│  (运行中)    │            │  (运行中)    │
└──────────────┘            └──────────────┘
```

实现步骤（Phase 13）：
1. 修复 CPU1 PSCI 启动问题
2. vCPU 可绑定到不同物理核
3. 跨核 vSGI 时，通过物理 SGI 触发目标核的 vGIC 维护中断
4. 目标核的 hypervisor 注入虚拟 SGI 到目标 vCPU

### 7.3 CPU1 启动问题（已知遗留）

**现象**：PSCI CPU_ON 返回 0（成功），但 CPU1 没有任何输出，`smp_online[1]` 始终为 0。

**已验证**：
- PSCI 调用方式：SMC（当前）→ 返回成功
- MPIDR 参数：0x1（aff0=1）→ 正确
- entry_point：`secondary_psci_entry` 的虚拟地址（等值映射下 = 物理地址）

**可能原因**（待 Phase 13 深入排查）：
- 次核启动后 MMU 关闭时访问地址不正确
- smp_boot_flag 的地址在 MMU 关闭时不可达
- 栈配置问题
- 异常向量未正确设置

---

## 8. 设计要点与思考

### 8.1 为什么 vSGI 需要 Hypervisor 介入？

在物理 GICv3 中，SGI 是通过写 ICC_SGI1R 寄存器触发的，Distributor 根据亲和性路由到目标 CPU。但在虚拟化环境中：

- Guest 看到的是**虚拟 CPU 接口**（vGIC）
- Guest 写 ICC_SGI1R 时，硬件只知道目标亲和性是虚拟的
- 硬件不知道哪个物理 CPU 上运行着目标 vCPU
- 所以**跨 vCPU 的 SGI 必须由 Hypervisor 路由**

### 8.2 vSGI 与硬件 vGIC 的关系

- **self SGI**（目标 = 自己）：硬件可直接处理，写入自己的 LR
- **other SGI**（目标 = 其他 vCPU）：必须 Hypervisor 介入
  - 找到目标 vCPU
  - 注入到目标 vCPU 的 vGIC 状态
  - 如果目标在别的物理核，还需要 IPI 触发

### 8.3 与真实 KVM 的对比

| 特性 | aVisor (Phase 12) | KVM/ARM |
|------|------------------|---------|
| 触发方式 | HVC 半虚拟化 | 写 ICC_SGI1R trap |
| 路由方式 | vcpu_id 直接查找 | 亲和性解析 → vcpu |
| 跨核支持 | 待实现 | 已支持（SGI 注入 + IPI） |
| vSGI 数量 | 16 (SGI0-15) | 16 (SGI0-15) |

---

## 9. 文件改动清单

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `include/common/hvc.h` | 新增 | HVC_VSGI_SEND (40) + HVC_VSGI_GET_CPUID (41) |
| `include/boards/virt/virt_board.h` | 新增 | vCPU 查找表接口声明 + 常量 |
| `hypervisor/boards/virt/virt_board.c` | 新增 | vm_vcpus 查找表 + register/find/inject 函数 |
| `hypervisor/boards/virt/main_virt.c` | 修改 | 各 vCPU 创建后调用 virt_register_vcpu |
| `hypervisor/arch/aarch64/sync_exc.c` | 修改 | 处理两个新的 HVC 调用 |
| `guests/el1_test/guest.h` | 新增 | vsgi_send / vsgi_get_cpuid 内联函数 |
| `guests/el1_test/main.c` | 新增 | phase12_vsgi_test + 中断处理中的 SGI 分支 |

---

## 10. 后续规划（Phase 13 展望）

1. **CPU1 启动修复**：深入调试 PSCI 次核启动问题
2. **跨核 vCPU 调度**：VM1 次 vCPU 放到 CPU1 上运行
3. **跨核 IPI 注入**：物理核间通过 SGI IPI 触发虚拟中断注入
4. **vSGI trap-and-emulate**（可选）：捕获 ICC_SGI1R 写入，标准 GIC 接口

---

## 11. 版本信息

- **版本 tag**：`v0.12.0-phase12-vsgi`
- **前序版本**：`v0.11.0-phase11-vsmp`
- **完成日期**：2026-10
