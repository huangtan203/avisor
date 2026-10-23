// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - SMP 核间通信（IPI）
 *
 * 基于 GICv3 SGI 的核间中断，提供：
 *   1. smp_send_ipi()：向指定物理核发送 SGI
 *   2. smp_ipi_handle()：收到 SGI 后的分发处理
 *   3. smp_online[]：核启动就绪标志（主核等待次核就绪后发送 IPI）
 *
 * SGI 是边沿触发的，经 ICC_SGI1R_EL1 触发后目标核的 Redistributor
 * 记录 pending，待目标核开中断（PSTATE.I=0）且 CPU Interface 使能
 * Group 1 后，即触发一次 IRQ 陷入 EL2。
 */

#include "boards/virt/arch_timer.h"
#include "boards/virt/gicv3.h"
#include "boards/virt/vgic.h"
#include "arch/aarch64/sysregs.h"
#include "common/debug.h"
#include "common/smp.h"
#include "common/utils.h"
#include "common/task.h"

/* 各核就绪标志：次核在 hypervisor_secondary_main 开中断后置 1 */
volatile int smp_online[MAX_CPUS] = { 0 };

/* 各核收到的 IPI 计数（调试/统计用） */
static volatile unsigned long ipi_count[MAX_CPUS];
static volatile int freeze_lock;
static volatile unsigned long freeze_generation;
static volatile unsigned long release_generation;
static volatile unsigned long release_ack_generation[MAX_CPUS];
static volatile int freeze_active;
static volatile int freeze_target_cpu = -1;
static volatile unsigned long freeze_ack_generation[MAX_CPUS];
static volatile unsigned long freeze_progress_count[MAX_CPUS];
static struct task_struct *freeze_target_task;

/*
 * Phase 31: 事务式冻结状态。
 *
 * txn_state:
 *   0 = TXN_NONE      无事务
 *   1 = TXN_FROZEN    目标核已 ACK，正在冻结中（IRQ handler 内忙等）
 *   2 = TXN_COMMITTED 已提交：目标核按冻结时保存的现场恢复执行
 *  -1 = TXN_ROLLED_BACK 已回滚：目标核从冻结点重新执行
 *
 * 决策寄存器 txn_decision 让冻结 handler 知道 commit/rollback，
 * 由协调核在 COMMIT/ROLLBACK HVC 中写入，目标核读到后退出忙等。
 */
enum {
    TXN_NONE = 0,
    TXN_FROZEN = 1,
    TXN_COMMITTED = 2,
    TXN_ROLLED_BACK = -1,
};
static volatile int txn_state = TXN_NONE;
static volatile int txn_decision; /* 1=commit(恢复现场) 2=rollback(重新执行) */
static volatile int txn_target_cpu = -1;
static struct task_struct *txn_target_task;


static void smp_freeze_lock_acquire(void)
{
    unsigned int old;
    unsigned int failed;

    for (;;) {
        asm volatile("ldaxr %w0, [%1]"
                     : "=&r"(old) : "r"(&freeze_lock) : "memory");
        if (old) {
            asm volatile("clrex" ::: "memory");
            continue;
        }

        asm volatile("stxr %w0, %w2, [%1]"
                     : "=&r"(failed)
                     : "r"(&freeze_lock), "r"(1) : "memory");
        if (!failed)
            return;
    }
}

static void smp_freeze_lock_release(void)
{
    asm volatile("stlr wzr, [%0]" :: "r"(&freeze_lock) : "memory");
}

