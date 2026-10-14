// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor
 *
 * A Tiny Hypervisor for IoT Development
 *
 * Copyright (c) 2022 Deng Jie (mr.dengjie@gmail.com).
 */

#pragma once

#define THREAD_CPU_CONTEXT 0 // offset of cpu_context in task_struct

#ifndef __ASSEMBLER__
#include "arch/aarch64/sysregs.h"
#define THREAD_SIZE 4096

#define NR_TASKS 64

/* SMP 支持：最大 CPU 数 */
#define MAX_CPUS 2

#define TASK_RUNNING 0
#define TASK_ZOMBIE  1
#define TASK_WAITING 2   /* 等待状态（如等待 I/O、等待共享内存消息） */

/* task_struct::flags：镜像加载完成（Stage 2 页表就绪）
 * 共享页表的次级 vCPU 依赖此标志判断主 vCPU 是否加载完毕 */
#define TASK_MM_READY 0x1

struct board_ops;

struct cpu_context {
	unsigned long x19;
	unsigned long x20;
	unsigned long x21;
	unsigned long x22;
	unsigned long x23;
	unsigned long x24;
	unsigned long x25;
	unsigned long x26;
	unsigned long x27;
	unsigned long x28;
	unsigned long fp;
	unsigned long sp;
	unsigned long pc;
};

struct cpu_sysregs {
	// not trapped (save & restore performed)
	unsigned long sctlr_el1;
	unsigned long ttbr0_el1;
	unsigned long ttbr1_el1;
	unsigned long tcr_el1;
	unsigned long esr_el1;
	unsigned long far_el1;
	unsigned long afsr0_el1;
	unsigned long afsr1_el1;
	unsigned long mair_el1;
	unsigned long amair_el1;
	unsigned long contextidr_el1;

	unsigned long cpacr_el1;
	unsigned long elr_el1;
	unsigned long fpcr;
	unsigned long fpsr;
	unsigned long midr_el1; // ro
	unsigned long mpidr_el1; // ro
	unsigned long par_el1;
	unsigned long sp_el0;
	unsigned long sp_el1;
	unsigned long spsr_el1;
	unsigned long tpidr_el0;
	unsigned long tpidr_el1;
	unsigned long tpidrro_el0;
	unsigned long vbar_el1;

	// trapped by TACR
	unsigned long actlr_el1; // rw

	// trapped by TID3
	unsigned long id_pfr0_el1; // ro
	unsigned long id_pfr1_el1; // ro
	unsigned long id_mmfr0_el1; // ro
	unsigned long id_mmfr1_el1; // ro
	unsigned long id_mmfr2_el1; // ro
	unsigned long id_mmfr3_el1; // ro
	unsigned long id_isar0_el1; // ro
	unsigned long id_isar1_el1; // ro
	unsigned long id_isar2_el1; // ro
	unsigned long id_isar3_el1; // ro
	unsigned long id_isar4_el1; // ro
	unsigned long id_isar5_el1; // ro
	unsigned long mvfr0_el1; // ro
	unsigned long mvfr1_el1; // ro
	unsigned long mvfr2_el1; // ro
	unsigned long id_aa64pfr0_el1; // ro
	unsigned long id_aa64pfr1_el1; // ro
	unsigned long id_aa64dfr0_el1; // ro
	unsigned long id_aa64dfr1_el1; // ro
	unsigned long id_aa64isar0_el1; // ro
	unsigned long id_aa64isar1_el1; // ro
	unsigned long id_aa64mmfr0_el1; // ro
	unsigned long id_aa64mmfr1_el1; // ro
	unsigned long id_aa64afr0_el1; // ro
	unsigned long id_aa64afr1_el1; // ro

	// trapped by TID2
	unsigned long ctr_el0; // ro
	unsigned long ccsidr_el1; // ro
	unsigned long clidr_el1; // ro
	unsigned long csselr_el1; // rw

	// trapped by TID1
	unsigned long aidr_el1; // ro
	unsigned long revidr_el1; // ro

	// system timer
	unsigned long cntkctl_el1;
	unsigned long cntp_ctl_el0;
	unsigned long cntp_cval_el0;
	unsigned long cntp_tval_el0;
	unsigned long cntv_ctl_el0;
	unsigned long cntv_cval_el0;
	unsigned long cntv_tval_el0;
};

