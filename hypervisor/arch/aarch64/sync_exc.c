// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor
 *
 * A Tiny Hypervisor for IoT Development
 *
 * Copyright (c) 2022 Deng Jie (mr.dengjie@gmail.com).
 */

#include "common/sync_exc.h"
#include "arch/aarch64/sysregs.h"
#include "common/debug.h"
#include "common/hvc.h"
#include "common/irq.h"
#include "common/mm.h"
#include "common/sched.h"
#include "common/task.h"

#ifdef PLATFORM_VIRT
#include "boards/virt/arch_timer.h"
#include "boards/virt/pci.h"
#include "boards/virt/virt_mm.h"
#include "boards/virt/virt_board.h"
#include "boards/virt/vgic.h"
#include "boards/virt/shmem.h"
#endif

const char *sync_error_reasons[] = {
	"Unknown reason.",
	"Trapped WFI or WFE instruction execution.",
	"(unknown)",
	"Trapped MCR or MRC access with (coproc==0b1111).",
	"Trapped MCRR or MRRC access with (coproc==0b1111).",
	"Trapped MCR or MRC access with (coproc==0b1110).",
	"Trapped LDC or STC access.",
	"Access to SVE, Advanced SIMD, or floating-point functionality trapped by CPACR_EL1.FPEN, CPTR_EL2.FPEN, CPTR_EL2.TFP, or CPTR_EL3.TFP control.",
	"Trapped VMRS access, from ID group trap.",
	"Trapped use of a Pointer authentication instruction because HCR_EL2.API == 0 || SCR_EL3.API == 0.",
	"(unknown)",
	"(unknown)",
	"Trapped MRRC access with (coproc==0b1110).",
	"Branch Target Exception.",
	"Illegal Execution state.",
	"(unknown)",
	"(unknown)",
	"SVC instruction execution in AArch32 state.",
	"HVC instruction execution in AArch32 state.",
	"SMC instruction execution in AArch32 state.",
	"(unknown)",
	"SVC instruction execution in AArch64 state.",
	"HVC instruction execution in AArch64 state.",
	"SMC instruction execution in AArch64 state.",
	"Trapped MSR, MRS or System instruction execution in AArch64 state.",
	"Access to SVE functionality trapped as a result of CPACR_EL1.ZEN, CPTR_EL2.ZEN, CPTR_EL2.TZ, or CPTR_EL3.EZ.",
	"Trapped ERET, ERETAA, or ERETAB instruction execution.",
	"(unknown)",
	"Exception from a Pointer Authentication instruction authentication failure.",
	"(unknown)",
	"(unknown)",
	"(unknown)",
	"Instruction Abort from a lower Exception level.",
	"Instruction Abort taken without a change in Exception level.",
	"PC alignment fault exception.",
	"(unknown)",
	"Data Abort from a lower Exception level.",
	"Data Abort without a change in Exception level, or Data Aborts taken to EL2 as a result of accesses generated associated with VNCR_EL2 as part of nested virtualization support.",
	"SP alignment fault exception.",
	"(unknown)",
	"Trapped floating-point exception taken from AArch32 state.",
	"(unknown)",
	"(unknown)",
	"(unknown)",
	"Trapped floating-point exception taken from AArch64 state.",
	"(unknown)",
	"(unknown)",
	"SError interrupt.",
	"Breakpoint exception from a lower Exception level.",
	"Breakpoint exception taken without a change in Exception level.",
	"Software Step exception from a lower Exception level.",
	"Software Step exception taken without a change in Exception level.",
	"Watchpoint from a lower Exception level.",
	"Watchpoint exceptions without a change in Exception level, or Watchpoint exceptions taken to EL2 as a result of accesses generated associated with VNCR_EL2 as part of nested virtualization support.",
	"(unknown)",
	"(unknown)",
	"BKPT instruction execution in AArch32 state.",
	"(unknown)",
	"Vector Catch exception from AArch32 state.",
	"(unknown)",
	"BRK instruction execution in AArch64 state.",
};

void handle_trap_wfx()
{
	schedule();
	increment_current_pc(4);
}