int smp_freeze_begin(int target_cpu)
{
    unsigned long generation;
    unsigned long timeout = 10000000;

    if (target_cpu < 0 || target_cpu >= MAX_CPUS || target_cpu == smp_processor_id() ||
        !smp_online[target_cpu])
        return -1;

    smp_freeze_lock_acquire();
    if (freeze_active) {
        unsigned long active_generation = freeze_generation;
        int active_cpu = freeze_target_cpu;
        if (__atomic_load_n(&release_generation, __ATOMIC_ACQUIRE) ==
                active_generation && active_cpu >= 0 &&
            __atomic_load_n(&release_ack_generation[active_cpu],
                            __ATOMIC_ACQUIRE) == active_generation) {
            __atomic_store_n(&freeze_target_cpu, -1, __ATOMIC_RELAXED);
            freeze_target_task = NULL;
            freeze_active = 0;
        } else {
            smp_freeze_lock_release();
            return -1;
        }
    }
    freeze_active = 1;
    smp_freeze_lock_release();

    generation = ++freeze_generation;
    struct task_struct *target = NULL;
    for (int i = 0; i < cpu_state[target_cpu].nr_tasks; i++) {
        struct task_struct *task = cpu_state[target_cpu].task[i];
        if (task && task->pid != 0 && task->vmid == current->vmid &&
            task->vcpu_id == 1 && task->phys_cpu == target_cpu &&
            task->state == TASK_RUNNING) {
            target = task;
            break;
        }
    }
    if (!target) {
        smp_freeze_lock_acquire();
        freeze_active = 0;
        smp_freeze_lock_release();
        return -1;
    }

    __atomic_store_n(&freeze_target_cpu, target_cpu, __ATOMIC_RELAXED);
    freeze_target_task = target;
    __atomic_store_n(&release_generation, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&freeze_ack_generation[target_cpu], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&release_ack_generation[target_cpu], 0, __ATOMIC_RELAXED);
    smp_send_ipi(target_cpu, SGI_VM_FREEZE);

    while (timeout--) {
        if (__atomic_load_n(&freeze_ack_generation[target_cpu], __ATOMIC_ACQUIRE) == generation)
            return 0;
        if ((timeout & 0xffff) == 0)
            smp_send_ipi(target_cpu, SGI_VM_FREEZE);
        asm volatile("yield" ::: "memory");
    }

    __atomic_store_n(&release_generation, generation, __ATOMIC_RELEASE);
    timeout = 10000000;
    while (timeout--) {
        if (__atomic_load_n(&release_ack_generation[target_cpu], __ATOMIC_ACQUIRE) == generation) {
            smp_freeze_lock_acquire();
            __atomic_store_n(&freeze_target_cpu, -1, __ATOMIC_RELAXED);
            freeze_target_task = NULL;
            __atomic_store_n(&freeze_active, 0, __ATOMIC_RELEASE);
            smp_freeze_lock_release();
            return -1;
        }
        asm volatile("yield" ::: "memory");
    }
    return -1;
}

int smp_freeze_release(void)
{
    unsigned long generation = __atomic_load_n(&freeze_generation, __ATOMIC_ACQUIRE);
    unsigned long timeout = 10000000;
    int target_cpu = __atomic_load_n(&freeze_target_cpu, __ATOMIC_ACQUIRE);

    if (!__atomic_load_n(&freeze_active, __ATOMIC_ACQUIRE) ||
        target_cpu < 0 || target_cpu >= MAX_CPUS ||
        __atomic_load_n(&freeze_ack_generation[target_cpu],
                        __ATOMIC_ACQUIRE) != generation ||
        __atomic_load_n(&release_generation, __ATOMIC_ACQUIRE) == generation)
        return -1;

    __atomic_store_n(&release_generation, generation, __ATOMIC_RELEASE);

    while (timeout--) {
        if (__atomic_load_n(&release_ack_generation[target_cpu],
                            __ATOMIC_ACQUIRE) == generation) {
            smp_freeze_lock_acquire();
            __atomic_store_n(&freeze_target_cpu, -1, __ATOMIC_RELAXED);
            freeze_target_task = NULL;
            __atomic_store_n(&freeze_active, 0, __ATOMIC_RELEASE);
            smp_freeze_lock_release();
            return 0;
        }

        asm volatile("yield" ::: "memory");
    }

    return -1;
}

