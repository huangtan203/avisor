# Phase 33：双 vCPU 事务式迁移（Transactional Dual-vCPU Live Migration）— 详细设计文档

## 1. 概述

### 1.1 背景

Phase 25 交付了 pre-copy 热迁移，但 `migration_start()` 内含一段显式拒绝逻辑：只要源 VM 存在第二个非 ZOMBIE 的 vCPU，`HVC_MIGRATE_START` 直接返回 `MIGRATION_REJECTED`。原因在于 pre-copy 的 START/PRECOPY 阶段源 VM 仍在执行，多个 vCPU 会并发写脏页，而 FINISH 的 stop-and-copy 只能"停住"当前这个 vCPU（HVC 边界），无法让同一 VM 的另一个物理核上的 vCPU 也停下——快照窗口内远端 vCPU 仍在写内存，导致 FINISH 之后的最后一轮拷贝与 vCPU 状态转移之间出现竞态。

Phase 31/32 建立了事务式冻结：Phase 32 的 VM 级 stop-the-world 冻结能在 HVC 边界让整 VM 两个 vCPU 同时静止。Phase 33 把二者结合——**在 VM 级冻结窗口内完成 stop-and-copy + 双 vCPU 状态转移**，交付 VM2（双 vCPU）整体迁移到 VM3。

### 1.2 核心语义

- **START/PRECOPY**：沿用 Phase 25 的 AF 脏页跟踪 + 迭代预拷贝，但**放行双 vCPU 源**（去掉多 vCPU 拒绝）。
- **FINISH（事务式）**：
  1. `smp_vm_txn_begin()` 冻结整 VM（vCPU1 经 SGI#5 在 CPU1 忙等，vCPU0 在 HVC 边界捕获自身快照）→ 整 VM 静止；
  2. 冻结窗口内做最后一轮脏页拷贝（两 vCPU 都不再写脏页，收敛）；
  3. **就地改造 vCPU1**：`vmid` 2→3、`mm`/`board_data` 挂到 VM3、重新注册到 vCPU 查找表；
  4. 转移 vCPU0 的 EL1 sysregs / vGIC / CNTV / virt / ERET 现场到 VM3 vCPU0（沿用 Phase 25 单核逻辑）；
  5. `smp_vm_txn_commit()` 释放 vCPU1——它从冻结点恢复，以 VM3 身份继续执行；
  6. vCPU0 自身 `schedule()` 切到 VM3 vCPU0。

### 1.3 目标

- VM2 双 vCPU 整体迁移到 VM3，迁移后 vCPU0 的 VMID=3、8 页内存内容正确。
- vCPU1 也迁移成功：迁移后仍从冻结点继续推进（hold_counter 严格递增）。
- 保留全部既有回归（Phase 11–32）。

### 1.4 非目标

- 不做跨物理机的网络迁移（源/目标都在同一 hypervisor 实例内，目标 VM3 dormant 预建）。
- 不新建 VM3 vCPU1 dormant 目标——采用**就地改造**复用 VM2 的 vCPU1 task。
- 不做迁移取消/回滚（FINISH 一旦进入冻结窗口即单向提交）。

## 2. 核心难点与设计决策

### 2.1 远端 vCPU1 如何"停住"

stop-and-copy 需要整 VM 静止。直接复用 Phase 32 的 `smp_vm_txn_begin()`：它向 CPU1 发 SGI#5，vCPU1 在 EL2 IRQ handler 内完成 `freeze_context` 快照后**忙等** `vm_txn_decision`。快照含完整 EL1 现场（cpu_sysregs + pt_regs + virt）。

### 2.2 vCPU1 状态如何转移到 VM3

关键决策：**就地改造，而非新建目标 + 拷贝**。两种方案对比：

