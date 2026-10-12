# aVisor Hypervisor Phase 5 详细设计文档 (SMP 多核与多描述符链)

> 版本：v0.5.0 (Phase 5)
> 平台：QEMU virt (ARMv8-A, GICv3, 虚拟化扩展)
> 目标：物理多核支持、虚拟 SMP、virtio-net 多描述符链、SMP 并发安全

---

## 1. 概述

### 1.1 目标达成情况

| 目标 | 状态 | 实现方式 |
|------|------|---------|
| 物理多核启动（2 核） | ✅ | PSCI CPU_ON（SMC #0，需 secure=on），次核独立初始化栈/GIC/定时器/调度器 |
| per-CPU 调度器 | ✅ | cpu_state[MAX_CPUS] 数组，每核独立 current/task 表/nr_tasks/rr_start |
| 虚拟 SMP（VM 多 vCPU） | ✅ | 多 vCPU 共享 VMID / Stage 2 页表 / 设备模型 / 时间基准，主 vCPU 加载镜像，次 vCPU 挂接共享 mm |
| virtio-net 多描述符链 TX | ✅ | 遍历 VRING_DESC_F_NEXT 链，逐描述符拼接读取数据包 |
| virtio-net 多描述符链 RX | ✅ | RX 描述符组成链（每条 4×256B），写入时按顺序填充链上各描述符并累计 len |
| SMP 并发安全 | ✅ | 页池自旋锁（ldaxr/stxr）+ virtio 设备自旋锁（MMIO 读写串行化） |
| GICv3 多核支持 | ✅ | gic_init_secondary() 初始化本核 Redistributor + CPU IF，SGI/PPI 操作 smp_processor_id() 的 Redistributor |
| 定时器 per-CPU | ✅ | hyp_tick_count[MAX_CPUS]，各核独立 100Hz 物理定时器 tick |

### 1.2 验证结果

**双核启动验证：**
```
[cpu0] booting on CPU0 (primary)
[9] SMP: PSCI CPU_ON(cpu1) returned 0
[cpu1] secondary CPU booted
[cpu1] Exception vectors installed (VBAR_EL2)
[cpu1] GICv3 initialized
[cpu1] vGIC enabled
[cpu1] Timer initialized (62500000 Hz)
[cpu1] Interrupts enabled
[cpu1] Entering scheduler loop...
[tick 100] Hypervisor is alive at EL2 (cpu0)
[tick 100] Hypervisor is alive at EL2 (cpu1)
```
- 两个物理核均正常启动，各自 100Hz tick 心跳稳定
- 次核通过 PSCI CPU_ON 从 EL2 入口启动
- per-CPU 的 VBAR/GIC/定时器/vGIC 均独立初始化成功

**虚拟 SMP 验证（VM2 双 vCPU）：**
```
[vm2] guest: hello from EL1!          (vcpu0, CPU0 上的主 vCPU)
[vm2] [vcpu1] guest: hello from secondary CPU (vm2)
[vm2] [vcpu1] guest: virtual GIC CPU interface initialized
[vm2] [vcpu1] guest: virtual timer started
[vm2] [vcpu1] guest: entering main loop
```
- VM2 的两个 vCPU 均启动并进入 EL1
- vcpu0 在 CPU0 上，vcpu1 在 CPU1 上（物理并行）
- 共享同一份 Stage 2 页表和 virtio-net 设备
- Guest 次核正确识别 MPIDR，走 secondary 初始化路径

**virtio-net 多描述符链环回测试（4/4 通过）：**
```
test1 PASSED (64 bytes, single desc)       单描述符基线
test2 PASSED (1024 bytes, 4-desc chain)    4 描述符链（256B × 4）
test3 PASSED (256 bytes, 1 desc boundary)  恰好 1 描述符边界
test4 PASSED (257 bytes, 2-desc chain)     跨 2 描述符（256B + 1B）
ALL loopback tests PASSED!
```
- VM1（单核）和 VM2（虚拟 SMP）均通过全部测试
- SMP 并发下 virtio-net 设备状态正确（自旋锁保护）

### 1.3 修改的关键文件

