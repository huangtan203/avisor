// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - QEMU virt 平台主入口 (Phase 2: Guest VM 运行)
 *
 * 学习要点：
 *   Phase 1 验证了 Hypervisor 自身在 EL2 的基本能力（串口/GIC/定时器），
 *   Phase 2 在此之上把 Guest VM 跑起来：
 *
 *     1. HCR_EL2 配置为"虚拟化模式"：
 *        - VM=1   : 开启 Stage 2 地址转换（Guest 的 IPA -> PA）
 *        - RW=1   : Guest 是 AArch64
 *        - IMO/FMO/AMO=1 : 物理 IRQ/FIQ/SError 路由到 EL2
 *        - HCE=1  : Guest 的 HVC 指令陷入 EL2（半虚拟化接口）
 *        - TWI/TWE=1 : Guest 的 WFI/WFE 陷入 EL2（借机调度）
 *        - TID/TACR=1 : ID 类寄存器陷入 EL2（每 VM 独立视图）
 *
 *     2. 创建 Guest VM（create_task + 内嵌镜像加载器），
 *        调度器首次切换到该任务时：
 *        switch_from_kthread -> prepare_task -> kernel_exit -> ERET
 *        CPU 从 EL2 进入 EL1，开始执行 Guest。
 *
 *     3. Hypervisor 自身退居"空闲循环"：
 *        disable_irq -> schedule -> enable_irq 循环，
 *        无事可做时被调度器换下，等物理定时器 tick 再唤醒。
 *
 *   Phase 2 的 Guest 服务模型（全半虚拟化/硬件虚拟化，无 MMIO 模拟）：
 *     - 控制台 : HVC 调用（HVC_CONSOLE_PUTC）
 *     - 定时器 : 架构虚拟定时器 CNTV（寄存器直通，CNTVOFF 隔离）
 *     - 中断   : GICv3 虚拟 CPU 接口（ICH_LR 注入，ICC_* 自动重定向）
 */

#include <stddef.h>
#include <stdint.h>

#include "arch/aarch64/sysregs.h"
#include "common/debug.h"
#include "common/irq.h"
#include "common/mm.h"
#include "common/ksm.h"
#include "common/migration.h"
#include "common/platform.h"
#include "common/printf.h"
#include "common/sched.h"
#include "common/smp.h"
#include "common/task.h"
#include "common/timer.h"
#include "common/utils.h"

/* virt 平台头文件 */
#include "boards/virt/pl011_uart.h"
#include "boards/virt/gicv3.h"
#include "boards/virt/vgic.h"
#include "boards/virt/arch_timer.h"
#include "boards/virt/guest_loader.h"
#include "boards/virt/virt_board.h"
#include "boards/virt/base.h"
#include "boards/virt/pci.h"
#include "boards/virt/its.h"
#include "boards/virt/vits.h"
#include "boards/virt/shmem.h"

/*
 * main_virt.c 中创建的任务都在 CPU0 上，用 cpu0_task 快速访问。
 * CPU1 上的任务用 cpu_state[1].task[...] 直接访问。
 */
#define cpu0_tasks (cpu_state[0].task)
#define cpu1_tasks (cpu_state[1].task)

/* 内嵌 Guest 镜像边界（guest_image.S） */
extern char el1_test_begin[];
extern char el1_test_end[];

/* SMP 次核启动标志（次核轮询此变量，等于自己的 CPU ID 则启动）
 * 0 = 等待，1 = CPU1 启动 */
volatile int smp_boot_flag = 0;

/*
 * Loader 参数（全局存储，避免栈上变量被覆盖）
 *
 * 背景：create_task_on_cpu 只是把 arg 指针存到 task 的 cpu_context 里，
 * 不会拷贝内容。如果 arg 是 hypervisor_main 栈上的局部变量，
 * 而任务（尤其是次级 vCPU，需要等待 master 加载完成）延迟执行，
 * 栈上的内容可能已被 hypervisor_main 后续代码覆盖，导致
 * loader 读到错误的 entry_point / master 指针，最终 Guest 从 PC=0 启动
 * 触发 Instruction Abort。
 *
 * 修复：把所有 loader 参数放到全局数据段，生命周期与系统一致。
 */
static struct embedded_binary_loader_args global_guest_args;
static struct shared_vm_loader_args global_vm1_shared_args;
static struct shared_vm_loader_args global_vm2_shared_args;

