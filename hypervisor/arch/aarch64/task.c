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

#define TASK_STACK_CANARY 0x535441434b475244UL

static unsigned long task_stack_guard_end(struct task_struct *tsk)
{
	return (unsigned long)tsk + sizeof(*tsk) + TASK_STACK_GUARD_SIZE;
}

void task_stack_init(struct task_struct *tsk)
{
	unsigned long *guard = (unsigned long *)((unsigned long)tsk + sizeof(*tsk));

	for (unsigned long i = 0; i < TASK_STACK_GUARD_SIZE / sizeof(*guard); i++)
		guard[i] = TASK_STACK_CANARY;
	tsk->stack_low_water = (unsigned long)task_pt_regs(tsk);
}

void task_stack_check(struct task_struct *tsk)
{
	unsigned long sp;
	unsigned long guard_end;

	if (!tsk || tsk->pid == 0)
		return;

	if (!task_stack_guard_valid(tsk))
		PANIC("EL2 task stack overflow: vmid=%lu vcpu=%d pid=%ld",
		      tsk->vmid, tsk->vcpu_id, tsk->pid);

	asm volatile("mov %0, sp" : "=r"(sp));
	guard_end = task_stack_guard_end(tsk);
	if (tsk == current) {
		if (sp <= guard_end || sp - guard_end < TASK_STACK_MIN_FREE)
			PANIC("EL2 task stack exhausted: vmid=%lu vcpu=%d free=%lu",
			      tsk->vmid, tsk->vcpu_id,
			      sp > guard_end ? sp - guard_end : 0);
		if (sp < tsk->stack_low_water)
			tsk->stack_low_water = sp;
	}
}

unsigned long task_stack_min_free(struct task_struct *tsk)
{
	if (!tsk || tsk->pid == 0 || !tsk->stack_low_water)
		return 0;
	return tsk->stack_low_water - task_stack_guard_end(tsk);
}

int task_stack_guard_valid(struct task_struct *tsk)
{
	unsigned long *guard;

	if (!tsk || tsk->pid == 0)
		return 0;
	guard = (unsigned long *)((unsigned long)tsk + sizeof(*tsk));
	for (unsigned long i = 0; i < TASK_STACK_GUARD_SIZE / sizeof(*guard); i++)
		if (guard[i] != TASK_STACK_CANARY)
			return 0;
	return 1;
}

struct pt_regs *task_pt_regs(struct task_struct *tsk)
{
	unsigned long p =
		(unsigned long)tsk + THREAD_SIZE - sizeof(struct pt_regs);
	return (struct pt_regs *)p;
}

static void prepare_task(loader_func_t loader, void *arg)
{
	struct pt_regs *regs = task_pt_regs(current);
	regs->pstate = PSR_MODE_EL1h;
	/* interrupt mask */
	regs->pstate |= (0xf << 6);

	if (loader(arg, regs) < 0)
		PANIC("failed to load");

	/*
	 * 标记镜像加载完成（Stage 2 页表就绪）。
	 * 共享同一 VM 页表的次级 vCPU（shared_vm_loader）在
	 * 自旋等待此标志——必须带 barrier 保证页表写入先于标志可见。
	 */
	current->flags |= TASK_MM_READY;
	asm volatile("dmb ish" ::: "memory");

	set_cpu_sysregs(current);
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
	/* 默认创建在当前 CPU 上 */
	return create_task_on_cpu(smp_processor_id(), loader, arg);
}

/* 全局 VMID 分配器（同一 VM 的多个 vCPU 共享一个 vmid） */
static unsigned long next_vmid = 1;

/* 在指定 CPU 上创建任务（用于 SMP 场景） */
int create_task_on_cpu(int cpu, loader_func_t loader, void *arg)
{
	struct task_struct *p;

	p = (struct task_struct *)allocate_page();
	if (!p)
		return -1;

	p->freeze_context = (struct freeze_context *)allocate_page();
	if (!p->freeze_context) {
		deallocate_page(p);
		return -1;
	}

	struct pt_regs *childregs = task_pt_regs(p);

	p->cpu_context.x19 = (unsigned long)prepare_task;
	p->cpu_context.x20 = (unsigned long)loader;
	p->cpu_context.x21 = (unsigned long)arg;
	p->flags = 0;
	p->priority = 1;  /* 默认优先级，和 idle task 一致 */
	p->state = TASK_RUNNING;
	p->counter = p->priority;
	p->vmid = next_vmid++;  /* 全局唯一；多 vCPU VM 由调用方覆盖为相同值 */
	p->vcpu_id = 0;
	p->phys_cpu = cpu;
	(void)strncpy(p->name, "VM", 36);

#ifdef PLATFORM_VIRT
	p->board_ops = &virt_board_ops;
	if (HAVE_FUNC(p->board_ops, initialize))
		p->board_ops->initialize(p);
#else
	p->board_ops = &bcm2837_board_ops;
	if (HAVE_FUNC(p->board_ops, initialize))
		p->board_ops->initialize(p);
#endif

	prepare_initial_sysregs();
	memcpy(&p->cpu_sysregs, &initial_sysregs, sizeof(struct cpu_sysregs));

	/*
	 * 设置虚拟 MPIDR（Guest 读 mpidr_el1 时返回这个值）。
	 * 虚拟 SMP 的不同 vCPU 需要不同的 MPIDR，Guest 借此
	 * 区分自己是第几个 vCPU。aff0 字段 = vcpu_id。
	 */
	p->cpu_sysregs.mpidr_el1 = (unsigned long)p->vcpu_id;

	p->cpu_context.pc = (unsigned long)switch_from_kthread;
	p->cpu_context.sp = (unsigned long)childregs;
	task_stack_init(p);

	/* 添加到指定 CPU 的任务表 */
	int pid = cpu_state[cpu].nr_tasks++;
	cpu_state[cpu].task[pid] = p;
	p->pid = pid;  /* pid 是 per-CPU 的，多核间可能重复 */

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
    int cpu = smp_processor_id();
	(void)strncpy(cpu_state[cpu].task[0]->name, "HV", 36);
}
