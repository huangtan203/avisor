// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - QEMU virt 平台主入口 (Phase 1 最小可运行版本)
 *
 * 学习要点：
 *   这是 Hypervisor 的 C 语言入口函数，在 boot.S 里跳转到这里。
 *
 *   Phase 1 的目标是验证最核心的功能：
 *     1. 启动代码正确，能从 EL2 进入 C 函数
 *     2. MMU 开启正常，内存映射正确
 *     3. UART 驱动正常，能打印字符
 *     4. GICv3 中断控制器正常工作
 *     5. 架构定时器能产生周期性中断
 *     6. 调度器基本框架能跑
 *
 *   注意：Phase 1 暂时没有 shell、没有文件系统、没有 guest VM。
 *   这些会在后续 Phase 中逐步添加。
 *   先把"地基"打牢，再往上盖楼。
 */

#include <stddef.h>
#include <stdint.h>

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
#include "boards/virt/arch_timer.h"
#include "boards/virt/base.h"

/*
 * Hypervisor 主函数
 *
 * 初始化顺序非常重要（由低到高依赖）：
 *   1. 串口（最早，否则没法打印调试信息）
 *   2. 初始任务/调度器数据结构
 *   3. 异常向量表
 *   4. 中断控制器（GIC）
 *   5. 定时器（依赖 GIC 来投递中断）
 *   6. 设置 HCR_EL2（IRQ/FIQ/SError 路由到 EL2）
 *   7. 开启中断
 */
void hypervisor_main()
{
    /* 1. 初始化串口 - 必须最先做，否则 printf 没法输出 */
    uart_init();

    printf("\n");
    printf("========================================\n");
    printf("  aVisor Hypervisor (QEMU virt port)\n");
    printf("  Phase 1: Core bring-up\n");
    printf("========================================\n\n");

    printf("[0] UART initialized (PL011, 115200 baud)\n");

    /* 2. 初始化初始任务（idle task / Hypervisor 自身） */
    printf("[1] Initial task set up (pid=0, name=HV)\n");

    /* 3. 设置异常向量表（VBAR_EL2）
     *    把 vectors 的基地址写入 VBAR_EL2 寄存器
     *    之后所有异常都会跳转到 vectors 表中对应的入口 */
    irq_vector_init();
    printf("[2] Exception vectors installed (VBAR_EL2)\n");

    /* 4. 初始化 GICv3 中断控制器
     *    - Distributor: 中断路由、优先级、使能
     *    - Redistributor: PPI/SGI 配置、唤醒 CPU 接口
     *    - CPU Interface: 优先级屏蔽、中断应答/结束 */
    gic_init();
    printf("[3] GICv3 initialized (Distributor + Redistributor + CPU IF)\n");

    /* 5. 初始化架构定时器
     *    EL2 物理定时器，100Hz tick，中断号 PPI #26 */
    timer_init();
    printf("[4] Architected timer initialized (100Hz tick)\n");

    /* 6. 设置 HCR_EL2，使物理 IRQ/FIQ/SError 路由到 EL2
     *
     *    关键要点：
     *    - IMO (bit 4) = 1: 物理 IRQ 异常路由到 EL2
     *    - FMO (bit 3) = 1: 物理 FIQ 异常路由到 EL2
     *    - AMO (bit 5) = 1: 物理 SError 异常路由到 EL2
     *
     *    注意：HCR_EL2.IMO=1 是 EL2 下能收到物理 IRQ 的必要条件。
     *    当 IMO=0 时，即使 DAIF.I=0 且 GIC 中断 pending，
     *    CPU 也不会触发 IRQ 异常（WFI 能被唤醒，但异常向量不跳转）。
     */
    {
        unsigned long hcr;
        asm volatile("mrs %0, hcr_el2" : "=r"(hcr));
        hcr |= (1UL << 4) | (1UL << 3) | (1UL << 5);  /* IMO + FMO + AMO */
        asm volatile("msr hcr_el2, %0" : : "r"(hcr));
        asm volatile("isb");
    }
    printf("[5] HCR_EL2 configured (IMO=1, FMO=1, AMO=1)\n");

    /* 7. 使能中断控制器 + 打开 PSTATE 中的 I/F 位 */
    enable_interrupt_controller();
    enable_irq();
    printf("[6] Interrupts enabled (IRQ + FIQ)\n");

    printf("\nAll core systems online. Entering idle loop...\n");
    printf("(Timer ticks at 100Hz, status prints every second)\n\n");

    /*
     * 主循环：Idle / 调度器
     *
     * 学习要点：
     *   这是 Hypervisor 的主循环。在真正的 OS/Hypervisor 里，
     *   主循环通常是"空闲循环"——没有任务可跑时，
     *   CPU 进入低功耗状态（WFI/WFE）等待中断。
     *
     *   中断来了之后：
     *   1. CPU 跳转到异常向量表的 IRQ 入口
     *   2. 保存上下文，调用 handle_irq()
     *   3. handle_irq() 里判断中断源，调用对应处理函数
     *   4. 定时器中断会调用 timer_tick()，可能触发调度
     *   5. 中断返回，继续执行
     *
     *   目前只有一个任务（idle task = Hypervisor 自身），
     *   所以调度器不会切换任务，但 tick 中断会正常触发。
     */
    while (1) {
        asm volatile("wfi" ::: "memory");
    }
}