/*
 * Hypercall 处理（半虚拟化接口）
 *
 * 学习要点：
 *   Guest 执行 HVC 指令陷入 EL2，x8 是功能号，x0 是参数。
 *   Guest 的通用寄存器已经保存在异常栈帧（pt_regs）里，
 *   修改 pt_regs->regs[] 就等于修改 Guest 恢复后的寄存器——
 *   这是"返回值"的传递方式（类似 syscall 的返回值写回 x0）。
 */
void handle_hvc64(unsigned long hvc_nr)
{
	struct pt_regs *regs = task_pt_regs(current);

	switch (hvc_nr) {
	case HVC_CONSOLE_PUTC: {
		/*
		 * Guest 控制台输出：只在每行行首打 "[vmN] " 前缀，
		 * 避免逐字符 HVC 时前缀刷屏。
		 */
		static int at_line_start = 1;
		char c = (char)(regs->regs[0] & 0xff);

		if (at_line_start)
			printf("[vm%lu] ", current->vmid);
		printf("%c", c);
		at_line_start = (c == '\n');
		break;
	}
#ifdef PLATFORM_VIRT
	case HVC_GET_TIMER_FREQ:
		/* 告诉 Guest 定时器频率，避免 guest 硬编码 */
		regs->regs[0] = get_timer_frequency();
		break;
	case HVC_GET_VMID:
		/* 告诉 Guest 自己的 VM 编号（多 VM 演示用）
		 * 用全局 vmid：虚拟 SMP 的多个 vCPU 返回相同编号 */
		regs->regs[0] = current->vmid;
		break;
	case HVC_PCI_DEBUG:
		/* PCI 直通中断链路诊断 dump（Phase 7 调试用） */
		pci_debug_dump();
		break;

	/* Phase 10: 调度器 hypercall */
	case HVC_SCHED_YIELD:
		sched_yield();
		regs->regs[0] = 0;
		break;
	case HVC_SCHED_GET_PRIO:
		regs->regs[0] = current->priority;
		break;
	case HVC_SCHED_SET_PRIO: {
		int prio = (int)(regs->regs[0] & 0xff);
		regs->regs[0] = sched_set_priority(current, prio);
		break;
	}

	/* Phase 10: 内存管理 hypercall */
	case HVC_MEM_MAP: {
		paddr_t ipa = (paddr_t)regs->regs[0];
		uint64_t flags = regs->regs[1];
		regs->regs[0] = hyp_mem_map(ipa, flags);
		break;
	}
	case HVC_MEM_UNMAP: {
		paddr_t ipa = (paddr_t)regs->regs[0];
		regs->regs[0] = hyp_mem_unmap(ipa);
		break;
	}

	/* Phase 10: 共享内存 hypercall */
	case HVC_SHM_CREATE: {
		uint32_t key = (uint32_t)regs->regs[0];
		uint32_t size = (uint32_t)regs->regs[1];
		regs->regs[0] = shm_create(key, size);
		break;
	}
	case HVC_SHM_ATTACH: {
		int shmid = (int)regs->regs[0];
		paddr_t ipa = (paddr_t)regs->regs[1];
		regs->regs[0] = shm_attach(shmid, ipa);
		break;
	}
	case HVC_SHM_DETACH: {
		int shmid = (int)regs->regs[0];
		regs->regs[0] = shm_detach(shmid);
		break;
	}
	case HVC_SHM_NOTIFY: {
		int shmid = (int)regs->regs[0];
		regs->regs[0] = shm_notify(shmid);
		break;
	}

	/* Phase 12: 虚拟 SGI（vSGI）hypercall */
	case HVC_VSGI_SEND: {
		int target_vcpu = (int)(regs->regs[0] & 0xff);
		uint32_t sgi_id = (uint32_t)(regs->regs[1] & 0xf);
		/* SGI 的 INTID 范围是 0-15（SGI0-SGI15） */
		if (sgi_id > 15) {
			regs->regs[0] = -1;
			break;
		}
		regs->regs[0] = virt_inject_virq_to_vcpu(
			(int)current->vmid, target_vcpu, sgi_id);
		break;
	}
	case HVC_VSGI_GET_CPUID:
		regs->regs[0] = current->vcpu_id;
		break;

	/* Phase 15: 虚拟 GICD 测试 hypercall——直接经 vgic_inject_virq
	 * 注入指定虚拟 SPI（带 GICD enable 门控），返回值 0=注入成功，
	 * -1=被 GICD 门控丢弃或 LR 无空闲。供 guest 验证 trap-and-emulate。 */
	case HVC_GICD_INJECT:
		regs->regs[0] = vgic_inject_virq((uint32_t)regs->regs[0]);
		break;
#endif
	default:
		WARN("HVC #%d", hvc_nr);
		break;
	}
}