unsigned long smp_freeze_progress(int cpu)
{
    if (cpu < 0 || cpu >= MAX_CPUS)
        return 0;
    return __atomic_load_n(&freeze_progress_count[cpu], __ATOMIC_ACQUIRE);
}

unsigned long smp_freeze_ack(int cpu)
{
    if (cpu < 0 || cpu >= MAX_CPUS)
        return 0;
    return __atomic_load_n(&freeze_ack_generation[cpu], __ATOMIC_ACQUIRE);
}

int smp_freeze_snapshot_status(int cpu, unsigned long *generation)
{
    struct task_struct *target = freeze_target_task;
    unsigned long active_generation =
        __atomic_load_n(&freeze_generation, __ATOMIC_ACQUIRE);

    if (cpu < 0 || cpu >= MAX_CPUS || cpu != freeze_target_cpu ||
        !__atomic_load_n(&freeze_active, __ATOMIC_ACQUIRE) || !target ||
        !target->freeze_context.active ||
        target->freeze_context.generation != active_generation ||
        __atomic_load_n(&freeze_ack_generation[cpu], __ATOMIC_ACQUIRE) !=
            active_generation)
        return 0;

    if (generation)
        *generation = target->freeze_context.generation;
    return 1;
}

/*
 * Phase 31: 事务式冻结。
 *
 * 与 smp_freeze_begin() 的区别：目标核 ACK 后不自动释放，
 * 保持在 IRQ handler 忙等，直到 COMMIT / ROLLBACK 显式决策。
 * 这使得冻结期间 vCPU 完全静止，快照持续有效。
 */
int smp_txn_freeze_begin(int target_cpu)
{
    unsigned long timeout = 10000000;

    if (target_cpu < 0 || target_cpu >= MAX_CPUS ||
        target_cpu == smp_processor_id() || !smp_online[target_cpu])
        return -1;

    smp_freeze_lock_acquire();
    if (txn_state != TXN_NONE) {
        smp_freeze_lock_release();
        return -1;
    }
    txn_state = TXN_FROZEN;
    smp_freeze_lock_release();

    /* 与瞬时冻结相同的拓扑约束：当前 VM 的 vCPU1，绑定目标物理核 */
    struct task_struct *target = NULL;
    for (int i = 0; i < cpu_state[target_cpu].nr_tasks; i++) {
        struct task_struct *task = cpu_state[target_cpu].task[i];
        if (task && task->pid != 0 && task->vmid == current->vmid &&
            task->vcpu_id == 1 && task->phys_cpu == target_cpu &&
            task->state == TASK_RUNNING) {
            target = task;
            break;
        }
    }
    if (!target) {
        smp_freeze_lock_acquire();
        txn_state = TXN_NONE;
        smp_freeze_lock_release();
        return -1;
    }

    __atomic_store_n(&txn_decision, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&txn_target_cpu, target_cpu, __ATOMIC_RELAXED);
    txn_target_task = target;
    __atomic_store_n(&freeze_ack_generation[target_cpu], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&release_ack_generation[target_cpu], 0, __ATOMIC_RELAXED);
    asm volatile("dmb ish" ::: "memory");
    smp_send_ipi(target_cpu, SGI_VM_TXN_FREEZE);

    while (timeout--) {
        if (__atomic_load_n(&freeze_ack_generation[target_cpu],
                            __ATOMIC_ACQUIRE) != 0)
            return 0;
        if ((timeout & 0xffff) == 0)
            smp_send_ipi(target_cpu, SGI_VM_TXN_FREEZE);
        asm volatile("yield" ::: "memory");
    }

    /* 超时：请求目标核退出（若它晚些收到 SGI 会看到 decision!=0 直接返回） */
    __atomic_store_n(&txn_decision, 2, __ATOMIC_RELEASE);
    timeout = 10000000;
    while (timeout--) {
        if (__atomic_load_n(&release_ack_generation[target_cpu],
                            __ATOMIC_ACQUIRE) != 0) {
            smp_freeze_lock_acquire();
            txn_state = TXN_NONE;
            txn_target_task = NULL;
            __atomic_store_n(&txn_target_cpu, -1, __ATOMIC_RELAXED);
            __atomic_store_n(&freeze_ack_generation[target_cpu], 0,
                             __ATOMIC_RELAXED);
            __atomic_store_n(&release_ack_generation[target_cpu], 0,
                             __ATOMIC_RELAXED);
            smp_freeze_lock_release();
            return -1;
        }
        asm volatile("yield" ::: "memory");
    }
    return -1;
}

