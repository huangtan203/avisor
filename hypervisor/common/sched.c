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

	/*
	 * 轮转起点（Phase 3 调度公平性修复）：
	 *
	 * 原实现按固定索引 0,1,2... 扫描，counter 相同时永远选
	 * 索引小的任务（init_task 是 0 号）——多个 VM 同优先级时，
	 * 重算 counter 后总是同一个 VM 先跑，产生系统性偏置。
	 *
	 * 修复：扫描从"上次选中任务的下一个"开始，相同 counter 时
	 * 轮流胜出，实现真正的轮转公平。
	 */
	static int rr_start = 0;

	while (1) {
		c = -1;
		next = 0;

		for (int i = 0; i < NR_TASKS; i++) {
			int idx = (rr_start + i) % NR_TASKS;
			p = task[idx];

			if (p && p->state == TASK_RUNNING && p->counter > c) {
				c = p->counter;
				next = idx;
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

	/* 下次扫描从本次选中者的下一个任务开始 */
	rr_start = (next + 1) % NR_TASKS;

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
	/*
	 * VMID 切换时失效 Stage 2 TLB（Phase 3 隔离强化）。
	 *
	 * TLB 条目按 VMID 标记，不同 VMID 间本不互相污染，但
	 * 硬件实现（尤其模拟器）对此的支持参差不齐；且 VM 销毁后
	 * VMID 复用时旧条目必然错误。切换前保守失效当前 VMID 的
	 * 全部 EL1 翻译条目（tlbi vmalle1 失效的是"当前 VTTBR 的
	 * VMID"——此时 VTTBR 还未切换，清的正是即将离开的 VM）。
	 */
	static unsigned long last_vmid = ~0UL;

	if ((unsigned long)tsk->pid != last_vmid) {
		asm volatile("tlbi vmalle1" ::: "memory");
		asm volatile("dsb ish" ::: "memory");
		last_vmid = (unsigned long)tsk->pid;
	}

	set_stage2_pgd(tsk->mm.first_table, tsk->pid);
	restore_sysregs(&tsk->cpu_sysregs);
}

void vm_entering_work()
{
	if (!current || current->pid == 0)
		return;

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