```
avisor/
├── include/
│   ├── common/
│   │   ├── sched.h            # ★ 重写：MAX_CPUS=2, cpu_sched_state, smp_processor_id(),
│   │   │                        per-CPU current/task/nr_tasks, task_struct 新增 vmid/vcpu_id/phys_cpu
│   │   └── task.h             # ★ 新增 create_task_on_cpu() 声明
│   └── boards/virt/
│       ├── gicv3.h            # ★ 新增 gic_init_secondary() 声明
│       ├── guest_loader.h     # ★ 新增 shared_vm_loader_args + shared_vm_loader()
│       ├── virt_board.h       # ★ 新增 virt_attach_secondary() 声明
│       └── virtio_mmio.h      # ★ virtio_mmio_dev 新增 lock 字段（SMP 自旋锁）
├── hypervisor/
│   ├── common/
│   │   ├── sched.c            # ★ 重写：per-CPU 调度器，sched_init_primary/secondary
│   │   └── mm.c               # ★ 新增 page_pool_lock（ldaxr/stxr 自旋锁）
│   ├── arch/aarch64/
│   │   ├── task.c             # ★ 重写：next_vmid 全局分配, create_task_on_cpu, 虚拟 MPIDR
│   │   └── sync_exc.c         # ★ 新增 MPIDR_EL1 陷阱仿真，HVC 控制台前缀用 vmid
│   └── boards/virt/
│       ├── boot_virt.S        # ★ 次核启动：secondary_entry + PSCI 入口 secondary_psci_entry
│       ├── main_virt.c        # ★ 重写主入口 + 新增 hypervisor_secondary_main + PSCI CPU_ON
│       ├── gicv3.c            # ★ per-CPU Redistributor, gic_init_secondary, SGI/PPI 按当前核操作
│       ├── arch_timer.c       # ★ hyp_tick_count[MAX_CPUS]，按核分别计数打印
│       ├── guest_loader.c     # ★ 新增 shared_vm_loader()：共享 mm + 设备模型 + 时间基准
│       ├── virt_board.c       # ★ 新增 virt_attach_secondary()：共享 board_data/cntvoff，独立 vGIC/vtimer
│       ├── pl011_uart.c       # ★ 修复：task[pid] → CPU_TASK_TABLE[pid]（per-CPU 任务表）
│       └── virtio_mmio.c      # ★ 新增 SMP 自旋锁，virtio_mmio_read/write 入口加锁
└── guests/el1_test/
    ├── start.S                # ★ 次核入口：secondary_entry，per-CPU 栈
    └── main.c                 # ★ 新增 guest_secondary_main()：次核 MMU/GIC/vtimer 初始化 + 多描述符链 TX/RX
```

---

## 2. 总体架构

### 2.1 Phase 5 系统视图

```
                 QEMU virt ( -smp 2, secure=on )
┌─────────────────────────────────────────────────────────────┐
│                     物理 CPU0                               │
│  ┌───────────── EL2 Hypervisor ──────────────────────┐     │
│  │ 调度器: idle + VM1(vcpu0) + VM2(vcpu0)            │     │
│  │ per-CPU: VBAR/GIC Redistributor/定时器/vGIC      │     │
│  │ 页池自旋锁 ←─────────────┐                         │     │
│  └───────────────────────────────────────────────────┘     │
│                              │                              │
│                 Stage 2 页表 (VMID=2)                      │
│                              │                              │
│                     物理 CPU1                               │
│  ┌───────────── EL2 Hypervisor ──────────────────────┐     │
│  │ 调度器: idle + VM2(vcpu1)                         │     │
│  │ per-CPU: VBAR/GIC Redistributor/定时器/vGIC      │     │
│  │ virtio 设备锁 ←──────────┘ (共享 virtio-net 设备)  │     │
│  └───────────────────────────────────────────────────┘     │
│                                                             │
│  Guest VM2 (虚拟 SMP, vcpu0 on CPU0, vcpu1 on CPU1)         │
│    - 共享 VMID=2, Stage 2 页表, virtio-net 设备, ivshmem    │
│    - 各自独立的 vGIC LR / 虚拟定时器状态 / 系统寄存器        │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 核心设计原则

1. **任务绑定物理核**：任务创建时指定 phys_cpu，不跨核迁移。简化调度器和缓存一致性。
2. **per-CPU 资源独立**：VBAR、GIC Redistributor、物理定时器、vGIC CPU Interface 等 per-CPU 硬件资源，每个核独立初始化独立使用。
3. **共享资源加锁**：全局页池、virtio 设备等跨核共享的数据结构，用自旋锁（ldaxr/stxr）保护并发访问。
4. **虚拟 SMP = 共享内存视图**：同一 VM 的多个 vCPU 共享 VMID、Stage 2 页表、设备模型、时间基准（CNTVOFF），各自独立 vCPU 上下文。

---

## 3. 物理多核启动

### 3.1 QEMU virt 平台的 SMP 行为

QEMU virt 平台上使用 `-kernel` 参数时：
- **主核 (BSP)**：从内核入口地址（0x40080000）启动
- **次核 (AP)**：默认处于关闭状态，需通过 PSCI CPU_ON 唤醒

PSCI（Power State Coordination Interface）是 ARM 标准的电源管理接口：
- QEMU 内置 PSCI v0.2 实现（EL3 安全世界固件模拟）
- 通过 `SMC #0` 指令调用（需 `-machine secure=on` 启用 EL3）
- CPU_ON function ID = `0xC4000003`（64 位）