void handle_trap_system(unsigned long esr)
{
/* msr(reg) */
#define DEFINE_SYSREG_MSR(name, _op1, _crn, _crm, _op2)                \
	do {                                                           \
		if (op1 == (_op1) && crn == (_crn) && crm == (_crm) && \
		    op2 == (_op2)) {                                   \
			current->cpu_sysregs.name = regs->regs[rt];    \
			goto sys_fin;                                  \
		}                                                      \
	} while (0)

/* mrs */
#define DEFINE_SYSREG_MRS(name, _op1, _crn, _crm, _op2)                \
	do {                                                           \
		if (op1 == (_op1) && crn == (_crn) && crm == (_crm) && \
		    op2 == (_op2)) {                                   \
			regs->regs[rt] = current->cpu_sysregs.name;    \
			goto sys_fin;                                  \
		}                                                      \
	} while (0)

	struct pt_regs *regs = task_pt_regs(current);

	unsigned int op0 = (esr >> 20) & 0x3;
	unsigned int op2 = (esr >> 17) & 0x7;
	unsigned int op1 = (esr >> 14) & 0x7;
	unsigned int crn = (esr >> 10) & 0xf;
	unsigned int rt = (esr >> 5) & 0x1f;
	unsigned int crm = (esr >> 1) & 0xf;
	unsigned int dir = esr & 0x1;

	// INFO("trap_system: op0=%u,op2=%u,op1=%u,crn=%u,rt=%u,crm=%u,dir=%u",
	//	op0, op2, op1, crn, rt, crm, dir);

	if ((op0 & 2) && dir == 0) {
		DEFINE_SYSREG_MSR(actlr_el1, 0, 1, 0, 1);
		DEFINE_SYSREG_MSR(csselr_el1, 1, 0, 0, 0);
	} else if ((op0 & 2) && dir == 1) {
		DEFINE_SYSREG_MRS(actlr_el1, 0, 1, 0, 1);
		/*
		 * MPIDR_EL1 (S3_0_C0_C0_5)：虚拟 SMP 的 vCPU 身份。
		 *
		 * HCR.TID1=1 使 guest 读 MPIDR 陷入 EL2。每个 vCPU 的
		 * 身份保存在 cpu_sysregs.mpidr_el1（创建任务时按
		 * vcpu_id 设置），guest 借此区分主/次 vCPU。
		 */
		DEFINE_SYSREG_MRS(mpidr_el1, 0, 0, 0, 5);
		DEFINE_SYSREG_MRS(id_pfr0_el1, 0, 0, 1, 0);
		DEFINE_SYSREG_MRS(id_pfr1_el1, 0, 0, 1, 1);
		DEFINE_SYSREG_MRS(id_mmfr0_el1, 0, 0, 1, 4);
		DEFINE_SYSREG_MRS(id_mmfr1_el1, 0, 0, 1, 5);
		DEFINE_SYSREG_MRS(id_mmfr2_el1, 0, 0, 1, 6);
		DEFINE_SYSREG_MRS(id_mmfr3_el1, 0, 0, 1, 7);
		DEFINE_SYSREG_MRS(id_isar0_el1, 0, 0, 2, 0);
		DEFINE_SYSREG_MRS(id_isar1_el1, 0, 0, 2, 1);
		DEFINE_SYSREG_MRS(id_isar2_el1, 0, 0, 2, 2);
		DEFINE_SYSREG_MRS(id_isar3_el1, 0, 0, 2, 3);
		DEFINE_SYSREG_MRS(id_isar4_el1, 0, 0, 2, 4);
		DEFINE_SYSREG_MRS(id_isar5_el1, 0, 0, 2, 5);
		DEFINE_SYSREG_MRS(mvfr0_el1, 0, 0, 3, 0);
		DEFINE_SYSREG_MRS(mvfr1_el1, 0, 0, 3, 1);
		DEFINE_SYSREG_MRS(mvfr2_el1, 0, 0, 3, 2);
		DEFINE_SYSREG_MRS(id_aa64pfr0_el1, 0, 0, 4, 0);
		DEFINE_SYSREG_MRS(id_aa64pfr1_el1, 0, 0, 4, 1);
		DEFINE_SYSREG_MRS(id_aa64dfr0_el1, 0, 0, 5, 0);
		DEFINE_SYSREG_MRS(id_aa64dfr1_el1, 0, 0, 5, 1);
		DEFINE_SYSREG_MRS(id_aa64isar0_el1, 0, 0, 6, 0);
		DEFINE_SYSREG_MRS(id_aa64isar1_el1, 0, 0, 6, 1);
		DEFINE_SYSREG_MRS(id_aa64mmfr0_el1, 0, 0, 7, 0);
		DEFINE_SYSREG_MRS(id_aa64mmfr1_el1, 0, 0, 7, 1);
		DEFINE_SYSREG_MRS(id_aa64afr0_el1, 0, 0, 5, 4);
		DEFINE_SYSREG_MRS(id_aa64afr1_el1, 0, 0, 5, 5);
		DEFINE_SYSREG_MRS(ctr_el0, 3, 0, 0, 1);
		DEFINE_SYSREG_MRS(ccsidr_el1, 1, 0, 0, 0);
		DEFINE_SYSREG_MRS(clidr_el1, 1, 0, 0, 1);
		DEFINE_SYSREG_MRS(csselr_el1, 2, 0, 0, 0);
		DEFINE_SYSREG_MRS(aidr_el1, 1, 0, 0, 7);
		DEFINE_SYSREG_MRS(revidr_el1, 0, 0, 0, 6);
	}

	WARN("system register access is not handled.");
sys_fin:
	increment_current_pc(4);
	return;
}