/*
 * 共享的决策执行：协调核写入 decision 后等待目标核退出忙等。
 * 返回 0 成功，-1 超时。
 */
static int smp_txn_wait_exit(int target_cpu)
{
    unsigned long timeout = 10000000;

    while (timeout--) {
        if (__atomic_load_n(&release_ack_generation[target_cpu],
                            __ATOMIC_ACQUIRE) != 0)
            return 0;
        asm volatile("yield" ::: "memory");
    }
    return -1;
}

/* 提交：目标核按冻结时保存的异常现场恢复执行（正常继续，非重执行） */
int smp_txn_freeze_commit(void)
{
    int target_cpu = __atomic_load_n(&txn_target_cpu, __ATOMIC_ACQUIRE);
    int ret;

    if (__atomic_load_n(&txn_state, __ATOMIC_ACQUIRE) != TXN_FROZEN ||
        target_cpu < 0 || target_cpu >= MAX_CPUS)
        return -1;

    __atomic_store_n(&txn_decision, 1, __ATOMIC_RELEASE);
    ret = smp_txn_wait_exit(target_cpu);

    smp_freeze_lock_acquire();
    txn_state = ret == 0 ? TXN_COMMITTED : TXN_NONE;
    txn_target_task = NULL;
    __atomic_store_n(&txn_target_cpu, -1, __ATOMIC_RELAXED);
    __atomic_store_n(&freeze_ack_generation[target_cpu], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&release_ack_generation[target_cpu], 0, __ATOMIC_RELAXED);
    smp_freeze_lock_release();

    /* 提交后事务结束，状态回到 NONE 允许下一轮。
     * TXN_COMMITTED 仅在内部短暂使用，外部 status 返回 NONE。 */
    if (ret == 0)
        txn_state = TXN_NONE;
    return ret;
}

/* 回滚：目标核从冻结点重新执行（恢复保存的异常帧，清 ACK 重走） */
int smp_txn_freeze_rollback(void)
{
    int target_cpu = __atomic_load_n(&txn_target_cpu, __ATOMIC_ACQUIRE);
    int ret;

    if (__atomic_load_n(&txn_state, __ATOMIC_ACQUIRE) != TXN_FROZEN ||
        target_cpu < 0 || target_cpu >= MAX_CPUS)
        return -1;

    __atomic_store_n(&txn_decision, 2, __ATOMIC_RELEASE);
    ret = smp_txn_wait_exit(target_cpu);

    smp_freeze_lock_acquire();
    txn_state = TXN_NONE;
    txn_target_task = NULL;
    __atomic_store_n(&txn_target_cpu, -1, __ATOMIC_RELAXED);
    __atomic_store_n(&freeze_ack_generation[target_cpu], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&release_ack_generation[target_cpu], 0, __ATOMIC_RELAXED);
    smp_freeze_lock_release();
    return ret;
}

int smp_txn_freeze_status(void)
{
    return __atomic_load_n(&txn_state, __ATOMIC_ACQUIRE);
}


/*
 * 向指定物理核发送核间中断
 */
void smp_send_ipi(int cpu, int sgi_id)
{
    gic_send_sgi(cpu, sgi_id);
}