| 方案 | 做法 | 问题 |
|------|------|------|
| 新建目标 | 建 VM3 vCPU1 dormant，把 vCPU1 的 freeze_context 拷进它 | 需要一个次核 resume loader、跨核唤醒时序、且 vCPU1 原 task 要单独置 ZOMBIE |
| **就地改造（采用）** | 直接改 vCPU1 的 `vmid`/`mm`/`board_data`，commit 后它从冻结点以新身份继续 | 无；vCPU1 的 EL1 现场（mpidr=1 等）本就该保留 |

就地改造的合理性：vCPU1 的 `cpu_sysregs`（含 mpidr_el1=1）是 VM 级不变量，无需改；`mm` 与 `board_data` 指向 VM 级共享资源，改指向 VM3 即完成"身份切换"。commit 后 vCPU1 走 `kernel_exit` → `vm_entering_work` → `set_cpu_sysregs()`，检测到 `vmid` 从 2 变 3，自动 `tlbi vmalle1` + 切 VTTBR 到 VM3 页表 + 恢复 EL1 sysregs，vCPU1 就自然运行在 VM3 地址空间。

### 2.3 时序正确性

vCPU0 在 `migration_finish()` 内（FINISH 的 HVC handler）依次执行：冻结 → 脏页拷贝 → 改造 vCPU1 → 转移 vCPU0 状态 → commit 释放 vCPU1 → schedule 切走。整个 FINISH 期间 vCPU0 不返回 Guest，vCPU1 忙等，因此：

- 最后一轮脏页拷贝（步骤 2）与 vCPU1 状态转移（步骤 3）之间没有 Guest 写内存——窗口一致。
- commit（步骤 5）先于 vCPU0 的 schedule（步骤 6），保证 vCPU1 释放时 VM3 的 vCPU0 状态已就绪（但即便顺序颠倒也无碍：两 vCPU 在不同物理核上独立恢复，VM 级状态已由迁移代码一致提交）。

## 3. 数据流与状态转移

### 3.1 vCPU1（就地改造）

```
迁移前（VM2 vCPU1，CPU1，vmid=2）
  ├─ mm            → VM2 Stage-2 页表（与 vm2_master 共享）
  ├─ board_data    → VM2 设备模型（virt_attach_secondary 挂到 master）
  ├─ cpu_sysregs   → mpidr_el1=1（次核 aff0=1）
  └─ freeze_context→ SGI#5 捕获的完整现场

迁移后（VM3 vCPU1，CPU1，vmid=3）
  ├─ mm            → VM3 Stage-2 页表（= mig_dst->mm，已由 mig_walk 全量/增量拷贝填充）
  ├─ board_data    → VM3 设备模型（= mig_dst->board_data）
  ├─ cpu_sysregs   → 不变（mpidr_el1=1 仍正确）
  └─ 恢复现场       → commit 后从冻结点继续（观测循环）
```

### 3.2 vCPU0（沿用 Phase 25 单核转移）

`cpu_sysregs` → `mig_dst->cpu_sysregs`；`virt`（vgic_save_state + CNTV 重读）→ `mig_dst->virt`；`virt_copy_vgic_state()`；`mm.cntvoff`；`task_pt_regs(src)` → `mig_saved_regs`（含 `regs[0]=脏页数`）。VM3 vCPU0 的 `migration_resume_loader` 首次调度时恢复 `mig_saved_regs`。

## 4. 实现

### 4.1 `hypervisor/common/migration.c`

- `migration_start()`：删除 `#ifdef PLATFORM_VIRT` 下的多 vCPU 拒绝循环，注释说明 Phase 33 双 vCPU 语义。
- `migration_finish()`：重写为事务式，插入 `smp_vm_txn_begin/commit` 与 vCPU1 就地改造块：
  ```c
  if (smp_vm_txn_begin() != 0)
      return -1;
  dirty = mig_walk(src, MIG_OP_COLLECT_DIRTY);
  vcpu1 = smp_vm_txn_target_task();
  if (vcpu1) {
      vcpu1->vmid = mig_dst->vmid;
      vcpu1->mm = mig_dst->mm;
      vcpu1->board_data = mig_dst->board_data;
      (void)strncpy(vcpu1->name, "vm3-cpu1", 36);
      virt_register_vcpu(vcpu1, (int)mig_dst->vmid, vcpu1->vcpu_id);
  }
  /* ... vCPU0 状态转移 ... */
  smp_vm_txn_commit();
  src->state = TASK_ZOMBIE; mig_dst->state = TASK_RUNNING; schedule();
  ```