### 3.2 次核启动流程

```
主核 (CPU0)                          次核 (CPU1)
─────────                            ─────────
hypervisor_main()
  ├─ sched_init_primary()           PSCI 休眠状态
  │   └─ 预初始化所有 CPU 的
  │      idle task + nr_tasks=1
  ├─ 创建所有任务（包括 CPU1 上的）
  ├─ psci_cpu_on(0x1, entry, 0) ────► secondary_psci_entry() (EL2, MMU off)
  ├─ smp_boot_flag = 1                 │
  └─ ...                               ├─ 设置栈 (LOW_MEMORY - cpu*0x4000)
                                       ├─ 等待 smp_boot_flag == cpu_id
                                       ├─ 开启 MMU（共享主核页表 pg_dir）
                                       └─ hypervisor_secondary_main()
                                            ├─ sched_init_secondary(cpu)
                                            ├─ irq_vector_init()  ← 关键！VBAR 是 per-CPU 的
                                            ├─ gic_init_secondary()
                                            ├─ vgic_init()
                                            ├─ timer_init()
                                            ├─ 设置 HCR_EL2
                                            └─ 调度循环
```

**关键点：**
1. **主核预初始化所有 CPU 的调度器基础状态**：因为任务创建在次核启动之前完成，必须先把每个 CPU 的 idle task 和 nr_tasks=1 设好，否则 pid 会从 0 开始分配，覆盖 task[0] 的 idle task。
2. **VBAR_EL2 是 per-CPU 寄存器**：次核必须独立调用 `irq_vector_init()`，否则中断产生时 CPU 不知道跳转到哪里。
3. **MPIDR 格式**：PSCI CPU_ON 的 MPIDR 参数**不带 U bit (bit 31)**，CPU1 传 `0x1` 即可（主核 MPIDR=0x80000000，U bit 置位）。

### 3.3 PSCI CPU_ON 实现

```c
#define PSCI_CPU_ON_64  0xC4000003

static int psci_cpu_on(unsigned long mpidr, unsigned long entry, unsigned long ctx)
{
    register unsigned long x0 asm("x0") = PSCI_CPU_ON_64;
    register unsigned long x1 asm("x1") = mpidr;
    register unsigned long x2 asm("x2") = entry;
    register unsigned long x3 asm("x3") = ctx;
    asm volatile("smc #0\n" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    return (int)x0;
}
```

**启动命令：**
```bash
qemu-system-aarch64 -M virt,virtualization=on,gic-version=3,secure=on \
    -cpu max -smp 2 -m 1G -kernel bin_virt/kernel8.img \
    -nographic -serial mon:stdio
```

---

## 4. per-CPU 调度器

### 4.1 数据结构

```c
#define MAX_CPUS  2

struct cpu_sched_state {
    struct task_struct *curr;       /* 当前运行任务 */
    struct task_struct *task[NR_TASKS]; /* 本核任务表 */
    int nr_tasks;                   /* 任务数量 */
    int rr_start;                   /* 轮转起点 */
    struct task_struct init_task;   /* 本核 idle 任务 */
};

extern struct cpu_sched_state cpu_state[MAX_CPUS];

#define current  (cpu_state[smp_processor_id()].curr)

static inline int smp_processor_id(void) {
    unsigned long mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (int)(mpidr & 0xFF);
}
```

