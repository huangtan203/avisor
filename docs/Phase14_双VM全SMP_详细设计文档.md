# Phase 14：双 VM 全 SMP（VM2 次 vCPU 跨核）详细设计文档

## 1. 概述

### 1.1 背景

Phase 13 完成了跨物理核 vCPU 调度与跨核 IPI 注入，把 VM1 的次 vCPU（vcpu1）放到 CPU1 上运行，并通过物理 SGI（IPI）触发目标核完成 vGIC 维护，从而让跨核 vSGI 稳定工作。

但当时 VM2 仍是**单核**（只有 vcpu0，运行在 CPU0）：

- Phase 11 的虚拟 SMP 测试（`phase11_smp_test`）对两个 VM 都会运行，VM2 因缺少次 vCPU 而稳定输出 `smp: FAILED (secondary vCPU timeout)`——这虽是"预期行为"，却让测试矩阵不完整；
- Phase 12 的 vSGI 测试（`phase12_vsgi_test`）则被 `vmid == 1` 硬编码屏蔽，VM2 从未验证过跨核 vSGI；
- `main_virt.c` 中遗留一段"VM2 次 vCPU 暂时禁用"的注释，理由是"CPU1 启动有问题 + 次 vCPU board 初始化会覆盖 ivshmem peer"。

Phase 13 已经修复了 CPU1 的 PSCI 启动问题，而 `virt_attach_secondary()`（Phase 11 引入）已经把次 vCPU 的 board 设备模型共享和 ivshmem peer 修正处理妥当。因此，启用 VM2 次 vCPU 的条件已经齐备。

### 1.2 目标

- ✅ 为 VM2 增加第二个 vCPU（vcpu1），跨核调度到 CPU1
- ✅ 两个 VM 都成为双核，4 个 vCPU 分布在 2 个物理核上（对称布局）
- ✅ Guest 测试矩阵补全：VM2 也运行虚拟 SMP + vSGI 测试并全部通过
- ✅ 清理过时的 board 初始化注释，验证 ivshmem peer 路由在多 vCPU 场景下依然正确
- ✅ 全量回归测试通过

### 1.3 平台与基础

- **硬件平台**：QEMU `virt` 机器，ARMv8-A，GICv3 + ITS + vGIC，2 物理核
- **Hypervisor**：aVisor（Type-1，EL2）
- **Guest**：EL1 测试程序（裸金属）
- **前序版本**：`v0.13.0-phase13-crosscore`

---

## 2. 整体架构设计

### 2.1 跨核 vCPU 布局变化

Phase 13 的布局（VM1 双核、VM2 单核）：

```
Phase 13
物理 CPU0                    物理 CPU1
├── idle                    ├── idle
├── VM1 vcpu0               └── VM1 vcpu1
└── VM2 vcpu0
```

Phase 14 之后（双 VM 全 SMP，对称布局）：

```
Phase 14
物理 CPU0                    物理 CPU1
├── idle                    ├── idle
├── VM1 vcpu0               ├── VM1 vcpu1
└── VM2 vcpu0               └── VM2 vcpu1
```

每个物理核上都有 3 个任务（1 个 idle + 2 个 vCPU），调度器在同一核内的任务间轮转；VM 的 vCPU 仍静态绑定到固定的 `phys_cpu`，不跨核迁移。

### 2.2 关键点

1. **VM2 vcpu1 创建**：复用 Phase 11 的 `shared_vm_loader`，与 VM1 vcpu1 完全对称
2. **vCPU 查找表**：`virt_register_vcpu(2, 1)` 把 VM2 vcpu1 注册到 `vm_vcpus[][]`，使 vSGI 路由能定位到它
3. **Guest 测试矩阵**：移除 `phase12_vsgi_test` 的 `vmid == 1` 硬编码屏蔽
4. **ivshmem peer 路由**：多 vCPU 创建顺序下 peer 指针最终仍正确指向各 VM 主 vCPU

---

## 3. VM2 次 vCPU 创建