struct mm_struct {
	unsigned long first_table;
	int user_pages_count;
	int kernel_pages_count;
	/*
	 * 虚拟计数器偏移（virt 平台）：
	 * 创建 VM 时快照物理计数器，Guest 的 CNTVCT 从 0 开始。
	 * 每次进入 VM 时由 board_ops->entering_vm 写入 CNTVOFF_EL2。
	 */
	unsigned long cntvoff;
};

struct task_stat {
	long wfx_trap_count;
	long hvc_trap_count;
	long sysreg_trap_count;
	long pf_count;
	long mmio_count;
};

struct task_console {
	struct fifo *in_fifo;
	struct fifo *out_fifo;
};

/*
 * per-VM 虚拟化状态（virt 平台，Phase 3-4）
 *
 * GICv3 虚拟接口的 ICH_VMCR/ICH_LR 和架构虚拟定时器 CNTV 都是
 * per-CPU 硬件资源：多个 VM 分时复用同一 CPU 时，世界切换必须
 * 保存/恢复，否则 VM A 的状态会泄漏给 VM B。
 */
struct vm_virt_state {
	unsigned long gic_vmcr;         /* ICH_VMCR_EL2 */
	unsigned long gic_lr[NR_VGIC_LRS]; /* ICH_LR0..LRn_EL2 */
	unsigned long cntv_ctl;         /* CNTV_CTL_EL0 */
	unsigned long cntv_cval;        /* CNTV_CVAL_EL0 */
	int vtimer_pending;             /* vtimer 延迟注入标志 */
	int vgic_ipi_pending;           /* 跨核 vGIC 注入：有待注入的虚拟中断 */
	unsigned int vgic_ipi_vintid;   /* 跨核 vGIC 注入：待注入的虚拟中断号 */
};

struct task_struct {
	struct cpu_context cpu_context;
	long state;
	long counter;
	long priority;
	long preempt_count;
	long pid; // per-CPU 任务表内的索引
	unsigned long vmid; // 全局 VMID（同一 VM 的多个 vCPU 共享，用于 VTTBR/TLB）
	unsigned long flags;
	char name[36];
	const struct board_ops *board_ops;
	void *board_data;
	struct mm_struct mm;
	struct cpu_sysregs cpu_sysregs;
	struct task_stat stat;
	struct task_console console;
	struct vm_virt_state virt; /* per-VM 虚拟化状态（Phase 3） */
    int vcpu_id;    /* 虚拟 CPU ID（SMP VM 的第几个 vCPU） */
    int phys_cpu;   /* 绑定的物理 CPU ID */
};

/* per-CPU 调度器状态
 * 每个物理核有自己的 current / task 表 / nr_tasks / rr_start
 * 任务不跨核迁移，VM 的 vCPU 绑定到固定物理核
 *
 * 注意：必须定义在 task_struct 之后（含完整结构体字段 init_task）
 */
struct cpu_sched_state {
    struct task_struct *curr;  /* 当前运行的任务（避免和 current 宏名冲突） */
    struct task_struct *task[NR_TASKS];
    int nr_tasks;
    int rr_start;       /* 轮转调度起点 */
    struct task_struct init_task;  /* 本核的 idle 任务 */
};

extern struct cpu_sched_state cpu_state[MAX_CPUS];

/* 获取当前物理 CPU ID（读 TPIDR_EL2）
 *
 * 注意：不能用 MPIDR_EL1，因为 HCR_EL2.VM=1 时，EL2 读 MPIDR_EL1
 * 可能返回 VMPIDR_EL2 中的虚拟值（而非物理 MPIDR），导致 per-CPU
 * 的 current 指针、任务表等数据结构全部错乱。
 *
 * 使用 TPIDR_EL2：EL2 专属的软件线程 ID 寄存器，完全由软件控制，
 * 在每个 CPU 启动时写入物理 CPU ID，之后只读，不受任何虚拟化影响。
 */
#define smp_processor_id() ({                              \
    unsigned long __cpu;                                   \
    asm volatile("mrs %0, tpidr_el2" : "=r"(__cpu));       \
    (int)__cpu;                                            \
})

/*
 * 当前核的 current 指针（宏：便于替换为 per-CPU 版本）
 *
 * 注意：不要用 `task` 或 `nr_tasks` 作为宏名——它们和代码中
 * 大量的函数参数名 / 局部变量名冲突（C 预处理器会全部替换）。
 * 需要访问 per-CPU 任务表时，直接写 cpu_state[smp_processor_id()].task
 * 或用下面的 get_cpu_tasks() / get_cpu_nr_tasks() 内联函数。
 */