### 4.2 命名空间约束

**重要教训：** 宏名不能和结构体字段名 / 函数参数名冲突。
- ❌ `#define current cpu_state[...].current` → 和结构体字段 `current` 冲突
- ❌ `#define task(i) cpu_state[...].task[i]` → 和大量局部变量 `task *p` 冲突
- ✅ `#define current cpu_state[...].curr` → 结构体字段改名为 `curr` 避免冲突
- ✅ 访问任务表直接写 `cpu_state[cpu].task[idx]`，不定义宏

### 4.3 初始化顺序

| 阶段 | 主核 | 次核 |
|------|------|------|
| 调度器预初始化 | sched_init_primary() → 初始化所有 CPU 的 idle task + nr_tasks=1 | - |
| 任务创建 | 创建本核和其他核的任务（通过 create_task_on_cpu()） | - |
| 调度器激活 | current = idle | sched_init_secondary(cpu) → curr = idle |
| 调度循环 | 主循环 disable/schedule/enable | 主循环 disable/schedule/enable |

---

## 5. 虚拟 SMP（多 vCPU VM）

### 5.1 设计思路

一个虚拟多核 VM 包含多个 vCPU，它们共享：
- **同一个 VMID**：Stage 2 TLB 标签相同
- **同一份 Stage 2 页表**：相同的内存视图
- **同一个 virtio 设备模型**：virtio-net/ivshmem 是 VM 级资源
- **同一个 CNTVOFF**：虚拟时间基准一致
- **共享内存**：Guest 内部的全局变量、BSS、heap 等

每个 vCPU 独立拥有：
- **vGIC 状态**：ICH_VMCR、ICH_LR 数组
- **虚拟定时器状态**：CNTV_CTL、CNTV_CVAL
- **系统寄存器**：SCTLR、TTBR0、MPIDR 等
- **vCPU ID**：vcpu_id，决定虚拟 MPIDR 的 aff0
- **绑定的物理 CPU**：phys_cpu，每个 vCPU 固定在一个物理核上

### 5.2 次 vCPU 加载器 (shared_vm_loader)

```
次 vCPU 第一次被调度时：
  prepare_task()
    └─ shared_vm_loader(args)
         ├─ 自旋等待 master->flags & TASK_MM_READY  ← 主 vCPU 完成镜像加载
         ├─ current->mm = master->mm                ← 共享页表
         ├─ virt_attach_secondary(current, master)   ← 共享设备模型 + cntvoff
         │     ├─ board_data = master->board_data    ← 共享 virtio-net/ivshmem 状态
         │     ├─ mm.cntvoff = master->mm.cntvoff    ← 共享时间基准
         │     └─ 独立初始化 vGIC VMCR / vtimer 状态
         └─ regs->pc = entry_point                   ← 同一入口，Guest 自己按 MPIDR 分流
```

**同步机制：**
- 主 vCPU 在 `prepare_task()` 中 loader 返回后置起 `TASK_MM_READY` 标志
- 使用 `dmb ish` 内存屏障保证页表写入先于标志位可见
- 次 vCPU 在 `shared_vm_loader` 中自旋等待该标志
- 两个 vCPU 可能在不同物理核上并行调度，主 vCPU 一定会被调度到（CPU0 上有 idle + 两个 VM），不会死锁

### 5.3 虚拟 MPIDR

Guest 通过读 MPIDR_EL1 判断自己是第几个 vCPU。

**实现方式：** HCR_EL2.TID1=1 使 Guest 读 MPIDR 时陷入 EL2，Hypervisor 返回该 vCPU 的虚拟 MPIDR。

```c
/* 在 task 创建时设置虚拟 MPIDR */
p->cpu_sysregs.mpidr_el1 = (unsigned long)p->vcpu_id;

/* 在 sysreg 陷阱处理中返回 */
DEFINE_SYSREG_MRS(mpidr_el1, 0, 0, 0, 5)  /* S3_0_C0_C0_5 */
```

> **注意**：HCR_EL2.TID1 控制的是 AArch32 ID 寄存器的 trap。对于 AArch64 的 MPIDR_EL1，实际是 TID3 位。
> 当前实现中 Guest 运行在 AArch64 下，但由于 vcpu1 运行在物理 CPU1 上，直接读 MPIDR_EL1 就会得到 aff0=1，
> 恰好能正确区分主次核。如需精确控制虚拟 MPIDR，需设置 HCR_EL2.TID3。

