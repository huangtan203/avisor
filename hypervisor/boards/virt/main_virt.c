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
#include "common/printf.h"
#include "common/sched.h"
#include "common/task.h"
#include "common/timer.h"
#include "common/utils.h"

/* virt 平台头文件 */
#include "boards/virt/pl011_uart.h"
#include "boards/virt/gicv3.h"
#include "boards/virt/vgic.h"
#include "boards/virt/arch_timer.h"
#include "boards/virt/guest_loader.h"
#include "boards/virt/base.h"

/* 内嵌 Guest 镜像边界（guest_image.S） */
extern char el1_test_begin[];
extern char el1_test_end[];

/*
 * Hypervisor 主函数
 *
 * 初始化顺序（由低到高依赖）：
 *   1. 串口   2. 调度器数据   3. 异常向量
 *   4. GICv3 + vGIC   5. EL2 物理定时器（调度 tick）
 *   6. HCR_EL2 虚拟化配置   7. 开中断
 *   8. 创建 Guest VM   9. 进入调度主循环
 */
void hypervisor_main()
{
    /* 1. 初始化串口 - 必须最先做，否则 printf 没法输出 */
    uart_init();

    printf("\n");
    printf("========================================\n");
    printf("  aVisor Hypervisor (QEMU virt port)\n");
    printf("  Phase 2: Guest VM bring-up\n");
    printf("========================================\n\n");

    printf("[0] UART initialized (PL011, 115200 baud)\n");

    /* 2. 初始化初始任务（idle task = Hypervisor 自身，pid=0） */
    init_initial_task();
    printf("[1] Initial task set up (pid=0, name=HV)\n");

    /* 3. 设置异常向量表（VBAR_EL2） */
    irq_vector_init();
    printf("[2] Exception vectors installed (VBAR_EL2)\n");

    /* 4. 初始化 GICv3 + GICv3 虚拟化（vGIC） */
    gic_init();
    printf("[3] GICv3 initialized (Distributor + Redistributor + CPU IF)\n");

    vgic_init();
    printf("[4] vGIC enabled (virtual CPU interface, LR injection)\n");

    /* 5. 初始化 EL2 物理定时器（100Hz 调度 tick，PPI #26） */
    timer_init();
    printf("[5] Architected timer initialized (100Hz tick)\n");

    /*
     * 6. HCR_EL2 虚拟化配置（Phase 2 的核心开关）
     *
     * Phase 1 只设了 IMO/FMO/AMO（让 EL2 能收物理中断）；
     * Phase 2 换成完整的 HCR_VALUE，额外打开：
     *   VM=1  Stage 2 地址转换（没有它 Guest 无法拥有独立内存视图）
     *   HCE=1 HVC 指令陷入（半虚拟化接口）
     *   TWI/TWE=1 WFI/WFE 陷入（Guest 睡觉时让出 CPU）
     *   TID/TACR ID 寄存器陷入（多 VM 隔离的基础）
     */
    WRITE_SYSREG64(HCR_VALUE, hcr_el2);
    asm volatile("isb");
    printf("[6] HCR_EL2 = 0x%lx (VM=1, IMO/FMO/AMO=1, HCE=1, TWI=1)\n",
           READ_SYSREG64(hcr_el2));

    /*
     * 清零 CPTR_EL2，解除 EL2 对浮点控制寄存器（FPCR/FPSR）访问的
     * trap。save_sysregs/restore_sysregs 会触碰它们（切换 VM 上下文），
     * 若被 trap 会触发 undefined instruction 异常递归。
     */
    WRITE_SYSREG64(0, cptr_el2);
    asm volatile("isb");

    /* 7. 使能中断控制器 + 打开 PSTATE 中的 I/F 位 */
    enable_interrupt_controller();
    enable_irq();
    printf("[7] Interrupts enabled (IRQ + FIQ)\n");

    /*
     * 8. 创建两个 Guest VM（Phase 3：多 VM）
     *
     * 同一份内嵌镜像创建两个实例：
     *   - 相同的 IPA 布局（0x40000000），但各自的 Stage 2 页表
     *     把 IPA 映射到【不同】的物理页——内存视图完全隔离
     *   - 各自独立的 VMID / CNTVOFF / vGIC 状态 / 虚拟定时器
     *   - Guest 通过 HVC_GET_VMID 知道自己是几号，
     *     vm1 用 10Hz、vm2 用 5Hz 定时器（运行时可观察独立性）
     */
    {
        struct embedded_binary_loader_args guest_args = {
            .load_addr = 0x40000000,
            .entry_point = 0x40000000,
            .image_begin = (unsigned long)el1_test_begin,
            .image_end = (unsigned long)el1_test_end,
        };

        for (int i = 0; i < 2; i++) {
            int pid = create_task(embedded_binary_loader, &guest_args);

            if (pid < 0)
                PANIC("failed to create guest VM #%d", i);

            /*
             * 给 Guest 更高的调度优先级（时间片更长）。
             *
             * 教训：_schedule 在 counter 相同时总是选索引小的任务
             * （init_task 是 0 号），如果 Guest 和 HV 同优先级，
             * HV 的调度循环会反复选自己，Guest 被饿死。
             * （Phase 3 已加轮转指针保证 VM 间公平，
             *   这里仍保持 VM 相对 HV 的高优先级。）
             */
            task[pid]->priority = 10;
            task[pid]->counter = 10;

            printf("[8.%d] Guest VM created (pid=%d, entry=0x40000000, image=%lu bytes)\n",
                   i + 1, pid,
                   (unsigned long)(el1_test_end - el1_test_begin));
        }
    }

    printf("\nAll core systems online. Entering scheduler loop...\n");
    printf("Expect: two guest banners, then interleaved heartbeats\n");
    printf("(vm1 every 1s @ 10Hz timer, vm2 every 1s @ 5Hz timer).\n\n");

    /*
     * 9. 调度主循环（Hypervisor 的"_idle"身份）
     *
     * 这个循环的上下文就是 init_task 的执行流：
     * schedule() 切到 Guest 时，cpu_switch_to 把这里的现场
     * （x19-x30/sp/pc）保存进 init_task.cpu_context；
     * 物理定时器 tick 触发调度换回 HV 时再恢复到这里继续。
     */
    while (1) {
        disable_irq();
        schedule();
        enable_irq();
    }
}
