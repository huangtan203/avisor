# Phase 25：Live Migration 热迁移 — 详细设计文档

## 1. 概述

### 1.1 背景

方向 3：虚拟化可靠性 / 运维能力。

前面的 Phase 已经建立了完整的 VM 生命周期：创建、调度、内存虚拟化、中断虚拟化、设备直通。但一个生产级 Hypervisor 还需要**热迁移（Live Migration）**能力——在不中断 Guest 运行的前提下，把一个 VM 从一台物理机（或一个 Hypervisor 实例）迁移到另一台，用于负载均衡、硬件维护、故障规避。

Phase 25 在单 Hypervisor 实例内实现了 **pre-copy 热迁移**的完整闭环：把源 VM（VM2）迁移到预先创建的 dormant 目标 VM（VM3），迁移过程中 Guest 几乎无感地继续运行，最终在目标 VM 上恢复执行。

### 1.2 目标

- ✅ 实现基于 Stage-2 AF 位的脏页跟踪
- ✅ 实现迭代预拷贝（pre-copy）迁移流程
- ✅ 实现 stop-and-copy：停止源 VM + 转移 vCPU / vGIC / 定时器状态
- ✅ 实现目标 VM 的恢复执行路径（ERET 到 HVC+4）
- ✅ 迁移后 Guest 可正确感知新的 VMID 并校验内存内容
- ✅ 新增 3 个 HVC 接口：MIGRATE_START / PRECOPY / FINISH
- ✅ Guest 侧完整测试验证（Test 18）
- ✅ 附带修复 Phase 23 回收算法误回收 Guest 内核区的 bug

### 1.3 非目标

- 不实现跨物理机的网络传输 —— 单实例内两个 VM 之间的迁移（共享页池直接拷贝）
- 不实现后拷贝（post-copy）或混合迁移 —— 仅 pre-copy
- 不实现脏页写跟踪（ARMv8.4 Dirty Bit）—— 用 AF 位近似（访问位超集）
- 不实现迁移带宽限速 / 收敛阈值自动判停 —— 由 Guest 手动分阶段调用

---

## 2. 设计总览

### 2.1 迁移模型：pre-copy

```
     源 VM (VM2)                              目标 VM (VM3, dormant)
 ┌───────────────┐                          ┌───────────────────────┐
 │ HVC_MIGRATE_  │                          │  create_task_on_cpu   │
 │  START        │  清 AF + 全量拷贝          │  migration_resume_    │
 │  PRECOPY      │  收集 AF=1 脏页并拷贝       │  loader (WAITING)     │
 │  FINISH       │  stop-and-copy + 状态转移  │                       │
 └───────┬───────┘                          └───────────┬───────────┘
         │                                              │
         │          schedule() 切走源 VM                 │ 恢复执行
         └──────────────────────────────────────────────┘
                                              ERET 到 HVC+4 继续运行
```

### 2.2 脏页跟踪原理

复用 Stage-2 描述符的 **Access Flag（AF，bit 10）**：

1. `START` 阶段清除源 VM 全部映射页的 AF 位
2. Guest 继续运行，访问某页 → 触发 Access Flag Fault → `handle_access_flag_fault` 自动置位 AF
3. `PRECOPY` / `FINISH` 阶段收集 **AF=1** 的页 = "自上次清 AF 后被访问过"的脏页

**说明**：AF 跟踪的是"访问"而非"写"，读过的页也会被重复拷贝，这是 AF 方案的固有超集行为，仅带来轻微额外拷贝，不影响正确性。

### 2.3 状态转移清单

迁移必须转移的 vCPU 状态：

| 状态 | 存储位置 | 说明 |
|------|---------|------|
| 通用寄存器现场 | `struct pt_regs` | FINISH 时捕获的 ERET 现场 |
| EL1 系统寄存器 | `cpu_sysregs` | SP_EL1 / ELR_EL1 / SPSR / TTBR 等 |
| vGIC CPU 接口 | `tsk->virt` | gic_vmcr / gic_lr[3] |
| 虚拟定时器 | `tsk->virt` | cntv_ctl / cntv_cval / vtimer_pending |
| vGIC MMIO 状态 | `board_data` | GICD / GICR（trap-and-emulate 建模） |
| 计数器偏移 | `mm.cntvoff` | 保证迁移后 CNTVCT 连续 |