---

## 6. GICv3 多核支持

### 6.1 per-CPU Redistributor

GICv3 的 Redistributor 是 per-CPU 的，每个 CPU 一个：
- Base: `0x080A0000` (GICR_BASE)
- Stride: `0x20000` (128KB per CPU)
- CPUn: `GICR_BASE + n * 0x20000`

每个 Redistributor 包含两个帧：
- **控制帧**（+0x00000）：GICR_CTLR、GICR_WAKER、GICR_TYPER 等
- **SGI 帧**（+0x10000）：GICR_ISENABLER0、GICR_IPRIORITYRn 等（SGI + PPI 配置）

### 6.2 次核初始化

```c
void gic_init_secondary(void)
{
    gicr_init(smp_processor_id());   /* 唤醒本核 Redistributor，配置 PPI 为 Group 1 */
    gic_cpuif_init();                /* 本核 CPU Interface: SRE + PMR + EOImode + IGRPEN1 */
}
```

### 6.3 SGI/PPI 的核归属

SGI（0-15）和 PPI（16-31）是 per-CPU 的，每个核有独立的使能/优先级/挂起状态。

```c
void gic_enable_irq(unsigned int irq)
{
    if (irq < 32) {
        /* SGI/PPI: 操作【当前 CPU】的 Redistributor */
        uint64_t sgi_base = gic_get_redist_base(smp_processor_id()) + GICR_SGI_BASE;
        put32(sgi_base + GICR_ISENABLER0, 1 << irq);
    } else {
        /* SPI: 操作 Distributor（全局） */
        ...
    }
}
```

**注意**：EL2 物理定时器（PPI #26）和虚拟定时器（PPI #27）都是 PPI，每个 CPU 独立使能。次核必须独立调用 `timer_init()` 来启用 PPI #26。

---

## 7. 定时器 per-CPU

### 7.1 per-CPU tick 计数

```c
static volatile unsigned long hyp_tick_count[MAX_CPUS];

void handle_timer_irq(void)
{
    int cpu = smp_processor_id();
    unsigned long interval = get_timer_frequency() / HYP_TIMER_HZ;
    WRITE_SYSREG(interval, CNTHP_TVAL_EL2); /* 写 TVAL 复位中断 */
    hyp_tick_count[cpu]++;

    if (hyp_tick_count[cpu] % HYP_TIMER_HZ == 0)
        printf("[tick %lu] Hypervisor is alive at EL2 (cpu%d)\n",
               hyp_tick_count[cpu], cpu);
    timer_tick();
}
```

每个物理核的 EL2 物理定时器是独立的硬件资源，各核自己初始化、自己处理中断、自己计数。

### 7.2 虚拟定时器（Guest）

虚拟定时器也是 per-CPU 的硬件资源。对于 VM2 的两个 vCPU：
- vcpu0 在 CPU0 上使用 CPU0 的虚拟定时器硬件
- vcpu1 在 CPU1 上使用 CPU1 的虚拟定时器硬件
- 世界切换时各自保存/恢复自己的 CNTV_CTL / CNTV_CVAL
- 共享同一个 CNTVOFF（时间基准一致）

---

## 8. SMP 并发安全

### 8.1 自旋锁实现

使用 ARMv8 独占指令（ldaxr/stxr）实现 ticketless 自旋锁：

```c
static inline void spin_lock(volatile unsigned int *lock)
{
    unsigned int ok;
    asm volatile(
        "1: ldaxr w0, [%1]\n"   /* 带 acquire 语义的独占读 */
        "   cbnz w0, 1b\n"      /* 已锁则自旋 */
        "   mov  w0, #1\n"
        "   stxr %w0, w0, [%1]\n" /* 独占写 1 */
        "   cbnz %w0, 1b\n"      /* 失败重试 */
        : "=&r"(ok) : "r"(lock) : "x0", "memory");
}

static inline void spin_unlock(volatile unsigned int *lock)
{
    asm volatile("stlr wzr, [%0]" ::"r"(lock) : "memory");  /* release 语义解锁 */
}
```

**内存序语义：**
- `ldaxr`：Load-Acquire Exclusive — 后续读写不能重排到它之前
- `stlr`：Store-Release — 之前的读写不能重排到它之后
- 保证锁内的所有内存访问在临界区内完成