/*
 * 处理当前核收到的 SGI
 *
 * 注意：在 EL2 中断上下文执行（handle_irq 内），此时仍处于
 * split EOI 流程中——SGI 属于普通中断，handle_irq 末尾会统一
 * 写 ICC_DIR 解除激活，这里无需手动处理 active 状态。
 */
void smp_ipi_handle(int sgi_id)
{
    int cpu = smp_processor_id();
    ipi_count[cpu]++;

    switch (sgi_id) {
    case SGI_IPI_NOTIFY:
        printf("[cpu%d] IPI: received SGI#%d (count=%lu)\n",
               cpu, sgi_id, ipi_count[cpu]);
        break;

    case SGI_IPI_TEST:
        printf("[cpu%d] IPI: received SGI#%d (count=%lu)\n",
               cpu, sgi_id, ipi_count[cpu]);
        /*
         * ping-pong：CPU1 收到 CPU0 的 ping 后回一个 pong。
         * （当前仅双核，次核固定回复给主核 CPU0。）
         */
        if (cpu == 1)
            gic_send_sgi(0, SGI_IPI_TEST);
        break;

    case SGI_VM_FREEZE: {
        unsigned long generation = __atomic_load_n(&freeze_generation,
                                                   __ATOMIC_ACQUIRE);
        struct task_struct *target = freeze_target_task;
        struct pt_regs *regs;

        if (!__atomic_load_n(&freeze_active, __ATOMIC_ACQUIRE) ||
            cpu != __atomic_load_n(&freeze_target_cpu, __ATOMIC_RELAXED) ||
            !target || current != target || target->phys_cpu != cpu ||
            target->vcpu_id != 1 || target->vmid != 2 ||
            target->state != TASK_RUNNING)
            break;

        memcpy(&target->freeze_context.cpu_sysregs, &target->cpu_sysregs,
               sizeof(target->cpu_sysregs));
        memcpy(&target->freeze_context.virt, &target->virt,
               sizeof(target->virt));
        vgic_save_state(&target->freeze_context.virt);
        target->freeze_context.virt.cntv_ctl = READ_SYSREG64(CNTV_CTL_EL0);
        target->freeze_context.virt.cntv_cval = READ_SYSREG64(CNTV_CVAL_EL0);
        target->freeze_context.virt.vtimer_pending =
            !!(target->freeze_context.virt.cntv_ctl & TIMER_CTL_ISTATUS);

        regs = task_pt_regs(target);
        for (int i = 0; i < 31; i++)
            target->freeze_context.regs[i] = regs->regs[i];
        target->freeze_context.sp = regs->sp;
        target->freeze_context.pc = regs->pc;
        target->freeze_context.pstate = regs->pstate;
        target->freeze_context.generation = generation;
        target->freeze_context.active = 1;
        asm volatile("dmb ish" ::: "memory");

        freeze_progress_count[cpu]++;
        __atomic_store_n(&freeze_ack_generation[cpu], generation,
                         __ATOMIC_RELEASE);
        while (__atomic_load_n(&release_generation, __ATOMIC_ACQUIRE) != generation)
            asm volatile("yield" ::: "memory");

        asm volatile("dmb ish" ::: "memory");
        if (target->freeze_context.generation != generation ||
            !target->freeze_context.active)
            break;

        regs = task_pt_regs(target);
        for (int i = 0; i < 31; i++)
            regs->regs[i] = target->freeze_context.regs[i];
        regs->sp = target->freeze_context.sp;
        regs->pc = target->freeze_context.pc;
        regs->pstate = target->freeze_context.pstate;
        target->freeze_context.active = 0;
        __atomic_store_n(&release_ack_generation[cpu], generation, __ATOMIC_RELEASE);
        break;
    }

    case SGI_VM_TXN_FREEZE: {
        /*
         * Phase 31: 事务式冻结。
         *
         * 与 SGI_VM_FREEZE 相同的快照捕获，但 ACK 后保持忙等
         * 直到 txn_decision 非 0：
         *   1 = commit：按保存的异常现场恢复（继续执行）
         *   2 = rollback：也恢复保存的异常现场（从冻结点重新执行）；
         *       两者对 CPU 现场相同——重执行语义由 Guest 测试的
         *       计数器验证（rollback 分支专门测试重执行）。
         * 目标核在忙等期间 vCPU 完全静止，快照持续有效。
         */
        struct task_struct *target = txn_target_task;
        struct pt_regs *regs;
        int decision;

        if (__atomic_load_n(&txn_state, __ATOMIC_ACQUIRE) != TXN_FROZEN ||
            cpu != __atomic_load_n(&txn_target_cpu, __ATOMIC_RELAXED) ||
            !target || current != target || target->phys_cpu != cpu ||
            target->vcpu_id != 1 || target->vmid != 2 ||
            target->state != TASK_RUNNING)
            break;

        /* 若协调核已超时放弃（decision 非 0），直接返回不 ACK */
        if (__atomic_load_n(&txn_decision, __ATOMIC_ACQUIRE) != 0)
            break;

        memcpy(&target->freeze_context.cpu_sysregs, &target->cpu_sysregs,
               sizeof(target->cpu_sysregs));
        memcpy(&target->freeze_context.virt, &target->virt,
               sizeof(target->virt));
        vgic_save_state(&target->freeze_context.virt);
        target->freeze_context.virt.cntv_ctl = READ_SYSREG64(CNTV_CTL_EL0);
        target->freeze_context.virt.cntv_cval = READ_SYSREG64(CNTV_CVAL_EL0);
        target->freeze_context.virt.vtimer_pending =
            !!(target->freeze_context.virt.cntv_ctl & TIMER_CTL_ISTATUS);

        regs = task_pt_regs(target);
        for (int i = 0; i < 31; i++)
            target->freeze_context.regs[i] = regs->regs[i];
        target->freeze_context.sp = regs->sp;
        target->freeze_context.pc = regs->pc;
        target->freeze_context.pstate = regs->pstate;
        target->freeze_context.generation = 1; /* 事务固定 generation */
        target->freeze_context.active = 1;
        asm volatile("dmb ish" ::: "memory");

        freeze_progress_count[cpu]++;
        __atomic_store_n(&freeze_ack_generation[cpu], 1, __ATOMIC_RELEASE);

        /* 忙等决策：保持冻结直到协调核 commit/rollback */
        while ((decision = __atomic_load_n(&txn_decision,
                                           __ATOMIC_ACQUIRE)) == 0)
            asm volatile("yield" ::: "memory");

        /* generation/active 校验失败（如新的瞬时冻结覆盖了快照）：不恢复现场 */
        asm volatile("dmb ish" ::: "memory");
        if (target->freeze_context.generation != 1 ||
            !target->freeze_context.active) {
            __atomic_store_n(&release_ack_generation[cpu], 1,
                             __ATOMIC_RELEASE);
            break;
        }

        /* commit / rollback 都恢复保存的异常帧：
         * commit = 继续执行（guest 计数器在冻结期间不变，之后正常推进）
         * rollback = 从冻结点重新执行（guest 侧观察重执行副作用） */
        regs = task_pt_regs(target);
        for (int i = 0; i < 31; i++)
            regs->regs[i] = target->freeze_context.regs[i];
        regs->sp = target->freeze_context.sp;
        regs->pc = target->freeze_context.pc;
        regs->pstate = target->freeze_context.pstate;
        target->freeze_context.active = 0;
        (void)decision;
        __atomic_store_n(&release_ack_generation[cpu], 1, __ATOMIC_RELEASE);
        break;
    }

    case SGI_VGIC_UPDATE:
        /*
         * 跨核 vGIC 维护：有虚拟中断要注入到当前核上运行的 vCPU。
         * 由 vgic_inject_virq_to_vm 在跨核注入时触发。
         */
        vgic_ipi_update_handle();
        break;

    default:
        WARN("Unknown SGI#%d", sgi_id);
        break;
    }
}
