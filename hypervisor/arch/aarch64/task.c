// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor
 *
 * A Tiny Hypervisor for IoT Development
 *
 * Copyright (c) 2022 Deng Jie (mr.dengjie@gmail.com).
 */

#include "common/task.h"
#include "common/board.h"
#include "common/debug.h"
#include "common/entry.h"
#include "common/fifo.h"
#include "common/mm.h"
#include "common/sched.h"
#include "common/utils.h"

/*
 * 平台相关的设备模型（board_ops）
 *
 * 学习要点：
 *   每个 guest VM 看到的硬件平台是由 Hypervisor 模拟的。
 *   不同平台的 guest 需要不同的设备模型：
 *     - 树莓派 guest: 模拟 BCM2837 外设
 *     - virt guest:     模拟 PL011 + GIC + 架构定时器
 *
 *   board_ops 就是设备模型的抽象接口，
 *   包含 mmio_read/mmio_write/entering_vm/leaving_vm 等回调。
 */
#ifndef PLATFORM_VIRT
#include "emulator/raspi/bcm2837.h"
#else
#include "boards/virt/virt_board.h"
#endif

int uart_forwarded_task = 0;

struct pt_regs *task_pt_regs(struct task_struct *tsk)
{
	unsigned long p =
		(unsigned long)tsk + THREAD_SIZE - sizeof(struct pt_regs);
	return (struct pt_regs *)p;
}

static void prepare_task(loader_func_t loader, void *arg)
{
	INFO("loading...");

	struct pt_regs *regs = task_pt_regs(current);
	regs->pstate = PSR_MODE_EL1h;
	/* interrupt mask */
	regs->pstate |= (0xf << 6);

	if (loader(arg, regs) < 0)
		PANIC("failed to load");

	set_cpu_sysregs(current);

	INFO("loaded");
}

static struct cpu_sysregs initial_sysregs;

static void prepare_initial_sysregs(void)
{
	static int is_first_call = 1;

	if (!is_first_call)
		return;

	get_all_sysregs(&initial_sysregs);

	/* Disable MMU */
	initial_sysregs.sctlr_el1 &= ~1;

	is_first_call = 0;
}

void increment_current_pc(int ilen)
{
	struct pt_regs *regs = task_pt_regs(current);
	regs->pc += ilen;
}

int create_task(loader_func_t loader, void *arg)
{
	struct task_struct *p;

	p = (struct task_struct *)allocate_page();
	struct pt_regs *childregs = task_pt_regs(p);

	if (!p)
		return -1;

	p->cpu_context.x19 = (unsigned long)prepare_task;
	p->cpu_context.x20 = (unsigned long)loader;
	p->cpu_context.x21 = (unsigned long)arg;
	p->flags = 0;
	p->priority = current->priority;
	p->state = TASK_RUNNING;
	p->counter = p->priority;
	(void)strncpy(p->name, "VM", 36);

#ifndef PLATFORM_VIRT
	p->board_ops = &bcm2837_board_ops;
	if (HAVE_FUNC(p->board_ops, initialize))
		p->board_ops->initialize(p);
#else
	/* virt 平台：Phase 2 的设备模型（CNTVOFF 快照等） */
	p->board_ops = &virt_board_ops;
	if (HAVE_FUNC(p->board_ops, initialize))
		p->board_ops->initialize(p);
#endif

	prepare_initial_sysregs();
	memcpy(&p->cpu_sysregs, &initial_sysregs, sizeof(struct cpu_sysregs));

	p->cpu_context.pc = (unsigned long)switch_from_kthread;
	p->cpu_context.sp = (unsigned long)childregs;
	int pid = nr_tasks++;
	task[pid] = p;
	p->pid = pid;

	init_task_console(p);

	return pid;
}

void init_task_console(struct task_struct *tsk)
{
	tsk->console.in_fifo = create_fifo();
	tsk->console.out_fifo = create_fifo();
}

void flush_task_console(struct task_struct *tsk)
{
	struct fifo *outfifo = tsk->console.out_fifo;
	unsigned long val;

	while (dequeue_fifo(outfifo, &val) == 0)
		printf("%c", val & 0xff);
}

void init_initial_task()
{
	(void)strncpy(task[0]->name, "HV", 36);
}