### 8.2 页池自旋锁

全局物理页分配器（`allocate_page` / `deallocate_page`）是跨核共享的，必须加锁：

```c
static volatile unsigned int page_pool_lock = 0;

void *allocate_page()
{
    page_pool_lock_acquire();
    /* 从 free list 取页 */
    page_pool_lock_release();
    return page;
}
```

**为什么中断里不需要锁？** 目前中断路径不分配物理页，所以锁内无需关中断。

### 8.3 virtio 设备自旋锁

同一 VM 的多个 vCPU 可能并发访问 virtio 设备（MMIO 读写），需要串行化：

```c
struct virtio_mmio_dev {
    ...
    volatile unsigned int lock;  /* SMP 锁 */
};

uint32_t virtio_mmio_read(struct virtio_mmio_dev *dev, uint32_t offset)
{
    uint32_t val = 0;
    virtio_dev_lock(dev);
    switch (offset) { /* 读寄存器 */ }
    virtio_dev_unlock(dev);
    return val;
}

void virtio_mmio_write(struct virtio_mmio_dev *dev, uint32_t offset, uint32_t val)
{
    virtio_dev_lock(dev);
    switch (offset) { /* 写寄存器 + queue_notify */ }
    virtio_dev_unlock(dev);
}
```

**保护范围：** 所有 MMIO 寄存器访问 + queue_notify（TX/RX 处理）+ 中断状态更新。
**粒度：** 整个设备一把锁（粗粒度），对于 virtio-net 这种低速设备足够。

---

## 9. virtio-net 多描述符链

### 9.1 描述符链结构

vring 的描述符可以通过 `VRING_DESC_F_NEXT` 标志链接成链：

```
avail ring (只写链头 idx)
   │
   ▼
desc[head] → desc[next] → desc[next] → ... → desc[last]
  len=256       len=256       len=256          len=剩余
  flags=NEXT    flags=NEXT    flags=NEXT        flags=0
```

**Guest TX 侧：**
- 按 256B 分片，每片一个描述符
- 链上除了最后一个描述符，都设置 `VRING_DESC_F_NEXT`
- avail ring 只写链头的索引
- 设备侧通过遍历 next 指针读取完整数据包

**Guest RX 侧：**
- 16 个描述符组成 4 条链（每条链 4 个描述符 × 256B = 1024B）
- 每条链的描述符都设置 `VRING_DESC_F_NEXT`（最后一个除外）
- avail ring 写每条链的链头（4 个）
- 接收时遍历链，逐描述符写入，used 只写链头和总长度

### 9.2 TX 处理（设备侧读链）

```c
int read_tx_packet(struct virtio_mmio_dev *dev, uint16_t desc_head,
                   uint8_t *buf, int buf_size)
{
    int total_len = 0;
    uint16_t idx = desc_head;

    while (1) {
        /* 读当前描述符 */
        struct vring_desc desc;
        vq_read_desc(dev, txq, idx, &desc);

        /* 拷贝数据（注意防止溢出） */
        int copy_len = desc.len;
        if (total_len + copy_len > buf_size)
            copy_len = buf_size - total_len;
        vq_copy_from_guest(dev, desc.addr, buf + total_len, copy_len);
        total_len += copy_len;

        /* 如果没有 NEXT 标志，到链尾 */
        if (!(desc.flags & VRING_DESC_F_NEXT))
            break;
        idx = desc.next;
    }
    return total_len;
}
```

**关键点：**
- 从链头开始，通过 `desc.next` 遍历整个链
- 每个描述符的 addr 是 guest 物理地址（IPA），需要映射后读取
- 链尾的标志是 `flags & VRING_DESC_F_NEXT == 0`
- used ring 只标记链头（一个链 = 一个 "used" 条目）

### 9.3 RX 处理（设备侧写链）