/*
 * PSCI CPU_ON 函数（通过 SMC #0 调用，由 QEMU 模拟的 PSCI 固件处理）
 *
 * 返回 0 = 成功，其他 = 错误码
 *
 * 注意：
 *   - ARM PSCI 标准使用 SMC 作为调用约定（SMC32/SMC64）
 *   - QEMU 的 TCG 模拟会捕获 SMC 指令并处理 PSCI 调用
 *   - 不能用 HVC #0，因为 hypervisor 本身运行在 EL2，
 *     EL2 执行 HVC 会触发未处理的同步异常（SYNC_INVALID_EL2）
 */
#define PSCI_CPU_ON_64        0xC4000003

static int psci_cpu_on(unsigned long mpidr, unsigned long entry_point,
                       unsigned long context_id)
{
    register unsigned long x0 asm("x0") = PSCI_CPU_ON_64;
    register unsigned long x1 asm("x1") = mpidr;
    register unsigned long x2 asm("x2") = entry_point;
    register unsigned long x3 asm("x3") = context_id;

    asm volatile(
        "smc #0\n"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3)
        : "memory");

    return (int)x0;
}

/*
 * 次核的 PSCI 入口点（由 PSCI CPU_ON 在 EL2 跳入）
 * 定义在 boot_virt.S 中
 */
extern void secondary_psci_entry(void);

/*
 * Hypervisor 主函数（主核 CPU0）
 *
 * 初始化顺序（由低到高依赖）：
 *   1. 串口   2. 调度器(per-CPU)   3. 异常向量
 *   4. GICv3 + vGIC   5. EL2 物理定时器（调度 tick）
 *   6. HCR_EL2 虚拟化配置   7. 开中断
 *   8. 创建 Guest VM（SMP 任务分配）
 *   9. 启动次核
 *   10. 进入调度主循环
 */