---

## 3. 迁移三阶段

### 3.1 migration_start

```c
int migration_start(struct task_struct *src)
{
    mig_active = 1;
    mig_walk(src, MIG_OP_CLEAR_AF);   /* 清 AF，开启脏页跟踪 */
    mig_walk(src, MIG_OP_FULL_COPY);  /* 全量拷贝所有映射页 */
    return 0;
}
```

### 3.2 migration_precopy

```c
int migration_precopy(struct task_struct *src)
{
    return mig_walk(src, MIG_OP_COLLECT_DIRTY);  /* 返回本轮脏页数 */
}
```

可多轮调用迭代收敛，每轮拷贝的脏页数递减。

### 3.3 migration_finish

```c
int migration_finish(struct task_struct *src)
{
    dirty = mig_walk(src, MIG_OP_COLLECT_DIRTY);   /* 1. 最后一轮脏页拷贝 */

    memcpy(&mig_saved_regs, task_pt_regs(src), sizeof(struct pt_regs)); /* 2. 捕获 ERET 现场 */
    mig_saved_regs.regs[0] = dirty;                                    /* 脏页数写入恢复后 x0 */

    memcpy(&mig_dst->cpu_sysregs, &src->cpu_sysregs, ...);              /* 3. EL1 系统寄存器 */

    vgic_save_state(&src->virt);                                       /* 4. 新鲜捕获 vGIC + vtimer */
    src->virt.cntv_cval = READ_SYSREG64(CNTV_CVAL_EL0);
    ...

    virt_copy_vgic_state(mig_dst, src);                                /* 5. vGIC MMIO 状态 */
    memcpy(&mig_dst->virt, &src->virt, ...);                           /* 6. per-vCPU 虚拟状态 */
    mig_dst->mm.cntvoff = src->mm.cntvoff;                             /* 7. 计数器偏移 */

    src->state = TASK_ZOMBIE;                                          /* 8. 停止源 VM */
    mig_dst->state = TASK_RUNNING;
    mig_dst->priority = src->priority;
    mig_dst->counter = 1000;
    mig_active = 0;
    schedule();                                 /* 立即调度到目标 VM，本函数不返回 */
}
```

---

## 4. 页表遍历与拷贝

### 4.1 mig_walk

**位置**：`hypervisor/common/migration.c`

三种操作：

```c
enum {
    MIG_OP_CLEAR_AF,      /* 清除全部 AF 位 */
    MIG_OP_FULL_COPY,     /* 全量拷贝所有映射页 */
    MIG_OP_COLLECT_DIRTY, /* 收集并拷贝 AF=1 的脏页，同时清 AF */
};
```

遍历 L1 → L2 → L3，处理 **L2 block（2MB）** 与 **L3 page（4KB）** 两种粒度：

- 跳过无效 entry
- 跳过 device memory（`(e & (0x7 << 2)) != MM_STAGE2_MEMATTR`，MMIO 不可拷贝）
- `COLLECT_DIRTY` 对 AF=1 的页拷贝后清 AF（供下一轮）

清 AF 后统一 `tlbi vmalle1 + dsb + isb`，保证下次访问触发 AF Fault。

### 4.2 mig_copy_page（防泄漏）

```c
static void mig_copy_page(struct task_struct *src, vaddr_t ipa, paddr_t src_pa)
{
    if (ipa_to_pa(mig_dst, ipa, &dst_pa) == 0) {
        /* 目标已映射：复用（覆盖内容），避免预拷贝每轮泄漏一页 */
        memcpy((void *)TO_VADDR(dst_pa), (void *)TO_VADDR(src_pa), PAGE_SIZE);
    } else {
        void *dst_va = allocate_page();
        memcpy(dst_va, (void *)TO_VADDR(src_pa), PAGE_SIZE);
        map_stage2_page(mig_dst, ipa, TO_PADDR(dst_va), MMU_STAGE2_PAGE_FLAGS);
    }
}
```