```c
int write_rx_packet(struct virtio_mmio_dev *dev, uint8_t *pkt, int pkt_len)
{
    /* 取一条空闲 RX 链的链头 */
    int desc_head = vq_pop_avail_rx_chain(dev, rxq);
    if (desc_head < 0) return -1;  /* 没有空闲描述符 */

    int total_written = 0;
    uint16_t idx = desc_head;

    while (1) {
        struct vring_desc desc;
        vq_read_desc(dev, rxq, idx, &desc);

        int chunk = pkt_len - total_written;
        if (chunk > desc.len) chunk = desc.len;
        vq_copy_to_guest(dev, desc.addr, pkt + total_written, chunk);
        total_written += chunk;

        if (total_written >= pkt_len ||
            !(desc.flags & VRING_DESC_F_NEXT))
            break;
        idx = desc.next;
    }

    /* used ring 只标记链头，len 是包的总长度 */
    vq_add_used(dev, rxq, (uint16_t)desc_head, total_written);
    return total_written;
}
```

**关键点：**
- RX 侧需要管理"描述符链"而不是单个描述符
- avail ring 中每个条目都是一条链的链头
- 写数据时按描述符顺序依次填满，跨描述符自动切换
- used ring 只写链头，len 是整个包的长度

### 9.4 测试用例

| 测试 | 大小 | 描述符数 | 验证点 |
|------|------|---------|--------|
| test1 | 64B | 1 | 单描述符基线 |
| test2 | 1024B | 4 | 多描述符链（256B × 4） |
| test3 | 256B | 1 | 恰好 1 个描述符边界 |
| test4 | 257B | 2 | 跨 2 个描述符（256B + 1B） |

全部 4 项测试在 VM1（单核）和 VM2（虚拟 SMP）上均通过。

---

## 10. 已知限制与未来优化

### 10.1 当前限制

1. **任务不迁移**：任务创建时绑定到固定物理核，不支持动态负载均衡
2. **无核间中断 (SGI)**：目前没有使用 SGI 做核间通知（如 TLB  shootdown）
3. **virtio 锁粒度粗**：整个设备一把锁，高并发场景可能成为瓶颈
4. **UART 输出无锁**：两个核并发写 UART 会导致输出字符交错（不影响功能）
5. **demand paging 并发放缩**：两个 vCPU 同时触发同页的 Stage 2 缺页，虽然页分配有锁，但页表映射可能重复写入（结果相同，无害但低效）
6. **次核启动需 secure=on**：依赖 QEMU 的 EL3 PSCI 实现，非 secure 模式下次核无法启动

### 10.2 未来优化方向

1. **任务迁移**：实现调度器的核间负载均衡
2. **细粒度锁**：virtio 设备按队列加锁（TX 锁 + RX 锁）
3. **SGI 核间通信**：实现 TLB shootdown、调度器 IPI 等
4. **虚拟 PSCI**：Guest 内部通过 PSCI 启动次 vCPU（当前 vcpu1 是 hypervisor 直接启动的）
5. **per-CPU 页池**：每个核有自己的页池子，减少锁竞争

---

## 11. 构建与运行

### 11.1 构建

```bash
cd avisor
make -f Makefile.virt clean && make -f Makefile.virt
```

### 11.2 运行（必须 -smp 2 + secure=on）

```bash
qemu-system-aarch64 \
    -M virt,virtualization=on,gic-version=3,secure=on \
    -cpu max -smp 2 -m 1G \
    -kernel bin_virt/kernel8.img \
    -nographic -serial mon:stdio
```

**退出 QEMU：** Ctrl+A 然后按 X

### 11.3 物理核分配

| 物理核 | 任务 |
|--------|------|
| CPU0 | idle + VM1(vcpu0) + VM2(vcpu0) |
| CPU1 | idle + VM2(vcpu1) |

VM1 是单核 VM（单 vCPU），VM2 是双核 VM（双 vCPU，虚拟 SMP）。

---

## 12. 各阶段对比

| 阶段 | 核心能力 | 关键技术 |
|------|---------|---------|
| Phase 1 | Hypervisor 自身启动到 EL2 | 异常向量、UART、GIC、定时器 |
| Phase 2 | 单 Guest VM 启动 | Stage 2 页表、世界切换、vGIC/vtimer |
| Phase 3 | 多 VM 调度 | 时间片轮转、VMID 切换、TLB 失效 |
| Phase 4 | 设备虚拟化 | MMIO 陷获、virtio 设备模型、ivshmem |
| **Phase 5** | **SMP 多核 + 多描述符链** | **物理多核启动、虚拟 SMP、per-CPU 调度器、SMP 锁、描述符链** |

---

*文档版本：v0.5.0 | 对应代码 tag：phase5-smp-virtio-chain*
