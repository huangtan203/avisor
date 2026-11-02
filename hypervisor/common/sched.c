// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor
 *
 * A Tiny Hypervisor for IoT Development
 *
 * Copyright (c) 2022 Deng Jie (mr.dengjie@gmail.com).
 */

#include "common/sched.h"
#include "common/board.h"
#include "common/debug.h"
#include "common/irq.h"
#include "common/mm.h"
#ifdef PLATFORM_VIRT
#include "boards/virt/irq_domain.h"
#endif
#include "common/smp.h"
#include "common/task.h"
#include "common/utils.h"

static struct task_struct init_task = INIT_TASK;
struct cpu_sched_state cpu_state[MAX_CPUS];
static volatile int balance_target[MAX_CPUS] = { -1, -1 };
static unsigned int balance_cooldown[MAX_CPUS];

static void rq_lock(int cpu)
{
    unsigned int value;
    unsigned int failed;

    for (;;) {
        asm volatile("1: ldaxr %w0, [%2]\n"
                     "cbnz %w0, 1b\n"
                     "mov %w0, #1\n"
                     "stxr %w1, %w0, [%2]\n"
                     "cbnz %w1, 1b\n"
                     : "=&r"(value), "=&r"(failed)
                     : "r"(&cpu_state[cpu].rq_lock)
                     : "memory");
        return;
    }
}

static void rq_unlock(int cpu)
{
    asm volatile("stlr wzr, [%0]"
                 :: "r"(&cpu_state[cpu].rq_lock) : "memory");
}

static unsigned long rq_irq_save(void)
{
    unsigned long daif;

    asm volatile("mrs %0, daif\n"
                 "msr daifset, #2"
                 : "=r"(daif) :: "memory");
    return daif;
}

static void rq_irq_restore(unsigned long daif)
{
    asm volatile("msr daif, %0" :: "r"(daif) : "memory");
}