多轮预拷贝不泄漏的关键：**先判断目标 VM 该 ipa 是否已映射**，已映射则复用原物理页覆盖，否则才新分配。

### 4.3 地址提取用 MM_STAGE2_PA_MASK

mig_walk 提取 descriptor 物理地址用 `MM_STAGE2_PA_MASK`（Phase 24 引入），而非 `PAGE_MASK`，避免 KSM 的 bit 55 污染。

---

## 5. vGIC 状态拷贝

### 5.1 virt_copy_vgic_state

**位置**：`hypervisor/boards/virt/virt_board.c`

```c
void virt_copy_vgic_state(struct task_struct *dst, struct task_struct *src)
{
    /* 1280 字节，8 对齐，可整块 memcpy */
    memcpy(&dd->vgicd, &sd->vgicd, sizeof(struct vgic_dist));

    /* 44 字节（非 8 的倍数），逐字段拷贝 */
    dd->vgicr.ctlr = sd->vgicr.ctlr;
    dd->vgicr.ppi_enable = sd->vgicr.ppi_enable;
    dd->vgicr.pending = sd->vgicr.pending;
    for (int i = 0; i < 32; i++)
        dd->vgicr.priority[i] = sd->vgicr.priority[i];
}
```

**关键约束**：`struct vgic_rdist` 是 **44 字节（非 8 的倍数）**，而 `utils.S` 的 `memcpy` 以 8 字节批量 `ldr/str` 步进，会越界写 4 字节。因此 `vgicr` 必须逐字段拷贝；`vgic_dist`（1280 字节）则可整块 `memcpy`。

---

## 6. 目标 VM 恢复路径

### 6.1 dormant VM3 创建（main_virt.c）

```c
int pid = create_task_on_cpu(0, migration_resume_loader, NULL);
cpu0_tasks[pid]->vmid = 3;
cpu0_tasks[pid]->vcpu_id = 0;
cpu0_tasks[pid]->state = TASK_WAITING;  /* dormant，直到 FINISH 唤醒 */
virt_register_vcpu(cpu0_tasks[pid], 3, 0);
migration_set_dest(cpu0_tasks[pid]);
```

### 6.2 migration_resume_loader

```c
int migration_resume_loader(void *arg, struct pt_regs *regs)
{
    memcpy(regs, &mig_saved_regs, sizeof(struct pt_regs));
    return 0;
}
```

首次调度流程：`switch_from_kthread` → `prepare_task`（先设 pstate，再调 loader，再 `set_cpu_sysregs`）→ loader 内 `memcpy` 覆盖为源 VM 现场 → `kernel_exit` **ERET 到 HVC+4**（源 VM 的 HVC 指令 `ELR_EL2` 硬件已自动 +4）。

### 6.3 返回值传递

FINISH 捕获的 `pt_regs->pc` 已是 HVC+4，`handle_hvc64` 在 FINISH 之后会把返回值写回 `regs->regs[0]`。因此 `migration_finish` 通过 `mig_saved_regs.regs[0] = dirty` 显式把脏页数作为返回值传给恢复后的目标 VM。

---

## 7. HVC 接口

### 7.1 HVC_MIGRATE_START (#50)

开始迁移：清 AF（开启脏页跟踪）+ 全量拷贝。

**返回**：0 成功，-1 失败

### 7.2 HVC_MIGRATE_PRECOPY (#51)

迭代预拷贝：收集并拷贝 AF=1 的脏页。

**返回**：本轮拷贝的脏页数

### 7.3 HVC_MIGRATE_FINISH (#52)

完成迁移：stop-and-copy + 状态转移，**不返回源 VM**（目标 VM 恢复后返回脏页数）。

---

## 8. Guest 测试用例

### 8.1 测试地址

- `PHASE25_BASE_IPA = 0x70000000`（与 Phase 24 的 0x60000000 错开）
- 8 页迁移测试内存，前 4 页制造脏页

### 8.2 测试流程（Test 18）