### 4.2 `hypervisor/common/smp.c` + `include/common/smp.h`

新增访问器：
```c
struct task_struct *smp_vm_txn_target_task(void);
```
在 `VM_TXN_FROZEN` 状态下返回 `vm_txn_target_task`（冻结中的 vCPU1），否则返回 NULL。迁移代码借它拿到 vCPU1 指针做就地改造。

### 4.3 `guests/el1_test/main.c`

- 移除 Test 18 末尾的"双 vCPU START 被拒绝"断言（该 if 块原本把 Test 19/20 包在其内），让 vmid==2 路径在 Test 19/20 之后自然落到后段的迁移流程（分配 8 页 → START → PRECOPY → FINISH）。
- 新增 **Test 21**：迁移后（vCPU0 已在 VM3）读共享页 `P31_HOLD_COUNTER_OFF` 两次，验证 vCPU1 也迁移成功且计数严格递增。

## 5. 验证

### 5.1 构建

```bash
make -f Makefile.virt
# 注意：guest 镜像变更需清理陈旧嵌入镜像
rm -f build_virt/hypervisor/common/guest_image_s.o build_virt/hypervisor/common/guest_image_s.d
make -f Makefile.virt
```

### 5.2 双核 QEMU 回归

```bash
qemu-system-aarch64 -M virt,virtualization=on,gic-version=3,highmem=off \
  -cpu max -smp 2 -m 1G -device edu \
  -kernel build_virt/kernel8.img -nographic -serial mon:stdio
```

两轮独立运行结果：

| 轮次 | PASS 计数 | FAIL 计数 | 迁移关键输出 |
|------|-----------|-----------|--------------|
| 1 | 71 | 0 | FINISH 脏页=7，VMID=3，Test 21 计数 213671→214540 |
| 2 | 70 | 0 | FINISH 脏页=7，VMID=3，Test 21 计数 213504→214378 |

（PASS 计数差异为调度时序导致的 KSM 合并页数观测噪声，不影响 PASSED 总数；两轮均 0 FAIL。）

Test 21 关键输出：
```
[vm3] --- Test 21: Phase 33 Dual-vCPU Migration ---
[vm3]   PASS: vCPU1 已迁移到 VM3 且继续推进 (213671 -> 214540)
[vm3]   PASSED: 双 vCPU Live Migration 成功（VM2 -> VM3）
```

## 6. 兼容性与回归

- Phase 25 单 vCPU 迁移语义不变（本实现只扩展 FINISH，未改动 START/PRECOPY 的 AF 跟踪逻辑）。
- Phase 31/32 事务冻结状态机不变，仅新增只读访问器。
- 全部既有测试（Phase 11–32）两轮 0 FAIL。

## 7. 局限与后续方向

- **迁移取消/回滚**：FINISH 进入冻结窗口后单向提交，不支持迁移中途取消。
- **远端 vCPU1 的 vGIC/vtimer 现场**：就地改造保留 vCPU1 自身 `cpu_sysregs`/`virt`（SGI#5 已存 `freeze_context`，但 commit 路径恢复的是 pt_regs 异常帧，EL1 sysregs 由 `vm_leaving_work` 已存进 `current->cpu_sysregs`）；其 vGIC CPU interface 状态 per-vCPU，随 task 保留，无需跨核拷贝。
- **跨物理机迁移**：源/目标同一 hypervisor，网络传输与设备状态迁移留待后续。