### 3.1 实现

在 `main_virt.c` 中，紧接 VM2 vcpu0 之后，新增 VM2 vcpu1 的创建（与 VM1 vcpu1 对称）：

```c
/* VM2 次 vCPU：创建在 CPU1（Phase 14 双 VM 全 SMP，跨核 vCPU） */
{
    struct shared_vm_loader_args shared_args = {
        .master = vm2_master,
        .entry_point = 0x40000000,
    };
    int pid = create_task_on_cpu(1, shared_vm_loader, &shared_args);
    if (pid < 0)
        PANIC("failed to create VM2 vcpu1");
    cpu1_tasks[pid]->vmid = 2;
    cpu1_tasks[pid]->vcpu_id = 1;
    cpu1_tasks[pid]->phys_cpu = 1;
    /* 同步虚拟 MPIDR（次 vCPU 返回 aff0=1） */
    cpu1_tasks[pid]->cpu_sysregs.mpidr_el1 = 1;
    cpu1_tasks[pid]->priority = 10;
    cpu1_tasks[pid]->counter = 10;
    (void)strncpy(cpu1_tasks[pid]->name, "vm2-cpu1", 36);
    printf("[8.4] VM2 vcpu1 created (vmid=2, cpu1, pid=%d)\n", pid);
    /* 注册 vCPU 到板级查找表（用于 vSGI 等跨 vCPU 操作） */
    virt_register_vcpu(cpu1_tasks[pid], 2, 1);
}
```

### 3.2 与 VM1 vcpu1 的对比

| 项 | VM1 vcpu1 | VM2 vcpu1 |
|----|-----------|-----------|
| 物理核 | CPU1 | CPU1 |
| vmid | 1 | 2 |
| vcpu_id | 1 | 1 |
| 虚拟 MPIDR | 1 | 1 |
| 加载器 | `shared_vm_loader`（挂接 master 页表） | 同左 |
| master | `vm1_master` | `vm2_master` |

### 3.3 共享页表与设备模型

`shared_vm_loader` 等待 master 置起 `TASK_MM_READY`（镜像加载完成、Stage 2 页表就绪），随后：

```c
current->mm = master->mm;          /* 共享同一份 Stage 2 页表 */
virt_attach_secondary(current, master);  /* 共享设备模型 + 修正 ivshmem peer */
```

因此 VM2 的两个 vCPU 看到同一块 Guest 物理内存、同一套 virtio-net/ivshmem/vITS 设备模型、同一个 CNTVOFF 时间基准，只有 vGIC CPU 接口 / vtimer 是 per-vCPU 的。

---

## 4. Guest 测试矩阵补全

### 4.1 移除 vSGI 的 vmid 硬编码屏蔽

原实现只在 VM1 上运行 vSGI 测试：

```c
/* 主核 */
if (vmid == 1)
    phase12_vsgi_test();

/* 次核 */
if (vmid == 1) {
    phase12_vsgi_test();
}
```

Phase 14 改为两个 VM 都运行：

```c
/* 主核 */
phase12_vsgi_test();

/* 次核 */
phase12_vsgi_test();
```

### 4.2 为什么可以直接移除屏蔽

vSGI 的半虚拟化接口是 vmid 无关的：

- `HVC_VSGI_SEND`：以 `current->vmid` 为源，`virt_inject_virq_to_vcpu(vmid, target_vcpu, sgi_id)` 定位目标 vCPU
- `HVC_VSGI_GET_CPUID`：返回 `current->vcpu_id`

因此只要 VM2 的 vcpu1 已注册到 `vm_vcpus[1][1]`，vSGI 路由就能正确送达，与 VM1 完全对称。

### 4.3 虚拟 SMP 测试

`phase11_smp_test` 原本就没有 vmid 屏蔽，Phase 14 之后 VM2 也能找到次 vCPU 并跑完并发计数器测试，不再输出 `FAILED (secondary vCPU timeout)`。

---

## 5. ivshmem peer 路由正确性分析