#define ESR_EL2_EC_SHIFT 26

#define ESR_EL2_EC_TRAP_WFX    1
#define ESR_EL2_EC_TRAP_FP_REG 7
#define ESR_EL2_EC_HVC64       22
#define ESR_EL2_EC_TRAP_SYSTEM 24
#define ESR_EL2_EC_TRAP_SVE    25
#define ESR_EL2_EC_DABT_LOW    36

void handle_sync_exception(unsigned long esr, unsigned long elr,
			   unsigned long far, unsigned long hvc_nr)
{
	int eclass = (esr >> ESR_EL2_EC_SHIFT) & 0x3f;

	switch (eclass) {
	case ESR_EL2_EC_TRAP_WFX:
		current->stat.wfx_trap_count++;
		handle_trap_wfx();
		break;
	case ESR_EL2_EC_TRAP_FP_REG:
		WARN("TRAP_FP_REG is not implemented.");
		break;
	case ESR_EL2_EC_HVC64:
		current->stat.hvc_trap_count++;
		handle_hvc64(hvc_nr);
		break;
	case ESR_EL2_EC_TRAP_SYSTEM:
		current->stat.sysreg_trap_count++;
		handle_trap_system(esr);
		break;
	case ESR_EL2_EC_TRAP_SVE:
		WARN("TRAP_SVE is not implemented.");
		break;
	case ESR_EL2_EC_DABT_LOW:
		if (handle_mem_abort(far, esr) < 0)
			PANIC("handle_mem_abort() failed.");
		break;
	default:
		PANIC("uncaught synchronous exception:\n%s\nesr: %x, address: %x",
		      sync_error_reasons[eclass], esr, elr);
		break;
	}
}