```
1. 分配 8 页迁移测试内存
2. 写入初始内容（每页独特模式 0x50..0x57）
3. START：清 AF + 全量拷贝
4. 修改前 4 页内容（0xA0..0xA3，制造脏页）
5. PRECOPY：收集并拷贝脏页
6. FINISH：stop-and-copy + 状态转移（此后代码在 VM3 上运行）
7. 验证迁移后 guest_vmid() == 3
8. 验证内存：前 4 页 = 0xA0..0xA3，后 4 页 = 0x54..0x57
```

### 8.3 测试结果

```
[vm2] --- Test 18: Phase 25 Live Migration ---
[vm2]   [3] START：清 AF + 全量拷贝完成
[vm2]   [5] PRECOPY：本轮拷贝脏页数 = 8
[vm3]   [6] FINISH：完成迁移，最后一轮脏页数 = 6
[vm3]   [7] 迁移后 VMID = 3
[vm3]   [8] 迁移后内存内容正确
[vm3]   PASSED: Live Migration 成功（VM2 -> VM3）
```

---

## 9. 附带修复：Phase 23 回收误回收 Guest 内核区

### 9.1 现象

VM2 在 Phase 23 回收测试中挂起，demand page fault 反复出现在 `ipa=0x40000000 / 0x40008000 / 0x403ff000`（Guest 代码 / 向量表 / 栈）。

### 9.2 根因

`reclaim_pages` 的时钟算法会回收 Guest 的**静态内存区**（代码镜像 `0x40000000`、栈顶 `0x40400000`），导致下次定时器中断跳到被 unmap 并被 demand paging 清零的向量表页，Guest 执行零字节后崩溃。

### 9.3 修复

```c
#define GUEST_STATIC_REGION_END 0x40400000UL

/* L2 block / L3 page 分支中： */
if (block_va < GUEST_STATIC_REGION_END) {
    /* Guest 内核区：跳过，不回收 */
}
```

回收时钟算法跳过 IPA 前 4MB，只回收 Guest 显式申请的数据页（测试页均在 0x48000000 以上），同时仍计入 `scanned_mapped` 以保持扫描预算不变式。

---

## 10. 文件变更清单

| 文件 | 类型 | 说明 |
|------|------|------|
| `include/common/migration.h` | 新增 | 迁移接口声明（含 struct pt_regs 前向声明） |
| `hypervisor/common/migration.c` | 新增 | 脏页跟踪 + 预拷贝 + 状态转移 + resume loader |
| `include/common/hvc.h` | 修改 | 新增 HVC_MIGRATE_START #50 / PRECOPY #51 / FINISH #52 |
| `include/boards/virt/virt_board.h` | 修改 | 新增 virt_copy_vgic_state 声明 |
| `hypervisor/boards/virt/virt_board.c` | 修改 | 新增 virt_copy_vgic_state（GICD memcpy + GICR 逐字段） |
| `hypervisor/arch/aarch64/sync_exc.c` | 修改 | HVC handler 新增 3 个 MIGRATE case |
| `hypervisor/boards/virt/main_virt.c` | 修改 | 创建 dormant VM3 + 注册迁移目标 |
| `hypervisor/common/mm.c` | 修改 | 修复回收误回收 Guest 内核区 |
| `guests/el1_test/guest.h` | 修改 | Guest 侧 MIGRATE HVC 常量 |
| `guests/el1_test/main.c` | 修改 | Phase 25 测试用例（Test 18） |

---

## 11. 总结

Phase 25 实现了完整的单实例 pre-copy 热迁移：

1. **硬件辅助脏页跟踪**：复用 Stage-2 AF 位，无需软件写保护
2. **防泄漏预拷贝**：目标映射复用判断，多轮迭代不泄漏物理页
3. **完整状态转移**：vCPU + vGIC MMIO + 虚拟定时器 + 计数器偏移
4. **无缝恢复**：resume loader 覆盖 pt_regs，ERET 到 HVC+4 继续执行
5. **细粒度拷贝约束**：识别 vgic_rdist 44 字节非 8 对齐，逐字段拷贝
6. **附带修复**：修正 Phase 23 回收误回收 Guest 内核区的 flaky bug

至此 aVisor 具备了 Type-1 Hypervisor 的核心运维能力之一——热迁移，为后续跨物理机迁移、容灾切换等场景奠定了基础。