#define current  (cpu_state[smp_processor_id()].curr)

static inline struct task_struct **get_cpu_tasks(void)
{
    return cpu_state[smp_processor_id()].task;
}

static inline int get_cpu_nr_tasks(void)
{
    return cpu_state[smp_processor_id()].nr_tasks;
}

/* 为了减少代码改动量，保留 task / nr_tasks 作为 get_cpu_tasks()/get_cpu_nr_tasks()
 * 的别名，但只能在不会和标识符冲突的上下文中使用（如 .c 文件里）。
 *
 * 实际上由于很多函数参数名就是 `task`，我们不用宏，而是在每个 .c 文件
 * 开头用局部的 #define 或者直接写 cpu_state[...] 形式。
 *
 * 为了最小化改动，这里还是提供两个宏，但文件名/参数名中含 `task` 的
 * 会有问题——我们在那些文件里手动改用 cpu_state[...]。
 */
#define CPU_TASK_TABLE   (cpu_state[smp_processor_id()].task)
#define CPU_NR_TASKS     (cpu_state[smp_processor_id()].nr_tasks)

#define FIRST_TASK CPU_TASK_TABLE[0]
#define LAST_TASK  CPU_TASK_TABLE[NR_TASKS - 1]

extern void sched_init(void);
extern void sched_init_primary(void);
extern void sched_init_secondary(int cpu);
extern void schedule(void);
extern void timer_tick(void);
extern void preempt_disable(void);
extern void preempt_enable(void);
extern void set_cpu_virtual_interrupt(struct task_struct *);
void set_cpu_sysregs(struct task_struct *);
extern void switch_to(struct task_struct *);
extern void cpu_switch_to(struct task_struct *, struct task_struct *);
extern void exit_task(void);
extern void show_task_list(void);

/* Phase 10: 优先级调度 + 时间片轮转 + sched_yield */

/* 优先级定义（数字越大优先级越高） */
#define SCHED_PRIO_LOW      1
#define SCHED_PRIO_NORMAL   2
#define SCHED_PRIO_HIGH     3
#define SCHED_PRIO_MAX      3

/* 各优先级对应的时间片（tick 数，假设 1 tick = 10ms）
 * 高优先级 = 更长的时间片 + 更多的调度机会
 */
#define SCHED_TIMESLICE_LOW     1   /* 10ms */
#define SCHED_TIMESLICE_NORMAL  2   /* 20ms */
#define SCHED_TIMESLICE_HIGH    4   /* 40ms */

/* 优先级到时间片的转换 */
static inline int sched_prio_to_timeslice(int prio)
{
    if (prio >= SCHED_PRIO_HIGH) return SCHED_TIMESLICE_HIGH;
    if (prio >= SCHED_PRIO_NORMAL) return SCHED_TIMESLICE_NORMAL;
    return SCHED_TIMESLICE_LOW;
}

/*
 * sched_yield: 主动让出 CPU（但仍在就绪队列中）
 * 调用后当前任务时间片清零，触发调度。
 */
extern void sched_yield(void);

/*
 * sched_set_priority: 设置任务优先级（1~SCHED_PRIO_MAX）
 * 返回 0 成功，-1 参数非法
 */
extern int sched_set_priority(struct task_struct *tsk, int prio);

/*
 * sched_wakeup: 唤醒一个处于 TASK_WAITING 状态的任务
 * 注意：只能在目标任务所在的 CPU 上下文中调用（或关中断）
 */
extern void sched_wakeup(struct task_struct *tsk);

/*
 * sched_wait: 将当前任务设为等待状态并调度
 * （需要其他任务/中断调用 sched_wakeup 来唤醒）
 */
extern void sched_wait(void);

#define INIT_TASK                                                              \
	{                                                                      \
		/* cpu_context */ { 0 }, /* state etc */ 0, 0, 1, 0, 0,        \
			/* vmid */ 0, /* flags */ 0, "", 0, 0,                 \
			/* mm */ { 0 }, /* cpu_sysregs */ { 0 },               \
			/* stat */ { 0 }, /* console */ { 0 },                 \
			/* virt */ { 0 }, /* vcpu_id */ 0, /* phys_cpu */ 0,   \
	}

#endif
