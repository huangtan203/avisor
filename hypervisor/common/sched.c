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
#include "common/task.h"
#include "common/utils.h"

static struct task_struct init_task = INIT_TASK;
struct cpu_sched_state cpu_state[MAX_CPUS];

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
    int cpu = smp_processor_id();
    int *rr_start = &cpu_state[cpu].rr_start;

	while (1) {
		c = -1;
		next = 0;

		for (int i = 0; i < NR_TASKS; i++) {
			int idx = (*rr_start + i) % NR_TASKS;
			p = cpu_state[cpu].task[idx];

			/*
			 * Phase 10: 只考虑 RUNNING 状态的任务
			 * （WAITING/ZOMBIE 不参与调度）
			 */
			if (p && p->state == TASK_RUNNING && p->counter > c) {
				c = p->counter;
				next = idx;
			}
		}

		if (c)
			break;

		/*
		 * 所有 RUNNING 任务时间片都用完了，重新分配。
		 * counter = (counter >> 1) + priority 的含义：
		 *   - priority 越高，分到的基础时间片越多
		 *   - 剩余时间 >> 1（衰减一半）让高优先级任务积累优势
		 *   - 等价于"加权公平调度"的简化版
		 */
		for (int i = 0; i < NR_TASKS; i++) {
			p = cpu_state[cpu].task[i];

			if (p && p->state == TASK_RUNNING)
				p->counter = (p->counter >> 1) + p->priority;
		}
	}

	/* 下次扫描从本次选中者的下一个任务开始 */
	*rr_start = (next + 1) % NR_TASKS;

	switch_to(cpu_state[cpu].task[next]);
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
		printf("%3d %12s %8s %7d %8x %7d %7d %7d %7d %7d\n", tsk->pid,
		       tsk->name ? tsk->name : "", task_state_str[tsk->state],
		       tsk->mm.user_pages_count, task_pt_regs(tsk)->pc,
		       tsk->stat.wfx_trap_count, tsk->stat.hvc_trap_count,
		       tsk->stat.sysreg_trap_count, tsk->stat.pf_count,
		       tsk->stat.mmio_count);
	}
}