int sched_enqueue_task(int cpu, struct task_struct *tsk)
{
    unsigned long daif;
    int slot = -1;

    if (!tsk || cpu < 0 || cpu >= MAX_CPUS)
        return -1;

    daif = rq_irq_save();
    rq_lock(cpu);
    for (int i = 1; i < cpu_state[cpu].nr_tasks; i++) {
        if (cpu_state[cpu].task[i] == tsk) {
            slot = i;
            goto out;
        }
    }
    for (int i = 1; i < cpu_state[cpu].nr_tasks; i++) {
        if (!cpu_state[cpu].task[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0 && cpu_state[cpu].nr_tasks < NR_TASKS)
        slot = cpu_state[cpu].nr_tasks++;
    if (slot >= 0) {
        cpu_state[cpu].task[slot] = tsk;
        tsk->pid = slot;
        tsk->phys_cpu = cpu;
    }
out:
    rq_unlock(cpu);
    rq_irq_restore(daif);
    return slot;
}

int sched_dequeue_task(struct task_struct *tsk)
{
    unsigned long daif;
    int cpu;
    int removed = 0;

    if (!tsk)
        return -1;
    cpu = tsk->phys_cpu;
    if (cpu < 0 || cpu >= MAX_CPUS)
        return -1;

    daif = rq_irq_save();
    rq_lock(cpu);
    for (int i = 1; i < cpu_state[cpu].nr_tasks; i++) {
        if (cpu_state[cpu].task[i] != tsk)
            continue;
        cpu_state[cpu].task[i] = NULL;
        while (cpu_state[cpu].nr_tasks > 1 &&
               !cpu_state[cpu].task[cpu_state[cpu].nr_tasks - 1])
            cpu_state[cpu].nr_tasks--;
        removed = 1;
        break;
    }
    rq_unlock(cpu);
    rq_irq_restore(daif);
    return removed;
}

struct task_struct *sched_find_vcpu(unsigned long vmid, int vcpu_id)
{
    struct task_struct *result = NULL;
    unsigned long daif = rq_irq_save();

    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        rq_lock(cpu);
        for (int i = 1; i < cpu_state[cpu].nr_tasks; i++) {
            struct task_struct *tsk = cpu_state[cpu].task[i];

            if (tsk && tsk->vmid == vmid && tsk->vcpu_id == vcpu_id &&
                tsk->state != TASK_ZOMBIE) {
                result = tsk;
                break;
            }
        }
        rq_unlock(cpu);
        if (result)
            break;
    }
    rq_irq_restore(daif);
    return result;
}

int sched_runqueue_selftest(int cpu)
{
    struct task_struct probe = { 0 };
    int first;
    int duplicate;

    if (cpu < 0 || cpu >= MAX_CPUS)
        return -1;
    probe.state = TASK_WAITING;
    probe.phys_cpu = cpu;

    first = sched_enqueue_task(cpu, &probe);
    if (first < 1)
        return -1;
    duplicate = sched_enqueue_task(cpu, &probe);
    if (duplicate != first || sched_dequeue_task(&probe) != 1)
        return -1;
    if (sched_dequeue_task(&probe) != 0)
        return -1;
    return 0;
}

int sched_migrate_current(int target_cpu)
{
    struct task_struct *tsk = current;
    int source_cpu = smp_processor_id();
    int first_cpu;
    int second_cpu;
    int source_slot = -1;
    int target_slot = -1;
    unsigned long daif;

    if (!tsk || tsk->pid == 0 || target_cpu < 0 || target_cpu >= MAX_CPUS ||
        target_cpu == source_cpu || !smp_online[target_cpu] ||
        tsk->phys_cpu != source_cpu || tsk->state != TASK_RUNNING)
        return -1;

    first_cpu = source_cpu < target_cpu ? source_cpu : target_cpu;
    second_cpu = source_cpu < target_cpu ? target_cpu : source_cpu;
    daif = rq_irq_save();
    rq_lock(first_cpu);
    rq_lock(second_cpu);

    for (int i = 1; i < cpu_state[source_cpu].nr_tasks; i++) {
        if (cpu_state[source_cpu].task[i] == tsk) {
            source_slot = i;
            break;
        }
    }
    for (int i = 1; i < cpu_state[target_cpu].nr_tasks; i++) {
        if (!cpu_state[target_cpu].task[i]) {
            target_slot = i;
            break;
        }
    }
    if (target_slot < 0 && cpu_state[target_cpu].nr_tasks < NR_TASKS)
        target_slot = cpu_state[target_cpu].nr_tasks;

    if (source_slot < 0 || target_slot < 0) {
        rq_unlock(second_cpu);
        rq_unlock(first_cpu);
        rq_irq_restore(daif);
        return -1;
    }

    tsk->state = TASK_MIGRATING;
    cpu_state[source_cpu].task[source_slot] = NULL;
    while (cpu_state[source_cpu].nr_tasks > 1 &&
           !cpu_state[source_cpu].task[cpu_state[source_cpu].nr_tasks - 1])
        cpu_state[source_cpu].nr_tasks--;

    if (target_slot == cpu_state[target_cpu].nr_tasks)
        cpu_state[target_cpu].nr_tasks++;
    cpu_state[target_cpu].task[target_slot] = tsk;
    tsk->pid = target_slot;
    tsk->phys_cpu = target_cpu;
#ifdef PLATFORM_VIRT
    if (irq_domain_update_vcpu_affinity(tsk->vmid, tsk->vcpu_id,
                                       target_cpu) < 0)
        WARN("sched: failed to update IRQ affinity for VM%lu vCPU%d",
             tsk->vmid, tsk->vcpu_id);
#endif

    rq_unlock(second_cpu);
    rq_unlock(first_cpu);
    rq_irq_restore(daif);
    schedule();
    return 0;
}

void sched_complete_migration(struct task_struct *tsk)
{
    if (!tsk || tsk->state != TASK_MIGRATING)
        return;
    __atomic_store_n(&tsk->state, TASK_RUNNING, __ATOMIC_RELEASE);
    smp_request_reschedule(tsk->phys_cpu);
}

unsigned int sched_cpu_load(int cpu)
{
    if (cpu < 0 || cpu >= MAX_CPUS)
        return 0;
    return __atomic_load_n(&cpu_state[cpu].load_percent, __ATOMIC_ACQUIRE);
}

void sched_balance_tick(int cpu)
{
    unsigned long busy;
    unsigned long idle;
    int other;
    unsigned int local_load;
    unsigned int remote_load;

    if (cpu < 0 || cpu >= MAX_CPUS || !current)
        return;

    if (current->pid == 0)
        cpu_state[cpu].idle_ticks++;
    else {
        cpu_state[cpu].busy_ticks++;
        current->runtime_ticks++;
    }

    busy = cpu_state[cpu].busy_ticks;
    idle = cpu_state[cpu].idle_ticks;
    if (busy + idle < 100)
        return;

    local_load = (unsigned int)((busy * 100) / (busy + idle));
    __atomic_store_n(&cpu_state[cpu].load_percent, local_load,
                     __ATOMIC_RELEASE);
    cpu_state[cpu].busy_ticks = 0;
    cpu_state[cpu].idle_ticks = 0;

    if (balance_cooldown[cpu]) {
        balance_cooldown[cpu]--;
        return;
    }

    other = cpu ^ 1;
    if (other >= MAX_CPUS || !smp_online[other] || current->pid == 0)
        return;

    remote_load = sched_cpu_load(other);
    if (local_load >= 75 && remote_load + 25 <= local_load &&
        current->state == TASK_RUNNING) {
        __atomic_store_n(&balance_target[cpu], other, __ATOMIC_RELEASE);
        balance_cooldown[cpu] = 5;
    }
}

void sched_balance_safe_point(void)
{
    int cpu = smp_processor_id();
    int target = __atomic_load_n(&balance_target[cpu], __ATOMIC_ACQUIRE);

    if (target < 0)
        return;
    __atomic_store_n(&balance_target[cpu], -1, __ATOMIC_RELEASE);
    if (!current || current->pid == 0 || current->state != TASK_RUNNING ||
        current->phys_cpu != cpu || sched_migrate_current(target) < 0)
        balance_cooldown[cpu] = 1;
}

/* 初始化 CPU0 的调度器状态（在 hypervisor_main 早期调用） */
void sched_init_primary(void)
{
    int cpu = 0;
    cpu_state[cpu].curr = &cpu_state[cpu].init_task;
    cpu_state[cpu].init_task = init_task;  /* 拷贝初始值 */
    cpu_state[cpu].task[0] = &cpu_state[cpu].init_task;
    cpu_state[cpu].nr_tasks = 1;
    cpu_state[cpu].rr_start = 0;

    /*
     * 预先初始化所有其他 CPU 的调度器基础状态（idle task + nr_tasks=1）。
     * 这样主核可以在次核启动之前就把任务创建到对应 CPU 的任务表上
     * （pid 从 1 开始分配，task[0] 始终是 idle task）。
     *
     * 次核启动后调用 sched_init_secondary() 只会设置 curr 指针，
     * 不会修改 task[] 和 nr_tasks（任务已经在那了）。
     */
    for (int i = 1; i < MAX_CPUS; i++) {
        cpu_state[i].init_task = (struct task_struct)INIT_TASK;
        cpu_state[i].task[0] = &cpu_state[i].init_task;
        cpu_state[i].nr_tasks = 1;
        cpu_state[i].rr_start = 0;
        cpu_state[i].curr = NULL;  /* 次核启动后才设置 */
    }
}

/* 初始化次核的调度器状态（在次核入口调用）
 *
 * 注意：task[] 和 nr_tasks 已经由主核的 sched_init_primary() 预初始化了
 * （主核在次核启动前就可能在该核上创建任务）。这里只设置 curr 指针
 * 和 rr_start，确保次核从 idle task 开始调度。
 */
void sched_init_secondary(int cpu)
{
    cpu_state[cpu].curr = &cpu_state[cpu].init_task;
    cpu_state[cpu].rr_start = 0;
}

void _schedule(void)
{
	int next, c;
	struct task_struct *p;
	struct task_struct *selected;
    int cpu = smp_processor_id();
    int rr_start;

	while (1) {
		rq_lock(cpu);
		c = -1;
		next = 0;
		rr_start = cpu_state[cpu].rr_start;

		for (int i = 0; i < NR_TASKS; i++) {
			int idx = (rr_start + i) % NR_TASKS;
			p = cpu_state[cpu].task[idx];

			if (p && p->state == TASK_RUNNING && p->counter > c) {
				c = p->counter;
				next = idx;
			}
		}

		if (c) {
			selected = cpu_state[cpu].task[next];
			cpu_state[cpu].rr_start = (next + 1) % NR_TASKS;
			rq_unlock(cpu);
			break;
		}

		for (int i = 0; i < NR_TASKS; i++) {
			p = cpu_state[cpu].task[i];
			if (p && p->state == TASK_RUNNING)
				p->counter = (p->counter >> 1) + p->priority;
		}
		rq_unlock(cpu);
	}

	switch_to(selected);
}

void schedule(void)
{
	current->counter = 0;
	_schedule();
}

/* Phase 10: 主动让出 CPU（时间片清零，重新调度） */
void sched_yield(void)
{
    current->counter = 0;
    _schedule();
}

/* Phase 10: 设置任务优先级 */
int sched_set_priority(struct task_struct *tsk, int prio)
{
    if (!tsk || prio < 1 || prio > SCHED_PRIO_MAX)
        return -1;
    tsk->priority = prio;
    return 0;
}

/* Phase 10: 将任务设为等待状态（让出 CPU） */
void sched_wait(void)
{
    current->state = TASK_WAITING;
    current->counter = 0;
    _schedule();
}

/* Phase 10: 唤醒等待中的任务 */
void sched_wakeup(struct task_struct *tsk)
{
    if (!tsk || tsk->state != TASK_WAITING)
        return;
    tsk->state = TASK_RUNNING;
    /* 给新唤醒的任务一个完整时间片，立即参与调度 */
    tsk->counter = sched_prio_to_timeslice(tsk->priority);
}

void set_cpu_virtual_interrupt(struct task_struct *tsk)
{
#ifdef PLATFORM_VIRT
	/*
	 * virt 平台（GICv3）：虚拟中断由 GIC 硬件的 List Register
	 * （ICH_LR）注入，信号由 GIC 虚拟接口直接驱动 CPU 的 vIRQ 线。
	 *
	 * 注意：绝不能在这里清 HCR_EL2.VI（clear_virq）——
	 * QEMU 中 VI 位与 GIC 虚拟接口信号共享 CPU_INTERRUPT_VIRQ，
	 * 写 0 会连带撤销 GIC 硬件注入的 vIRQ 信号！
	 * （raspi 的 BCM2837 无 GIC，才用 VI/VF 软件注入。）
	 */
	(void)tsk;
	return;
#else
	if (HAVE_FUNC(tsk->board_ops, is_irq_asserted) &&
	    tsk->board_ops->is_irq_asserted(tsk))
		assert_virq();
	else
		clear_virq();

	if (HAVE_FUNC(tsk->board_ops, is_fiq_asserted) &&
	    tsk->board_ops->is_fiq_asserted(tsk))
		assert_vfiq();
	else
		clear_vfiq();
#endif
}

void switch_to(struct task_struct *next)
{
	task_stack_check(current);
	if (current == next)
		return;

	struct task_struct *prev = current;
	task_stack_check(next);

	/*
	 * 世界切换：保存 prev VM 的虚拟化状态（GIC 虚拟接口、虚拟定时器等）。
	 * 只在真正切换 VM 时做——同一个 VM 进出 EL2 不碰这些寄存器，
	 * 避免破坏 guest 的中断上下文（比如 active 状态的 vtimer）。
	 */
	if (HAVE_FUNC(prev->board_ops, leaving_vm))
		prev->board_ops->leaving_vm(prev);

	current = next;

	/* 世界切换：恢复 next VM 的虚拟化状态 */
	if (HAVE_FUNC(next->board_ops, entering_vm))
		next->board_ops->entering_vm(next);

	cpu_switch_to(prev, next);
}

void timer_tick()
{
	--current->counter;

	if (current->counter > 0)
		return;

	current->counter = 0;
	_schedule();
}

void exit_task()
{
    int cpu = smp_processor_id();
	for (int i = 0; i < NR_TASKS; i++) {
		if (cpu_state[cpu].task[i] == current) {
			cpu_state[cpu].task[i]->state = TASK_ZOMBIE;
			break;
		}
	}

	schedule();
}

void set_cpu_sysregs(struct task_struct *tsk)
{
	/*
	 * VMID 切换时失效 Stage 2 TLB（Phase 3 隔离强化）。
	 *
	 * TLB 条目按 VMID 标记，不同 VMID 间本不互相污染，但
	 * 硬件实现（尤其模拟器）对此的支持参差不齐；且 VM 销毁后
	 * VMID 复用时旧条目必然错误。切换前保守失效当前 VMID 的
	 * 全部 EL1 翻译条目（tlbi vmalle1 失效的是"当前 VTTBR 的
	 * VMID"——此时 VTTBR 还未切换，清的正是即将离开的 VM）。
	 *
	 * SMP：last_vmid 必须按物理核分开记录——两个核各自维护
	 * 自己的 TLB/上一次加载的 VMID，共享一个变量会导致某个核
	 * 误判"VMID 没变"而跳过 TLB 失效（内存隔离被破坏）。
	 */
	static unsigned long last_vmid[MAX_CPUS] = {
		[0 ... MAX_CPUS - 1] = ~0UL,
	};
	int cpu = smp_processor_id();

	if (tsk->vmid != last_vmid[cpu]) {
		asm volatile("tlbi vmalle1" ::: "memory");
		asm volatile("dsb ish" ::: "memory");
		last_vmid[cpu] = tsk->vmid;
	}

	set_stage2_pgd(tsk->mm.first_table, tsk->vmid);
	restore_sysregs(&tsk->cpu_sysregs);
}

void vm_entering_work()
{
	if (!current || current->pid == 0)
		return;

	task_stack_check(current);

	if (is_uart_forwarded_task(current))
		flush_task_console(current);

	set_cpu_sysregs(current);
	set_cpu_virtual_interrupt(current);
}

void vm_leaving_work()
{
	if (!current || current->pid == 0)
		return;

	save_sysregs(&current->cpu_sysregs);

	if (is_uart_forwarded_task(current))
		flush_task_console(current);
}

const char *task_state_str[] = {
	"RUNNING",
	"ZOMBIE",
    "WAITING",
};

void show_task_list(void)
{
    int cpu = smp_processor_id();
	printf("%3s %12s %8s %7s %8s %7s %7s %7s %7s %7s\n", "id", "name",
	       "state", "pages", "saved-pc", "wfx", "hvc", "sysreg", "pf",
	       "mmio");

	for (int i = 0; i < cpu_state[cpu].nr_tasks; i++) {
		struct task_struct *tsk = cpu_state[cpu].task[i];

		if (!tsk)
			continue;
		printf("%3d %12s %8s %7d %8x %7d %7d %7d %7d %7d\n", tsk->pid,
		       tsk->name ? tsk->name : "", task_state_str[tsk->state],
		       tsk->mm.user_pages_count, task_pt_regs(tsk)->pc,
		       tsk->stat.wfx_trap_count, tsk->stat.hvc_trap_count,
		       tsk->stat.sysreg_trap_count, tsk->stat.pf_count,
		       tsk->stat.mmio_count);
	}
}