### 5.1 创建顺序与 peer 指针状态

多 vCPU 创建过程中，`create_task_on_cpu` 会为**每个** vCPU 调用一次 `board_ops->initialize`（即 `virt_initialize`），其中 ivshmem 的 `peer_id` 按自动分配的临时 vmid 计算，会短暂写入全局 `ivshmem_peers[]` 数组。随后调用方覆盖 vmid/vcpu_id，`virt_attach_secondary` 在次 vCPU 首次调度时修正 peer 指针。

Phase 14 的四步创建顺序与最终 peer 数组状态：

| 步骤 | 创建 | virt_initialize 时的临时 peer | 最终 peer |
|------|------|------------------------------|-----------|
| 1 | VM1 vcpu0 | peer 0 = VM1 设备 | peer 0 = VM1 设备 ✅ |
| 2 | VM1 vcpu1 | 临时 peer 1（次核临时设备） | 由 attach 恢复 peer 0 |
| 3 | VM2 vcpu0 | 临时 peer 2（越界，不注册） | set_peer(1) → peer 1 = VM2 设备 ✅ |
| 4 | VM2 vcpu1 | 临时 peer 3（越界，不注册） | 由 attach 恢复 peer 1 |

由于 `IVSHMEM_MAX_PEERS == 2`，步骤 3/4 的临时 `peer_id`（2、3）都越界，`ivshmem_init` 内部有 `peer_id < IVSHMEM_MAX_PEERS` 检查，**不会**覆盖全局数组；而 `virt_attach_secondary` 会把对应 master 的 peer 指针恢复正确。

最终状态：`ivshmem_peers[0] = VM1 设备`、`ivshmem_peers[1] = VM2 设备`，doorbell 路由正确。

### 5.2 为什么不需要大的 board 初始化重构

原注释曾建议"重构 board 初始化（VM 级别）"。经分析，`virt_attach_secondary` 已把次 vCPU 的设备模型共享与 peer 修正处理妥当，临时设备模型的分配是一次性的、无正确性影响的冗余（每核多占一页）。在保持"小步快跑、不做过度设计"的前提下，Phase 14 仅清理过时注释，不引入 VM 抽象层。

---

## 6. Guest 启动同步回顾

VM2 双核后，其两个 vCPU 同样面临 Phase 13 解决的跨核并发启动竞态：

- master（CPU0）在 `guest_main` 中先 `mmu_init()`（清零 BSS + 建页表），再置 `boot_done = 1`
- secondary（CPU1）在 `guest_secondary_main` 中先等 `boot_done == 1`，再 `mmu_init()`

由于两个 vCPU 运行在不同物理核上并发执行，`boot_done` 同步标志 + `dmb ish` 内存屏障保证了次核不会在主核建好页表前写入 `guest_pg_dir`。这套机制 Phase 13 已在 VM1 上验证，Phase 14 对 VM2 同样生效（每个 VM 有各自独立的 Guest 物理内存，互不干扰）。

---

## 7. 测试结果

### 7.1 测试环境

- **命令**：`make -f Makefile.virt run`（`timeout 45`，退出码 124 = VM 稳定运行至超时）
- **配置**：VM1 双 vCPU（vcpu0→CPU0，vcpu1→CPU1），VM2 双 vCPU（vcpu0→CPU0，vcpu1→CPU1）

### 7.2 测试矩阵

| 测试项 | VM1 | VM2 | 结果 |
|--------|-----|-----|------|
| virtio-net loopback（4 项） | ✅ | ✅ | 通过 |
| ivshmem ping-pong | ✅ | ✅ | 通过 |
| edu（liveness/factorial/MSI） | ✅ | - | 通过 |
| vITS vLPI 注入 | ✅ | - | 通过 |
| Phase 10 综合（sched/mem/shm） | ✅ | ✅ | 通过 |
| Phase 11 vSMP（counter=20000） | ✅ | ✅ | 通过 |
| Phase 12 vSGI（单次 + 多次握手） | ✅ | ✅ | 通过 |