void hypervisor_main()
{
    /* 1. 先用 QEMU 兼容基址开启串口，保证探针可以输出诊断。 */
    uart_init();
    /* 2. 探测平台并覆盖有效 DTB 提供的运行时基址。 */
    platform_early_probe();
    uart_init();

    printf("\n");
    printf("========================================\n");
    printf("  aVisor Hypervisor (QEMU virt port)\n");
    printf("  Phase 2: Guest VM bring-up (SMP)\n");
    printf("========================================\n\n");

    printf("[0] UART initialized (PL011, 115200 baud)\n");
    printf("[cpu0] booting on CPU%d (primary)\n", smp_processor_id());

    /* 2. 初始化 per-CPU 调度器（idle task = Hypervisor 自身） */
    sched_init_primary();
    init_initial_task();
    printf("[1] CPU0 scheduler initialized (idle task, name=HV)\n");

    /* 3. 设置异常向量表（VBAR_EL2） */
    irq_vector_init();
    printf("[2] Exception vectors installed (VBAR_EL2)\n");

    /* 4. 初始化 GICv3 + GICv3 虚拟化（vGIC） */
    gic_init();
    printf("[3] GICv3 initialized (Distributor + Redistributor + CPU IF)\n");

    vgic_init();
    printf("[4] vGIC enabled (virtual CPU interface, LR injection)\n");

    /* 4.25 初始化 ITS（GICv3 Interrupt Translation Service）
     * 必须在 GIC 初始化后、PCI 初始化前调用，
     * 因为 pci_init 里会调用 its_setup_pci_msi 建立 MSI→LPI 映射。 */
    its_init();
    printf("[4.25] ITS initialized (GICv3 MSI controller)\n");

    /* 4.3 初始化 vITS 全局状态（物理 ITS 就绪后、PCI 初始化前） */
    vits_global_init();
    printf("[4.3] vITS global initialized (virtual MSI controller)\n");

    /* 4.4 初始化跨 VM 共享内存子系统 */
    shmem_init();
    printf("[4.4] shared memory initialized (cross-VM shm)\n");

    /* 4.45 初始化 KSM 内存去重子系统 */
    ksm_init();
    printf("[4.45] KSM initialized (memory deduplication)\n");

    /* 4.5 初始化 PCI 直通（必须在创建 VM 之前，使 virt_initialize
     * 调用 pci_passthrough_map 时 edu 已枚举完成） */
    pci_init();
    printf("[4.5] PCI passthrough initialized (edu device)\n");

    /* 5. 初始化 EL2 物理定时器（100Hz 调度 tick，PPI #26） */
    timer_init();
    printf("[5] Architected Timer frequency: %lu Hz (%d Hz tick)\n",
           get_timer_frequency(), HYP_TIMER_HZ);

    /*
     * 6. HCR_EL2 虚拟化配置（Phase 2 的核心开关）
     *
     * TID1=1 使 MPIDR_EL1 等 ID 寄存器读陷阱——虚拟 SMP：
     * 每个 vCPU 返回不同的 MPIDR，Guest 借此区分主/次 vCPU。
     */
    WRITE_SYSREG64(HCR_VALUE, hcr_el2);
    asm volatile("isb");
    printf("[6] HCR_EL2 = 0x%lx (VM=1, IMO/FMO/AMO=1, HCE=1, TWI=1, TID1=1)\n",
           READ_SYSREG64(hcr_el2));

    /*
     * 清零 CPTR_EL2，解除 EL2 对浮点控制寄存器的 trap */
    WRITE_SYSREG64(0, cptr_el2);
    asm volatile("isb");

    /* 7. 使能中断控制器 + 打开 PSTATE 中的 I/F 位 */
    enable_interrupt_controller();
    enable_irq();
    printf("[7] Interrupts enabled (IRQ + FIQ)\n");

    /* 主核就绪（用于核间通信握手；主核不发给自己，此处仅标记） */
    asm volatile("dmb ish" ::: "memory");
    smp_online[0] = 1;

    /*
     * 8. 创建 Guest VM（SMP 任务分配）
     *
     * Phase 11 虚拟 SMP 配置：
     *   CPU0：VM1_vcpu0 + VM1_vcpu1 + VM2_vcpu0 + idle（3 个 vCPU + idle）
     *
     * VM1（vmid=1）：双核，两个 vCPU 都在 CPU0（同核 vCPU 调度演示）
     * VM2（vmid=2）：单核，vcpu0 在 CPU0（跨核 vCPU 演示暂未启用）
     *
     * 同一 VM 的 vCPU 共享同一份 Stage 2 页表和设备模型。
     */
    {
        global_guest_args = (struct embedded_binary_loader_args){
            .load_addr = 0x40000000,
            .entry_point = 0x40000000,
            .image_begin = (unsigned long)el1_test_begin,
            .image_end = (unsigned long)el1_test_end,
        };

        /* VM1 主 vCPU：创建在 CPU0，vmid=1, vcpu0 */
        struct task_struct *vm1_master = NULL;
        {
            int pid = create_task_on_cpu(0, embedded_binary_loader, &global_guest_args);
            if (pid < 0)
                PANIC("failed to create VM1 vcpu0");
            cpu0_tasks[pid]->vmid = 1;
            cpu0_tasks[pid]->vcpu_id = 0;
            cpu0_tasks[pid]->phys_cpu = 0;
            cpu0_tasks[pid]->cpu_sysregs.mpidr_el1 = 0;
            /* 给 Guest 更高调度优先级 */
            cpu0_tasks[pid]->priority = 10;
            cpu0_tasks[pid]->counter = 10;
            (void)strncpy(cpu0_tasks[pid]->name, "vm1-cpu0", 36);
            vm1_master = cpu0_tasks[pid];
            printf("[8.1] VM1 vcpu0 created (vmid=1, cpu0, pid=%d)\n", pid);
            /* Phase 12: 注册 vCPU 到板级查找表（用于 vSGI 等跨 vCPU 操作） */
            virt_register_vcpu(cpu0_tasks[pid], 1, 0);
        }

        /* VM1 次 vCPU：创建在 CPU1（跨物理核 vCPU 调度，验证跨核 IPI + vSGI） */
        {
            global_vm1_shared_args = (struct shared_vm_loader_args){
                .master = vm1_master,
                .entry_point = 0x40000000,
            };
            int pid = create_task_on_cpu(1, shared_vm_loader, &global_vm1_shared_args);
            if (pid < 0)
                PANIC("failed to create VM1 vcpu1");
            cpu1_tasks[pid]->vmid = 1;
            cpu1_tasks[pid]->vcpu_id = 1;
            cpu1_tasks[pid]->phys_cpu = 1;
            /* 同步虚拟 MPIDR（Guest 读 mpidr_el1 时返回这个值）
             * 注意：create_task_on_cpu 内部已经用 vcpu_id=0 初始化了 mpidr，
             * 这里改了 vcpu_id 必须同步改 mpidr，否则 Guest 读到的还是 0。 */
            cpu1_tasks[pid]->cpu_sysregs.mpidr_el1 = 1;
            cpu1_tasks[pid]->priority = 10;
            cpu1_tasks[pid]->counter = 10;
            (void)strncpy(cpu1_tasks[pid]->name, "vm1-cpu1", 36);
            printf("[8.2] VM1 vcpu1 created (vmid=1, cpu1, pid=%d)\n", pid);
            /* Phase 12: 注册 vCPU 到板级查找表 */
            virt_register_vcpu(cpu1_tasks[pid], 1, 1);
        }

        /* VM2 主 vCPU：创建在 CPU0，vmid=2, vcpu0 */
        struct task_struct *vm2_master = NULL;
        {
            int pid = create_task_on_cpu(0, embedded_binary_loader, &global_guest_args);
            if (pid < 0)
                PANIC("failed to create VM2 vcpu0");
            cpu0_tasks[pid]->vmid = 2;
            cpu0_tasks[pid]->vcpu_id = 0;
            cpu0_tasks[pid]->phys_cpu = 0;
            cpu0_tasks[pid]->cpu_sysregs.mpidr_el1 = 0;
            cpu0_tasks[pid]->priority = 10;
            cpu0_tasks[pid]->counter = 10;
            (void)strncpy(cpu0_tasks[pid]->name, "vm2-cpu0", 36);
            vm2_master = cpu0_tasks[pid];

            /* 修正 ivshmem peer_id：
             * create_task_on_cpu 里用自动分配的 vmid 初始化了
             * ivshmem（peer_id = vmid_initial - 1），但我们
             * 已经把 vmid 改成了 2，所以 peer_id 应该是 1。 */
            virt_board_set_ivshmem_peer(vm2_master, 1);

            /* Phase 12: 注册 vCPU 到板级查找表 */
            virt_register_vcpu(vm2_master, 2, 0);

            printf("[8.3] VM2 vcpu0 created (vmid=2, cpu0, pid=%d)\n", pid);
        }

        /* VM2 次 vCPU：创建在 CPU1（Phase 14 双 VM 全 SMP，跨核 vCPU） */
        {
            global_vm2_shared_args = (struct shared_vm_loader_args){
                .master = vm2_master,
                .entry_point = 0x40000000,
            };
            int pid = create_task_on_cpu(1, shared_vm_loader, &global_vm2_shared_args);
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

        /* Phase 25: 创建 dormant 迁移目标 VM3（vmid=3，单 vCPU，等待迁移恢复） */
        {
            int pid = create_task_on_cpu(0, migration_resume_loader, NULL);
            if (pid < 0)
                PANIC("failed to create VM3 (migration target)");
            cpu0_tasks[pid]->vmid = 3;
            cpu0_tasks[pid]->vcpu_id = 0;
            cpu0_tasks[pid]->phys_cpu = 0;
            cpu0_tasks[pid]->cpu_sysregs.mpidr_el1 = 0;
            cpu0_tasks[pid]->priority = 10;
            cpu0_tasks[pid]->counter = 10;
            cpu0_tasks[pid]->state = TASK_WAITING;  /* dormant，直到迁移 FINISH 唤醒 */
            (void)strncpy(cpu0_tasks[pid]->name, "vm3-cpu0", 36);
            virt_register_vcpu(cpu0_tasks[pid], 3, 0);
            migration_set_dest(cpu0_tasks[pid]);
            printf("[8.5] VM3 vcpu0 created (vmid=3, migration target, pid=%d)\n", pid);
        }

        /*
         * 最后修正：确保 VM1 的 ivshmem peer 0 正确指向主 vCPU。
         *
         * 因为创建顺序是 VM1-vcpu0 → VM1-vcpu1 → VM2-vcpu0 → VM2-vcpu1，
         * 中间步骤里 peer 0 会被后续临时初始化覆盖。
         * 这里手动恢复 VM1 的 peer 0，保证 doorbell 路由正确。
         */
        virt_board_set_ivshmem_peer(vm1_master, 0);
    }

    /*
     * 9. 启动次核 CPU1
     *
     * QEMU virt 平台上，次核默认处于关闭状态，需要通过 PSCI CPU_ON 唤醒。
     * 次核启动后进入 EL2，从 secondary_psci_entry 开始执行。
     *
     * 注意：
     *  - MPIDR 参数不能带 U bit (bit 31)，直接用 aff0 = 1 即可
     *  - 必须用 -machine virt,secure=on 才能通过 SMC 调用 PSCI
     *  - 次核启动后先等待 smp_boot_flag==1，确保主核初始化完成
     */
    {
        int ret = psci_cpu_on(0x1, (unsigned long)secondary_psci_entry, 0);
        printf("[9] SMP: PSCI CPU_ON(cpu1) returned %d\n", ret);
        if (ret != 0) {
            printf("    WARNING: Failed to boot CPU1!\n");
        } else {
            /* 次核启动成功，置起标志让它继续初始化 */
            smp_boot_flag = 1;
            asm volatile("sev" ::: "memory");
        }
    }

    /*
     * 等待次核就绪（smp_online[1] == 1），再做核间通信测试。
     * 次核在 hypervisor_secondary_main 开中断后置起该标志。
     * 用 wfe 等待：次核置标志后 sev 唤醒；即便 sev 丢失，主核
     * 自身的 100Hz 定时器 tick 也会周期性唤醒它重新检查。
     */
    while (!smp_online[1])
        asm volatile("wfe" ::: "memory");
    asm volatile("dmb ish" ::: "memory");

    printf("\n[cpu0] CPU1 is online, testing SGI IPI...\n");
    printf("[cpu0] IPI: sending SGI#%d (ping) to CPU1\n", SGI_IPI_TEST);
    smp_send_ipi(1, SGI_IPI_TEST);

    printf("\nAll core systems online. Entering scheduler loop...\n");
    printf("CPU0: idle + VM1 vcpu0 + VM2 vcpu0\n");
    printf("CPU1: idle + VM1 vcpu1 + VM2 vcpu1\n\n");

    /*
     * 10. 调度主循环（Hypervisor 的"_idle"身份）
     */
    while (1) {
        disable_irq();
        schedule();
        enable_irq();
    }
}

/*
 * 次核入口（CPU1 从 boot.S 跳转到这里）
 *
 * 次核需要初始化自己的：
 *   - GIC Redistributor + CPU Interface
 *   - EL2 物理定时器
 *   - per-CPU 调度器
 *   - vGIC CPU 接口（系统寄存器是 per-CPU 的，自动有各自的 LR
 */
void hypervisor_secondary_main(void)
{
    int cpu = smp_processor_id();

    /* uart_init() PL011 是共享外设，主核已初始化过，次核直接用 */
    printf("[cpu%d] secondary CPU booted\n", cpu);

    /* 初始化 per-CPU 调度器 */
    sched_init_secondary(cpu);

    /* 异常向量表（VBAR_EL2 是 per-CPU 寄存器，各自设置） */
    irq_vector_init();
    printf("[cpu%d] Exception vectors installed (VBAR_EL2)\n", cpu);

    /* 次核自己的 GIC（Distributor 已由主核初始化过了 */
    gic_init_secondary();
    printf("[cpu%d] GICv3 initialized\n", cpu);

    /* vGIC CPU interface 是 per-CPU 系统寄存器，独立开启 */
    vgic_init();
    printf("[cpu%d] vGIC enabled\n", cpu);

    /* 次核自己的 EL2 物理定时器 */
    timer_init();
    printf("[cpu%d] Timer initialized (%lu Hz)\n",
           cpu, get_timer_frequency());

    /* HCR_EL2 是 per-CPU 的，各自独立配置 */
    WRITE_SYSREG64(HCR_VALUE, hcr_el2);
    asm volatile("isb");

    WRITE_SYSREG64(0, cptr_el2);
    asm volatile("isb");

    /* 开中断 */
    enable_interrupt_controller();
    enable_irq();
    printf("[cpu%d] Interrupts enabled\n", cpu);

    /* 标记本核就绪，唤醒主核（主核在 wfe 等待 smp_online[cpu]） */
    asm volatile("dmb ish" ::: "memory");
    smp_online[cpu] = 1;
    asm volatile("sev" ::: "memory");

    printf("[cpu%d] Entering scheduler loop...\n", cpu);

    /* 次核调度循环 */
    while (1) {
        disable_irq();
        schedule();
        enable_irq();
    }
}
