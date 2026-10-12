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
struct task_struct *current = &(init_task);

struct task_struct *task[NR_TASKS] = {
	&(init_task),
};

int nr_tasks = 1;

void _schedule(void)
{
	int next, c;

	struct task_struct *p;

	while (1) {
		c = -1;
		next = 0;

		for (int i = 0; i < NR_TASKS; i++) {
			p = task[i];

			if (p && p->state == TASK_RUNNING && p->counter > c) {
				c = p->counter;
				next = i;
			}
		}

		if (c)
			break;

		for (int i = 0; i < NR_TASKS; i++) {
			p = task[i];

			if (p)
				p->counter = (p->counter >> 1) + p->priority;
		}
	}

	switch_to(task[next]);
}

void schedule(void)
{
	current->counter = 0;
	_schedule();
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
	if (current == next)
		return;

	struct task_struct *prev = current;
	current = next;

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
	for (int i = 0; i < NR_TASKS; i++) {
		if (task[i] == current) {
			task[i]->state = TASK_ZOMBIE;
			break;
		}
	}

	schedule();
}

void set_cpu_sysregs(struct task_struct *tsk)
{
	set_stage2_pgd(tsk->mm.first_table, tsk->pid);
	restore_sysregs(&tsk->cpu_sysregs);
}

void vm_entering_work()
{
	if (!current || current->pid == 0)
		return;

	if (HAVE_FUNC(current->board_ops, entering_vm))
		current->board_ops->entering_vm(current);

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

	if (HAVE_FUNC(current->board_ops, leaving_vm))
		current->board_ops->leaving_vm(current);

	if (is_uart_forwarded_task(current))
		flush_task_console(current);
}

const char *task_state_str[] = {
	"RUNNING",
	"ZOMBIE",
};

void show_task_list(void)
{
	printf("%3s %12s %8s %7s %8s %7s %7s %7s %7s %7s\n", "id", "name",
	       "state", "pages", "saved-pc", "wfx", "hvc", "sysreg", "pf",
	       "mmio");

	for (int i = 0; i < nr_tasks; i++) {
		struct task_struct *tsk = task[i];
		printf("%3d %12s %8s %7d %8x %7d %7d %7d %7d %7d\n", tsk->pid,
		       tsk->name ? tsk->name : "", task_state_str[tsk->state],
		       tsk->mm.user_pages_count, task_pt_regs(tsk)->pc,
		       tsk->stat.wfx_trap_count, tsk->stat.hvc_trap_count,
		       tsk->stat.sysreg_trap_count, tsk->stat.pf_count,
		       tsk->stat.mmio_count);
	}
}