### 7.3 关键输出

```
[vm2] smp: counter = 20000 (expected 20000)
[vm2] smp: PASSED (spinlock + shared counter correct)
[vm2] vsgi: PASSED (master->secondary vSGI correct)
[vm2] vsgi: secondary received count = 5 (5 round-trips)
[vm2] vsgi: PASSED (multiple vSGI correct)

[vm1] smp: counter = 20000 (expected 20000)
[vm1] smp: PASSED (spinlock + shared counter correct)
[vm1] vsgi: PASSED (master->secondary vSGI correct)
[vm1] vsgi: secondary received count = 5 (5 round-trips)
[vm1] vsgi: PASSED (multiple vSGI correct)
```

> 注：多 vCPU 并发输出导致串口日志高度交织（4 个 vCPU + 2 个 idle 跨 2 核并发 printf），属预期现象；关键 PASSED 标记全部正确出现，无 FAILED。

---

## 8. 设计要点与思考

### 8.1 对称性是关键

VM2 vcpu1 的创建代码与 VM1 vcpu1 完全对称（同样的 `shared_vm_loader`、同样的 vmid/vcpu_id/phys_cpu/mpidr 修正、同样的 `virt_register_vcpu`）。这种对称性让 Phase 13 为跨核注入建立的机制（mailbox + 物理 SGI）无需任何修改即可直接服务第二个 VM，充分说明该机制是**通用**的，而非为 VM1 特制。

### 8.2 测试矩阵完整性带来的价值

Phase 14 之前，VM2 的 `smp: FAILED (secondary vCPU timeout)` 虽标注为"预期"，但任何真正的回归都难以从一堆预期失败中分辨出真实失败。补齐 VM2 的次 vCPU 后，两个 VM 的 SMP/vSGI 测试都必须是 PASSED，测试矩阵的**判定力**得到实质提升。

### 8.3 与真实 KVM 的对比

| 特性 | aVisor (Phase 14) | KVM/ARM |
|------|------------------|---------|
| 多 VM 多核布局 | 每 VM 双 vCPU，静态绑定物理核 | 每 VM 任意 vCPU，动态迁移 |
| vCPU 查找 | 静态 `vm_vcpus[vmid-1][vcpu_id]` 表 | `kvm->vcpus[]` + 动态管理 |
| vSGI 路由 | `current->vmid` + `vcpu_id` 查表 | vGIC Distributor pending 表 |
| 设备模型共享 | `board_data` 指向 master 设备模型 | VM 级设备模型 + per-vCPU 状态 |

---

## 9. 文件改动清单

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `hypervisor/boards/virt/main_virt.c` | 修改 | 新增 VM2 vcpu1 创建（跨核 CPU1）；清理过时注释；更新启动打印 |
| `guests/el1_test/main.c` | 修改 | 移除 `phase12_vsgi_test` 的 `vmid == 1` 屏蔽，两个 VM 都跑 SMP + vSGI |

---

## 10. 后续规划（Phase 15 展望）

1. **vSGI trap-and-emulate**（可选）：捕获 Guest 的 ICC_SGI1R_EL1 写入，提供标准 GIC 接口，替代 HVC 半虚拟化
2. **虚拟 GIC Distributor（GICD）模拟**：让 Guest 自行配置虚拟 SPI（当前由 Hypervisor 预启用 48/49/50/60）
3. **vCPU 动态迁移**：打破静态 `phys_cpu` 绑定，实现负载均衡（需要处理跨核迁移时的 vGIC 状态转移）
4. **ivshmem doorbell 跨核安全**：把 `ivshmem_mmio_write` 中"写 saved state"的老式注入路径统一为 `vgic_inject_virq_to_vm`，消除潜在跨核竞态

---

## 11. 版本信息

- **版本 tag**：`v0.14.0-phase14-dual-vm-smp`
- **前序版本**：`v0.13.0-phase13-crosscore`
- **完成日期**：2026-10
