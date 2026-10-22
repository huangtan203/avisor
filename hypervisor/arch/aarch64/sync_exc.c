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
#include "common/ksm.h"
#include "common/migration.h"
#include "common/smp.h"
#include "arch/aarch64/mmu.h"
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
	/* Phase 20: 内存统计 hypercall
	 * 返回 x0 = user_pages_count, x1 = kernel_pages_count */
	case HVC_MEM_STATS: {
		regs->regs[0] = current->mm.user_pages_count;
		regs->regs[1] = current->mm.kernel_pages_count;
		break;
	}
	/* Phase 21: 修改内存页权限
	 * x0 = ipa (页对齐), x1 = prot (0=RW, 1=RO, 2=NONE)
	 * 返回 x0 = 0 成功，-1 失败 */
	case HVC_MEM_PROTECT: {
		paddr_t ipa = (paddr_t)regs->regs[0];
		uint64_t prot = regs->regs[1];
		uint64_t ap;

		switch (prot) {
		case 0: ap = S2_AP_RW;   break;
		case 1: ap = S2_AP_RO;   break;
		case 2: ap = S2_AP_NONE; break;
		default:
			regs->regs[0] = -1;  /* 无效 prot 值 */
			break;
		}
		if (prot > 2)
			break;

		regs->regs[0] = mprotect_stage2_page(current, ipa, ap);
		break;
	}

	/* Phase 22: 查询内存使用情况
	 * 返回 x0 = 当前用户内存(字节), x1 = 配额(字节), x2 = 大页数量 */
	case HVC_MEM_QUERY: {
		regs->regs[0] = get_task_memory_usage(current);
		regs->regs[1] = current->mm.max_memory;
		regs->regs[2] = current->mm.huge_pages_count;
		break;
	}

	/* Phase 22: 设置内存配额
	 * x0 = max_bytes (0 表示无限制)
	 * 返回 x0 = 0 成功，-1 失败 */
	case HVC_MEM_SET_QUOTA: {
		unsigned long max_bytes = (unsigned long)regs->regs[0];
		regs->regs[0] = set_task_memory_quota(current, max_bytes);
		break;
	}

	/* Phase 22: 映射 2MB 大页
	 * x0 = ipa (2MB 对齐), x1 = prot (0=RW, 1=RO, 2=NONE)
	 * 返回 x0 = 0 成功，-1 失败 */
	case HVC_MEM_HUGE_MAP: {
		paddr_t ipa = (paddr_t)regs->regs[0];
		uint64_t prot = regs->regs[1];
		uint64_t ap;
		uint64_t flags;
		int npages = 1 << (LV2_SHIFT - PAGE_SHIFT);

		switch (prot) {
		case 0: ap = S2_AP_RW;   break;
		case 1: ap = S2_AP_RO;   break;
		case 2: ap = S2_AP_NONE; break;
		default:
			regs->regs[0] = -1;
			break;
		}
		if (prot > 2)
			break;

		/* 配额检查：分配 2MB */
		if (quota_check(current, SECTION_SIZE) < 0) {
			regs->regs[0] = -1;
			break;
		}

		/* 分配连续 512 个物理页（2MB） */
		paddr_t block_pa = allocate_block_pages(npages);
		if (!block_pa) {
			regs->regs[0] = -1;
			break;
		}

		flags = (MMU_STAGE2_BLOCK_FLAGS & ~S2_AP_MASK) | ap;
		regs->regs[0] = map_stage2_block(current, ipa, block_pa, flags);

		/* 如果映射失败，释放已分配的物理页 */
		if (regs->regs[0] != 0)
			free_block_pages(block_pa, npages);
		break;
	}

	/* Phase 22: 取消 2MB 大页映射
	 * x0 = ipa (2MB 对齐)
	 * 返回 x0 = 0 成功，-1 失败 */
	case HVC_MEM_HUGE_UNMAP: {
		paddr_t ipa = (paddr_t)regs->regs[0];
		regs->regs[0] = unmap_stage2_block(current, ipa, 1);
		break;
	}

	/* Phase 22: 修改 2MB 大页权限
	 * x0 = ipa (2MB 对齐), x1 = prot (0=RW, 1=RO, 2=NONE)
	 * 返回 x0 = 0 成功，-1 失败 */
	case HVC_MEM_HUGE_PROTECT: {
		paddr_t ipa = (paddr_t)regs->regs[0];
		uint64_t prot = regs->regs[1];
		uint64_t ap;

		switch (prot) {
		case 0: ap = S2_AP_RW;   break;
		case 1: ap = S2_AP_RO;   break;
		case 2: ap = S2_AP_NONE; break;
		default:
			regs->regs[0] = -1;
			break;
		}
		if (prot > 2)
			break;

		regs->regs[0] = mprotect_stage2_block(current, ipa, ap);
		break;
	}

	/* Phase 23: 查询页面回收统计
	 * 返回 x0 = 累计回收页数（4KB）, x1 = 回收失败次数 */
	case HVC_MEM_RECLAIM_STATS: {
		regs->regs[0] = current->mm.reclaim_count;
		regs->regs[1] = current->mm.reclaim_fail_count;
		break;
	}

	/* Phase 23: 主动回收页面
	 * x0 = 目标回收页数，返回 x0 = 实际回收页数 */
	case HVC_MEM_RECLAIM: {
		int nr = (int)regs->regs[0];
		regs->regs[0] = reclaim_pages(current, nr);
		break;
	}

	/* Phase 24: 触发 KSM 扫描合并
	 * x0 = 最大扫描页数 (0 = 全部扫描)
	 * 返回 x0 = 实际合并的页面数 */
	case HVC_MEM_KSM_SCAN: {
		int max_pages = (int)regs->regs[0];
		regs->regs[0] = ksm_scan_vm(current, max_pages);
		break;
	}

	/* Phase 24: 查询 KSM 统计
	 * 返回 x0=共享页数, x1=共享映射数, x2=累计合并, x3=累计分离 */
	case HVC_MEM_KSM_STATS: {
		struct ksm_stats stats;
		ksm_get_stats(&stats);
		regs->regs[0] = stats.pages_shared;
		regs->regs[1] = stats.pages_sharing;
		regs->regs[2] = stats.pages_merged;
		regs->regs[3] = stats.pages_unmerged;
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

	/* Phase 16: 虚拟 GICR 测试 hypercall——与 GICD_INJECT 相同，
	 * 底层都走 vgic_inject_virq（对 PPI 做 GICR enable 门控），
	 * 语义上区分 SPI/PPI，便于 guest 测试可读性。 */
	case HVC_GICR_INJECT:
		regs->regs[0] = vgic_inject_virq((uint32_t)regs->regs[0]);
		break;

	/* Phase 25: Live Migration 热迁移 hypercall */
	case HVC_MIGRATE_START:
		regs->regs[0] = migration_start(current);
		break;
	case HVC_MIGRATE_PRECOPY:
		regs->regs[0] = migration_precopy(current);
		break;
	case HVC_MIGRATE_FINISH:
		regs->regs[0] = migration_finish(current);
		break;
	case HVC_MIGRATE_STATUS:
		regs->regs[0] = migration_get_state();
		break;
	case HVC_SMP_FREEZE_BEGIN:
		regs->regs[0] = (current->vmid == 2 && current->vcpu_id == 0 &&
				 smp_processor_id() == 0)
				? smp_freeze_begin(1) : -1;
		break;
	case HVC_SMP_FREEZE_RELEASE:
		regs->regs[0] =
			(current->vmid == 2 && current->vcpu_id == 0 &&
			 smp_processor_id() == 0)
				? smp_freeze_release() : -1;
		break;
	case HVC_SMP_FREEZE_ACK:
		regs->regs[0] =
			(current->vmid == 2 && current->vcpu_id == 0 &&
			 smp_processor_id() == 0)
				? smp_freeze_ack((int)regs->regs[0]) : 0;
		break;
	case HVC_SMP_FREEZE_PROGRESS:
		regs->regs[0] =
			(current->vmid == 2 && current->vcpu_id == 0 &&
			 smp_processor_id() == 0)
				? smp_freeze_progress((int)regs->regs[0]) : 0;
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
#ifdef PLATFORM_VIRT
		/*
		 * Phase 17: 虚拟 SGI trap-and-emulate。
		 * ICH_HCR_EL2.TC=1 使 Guest 写 ICC_SGI1R_EL1（S3_0_C12_C11_5）
		 * trap 到这里，解析写入值并翻译成 vSGI 注入。
		 */
		if (op0 == 3 && op1 == 0 && crn == 12 && crm == 11 && op2 == 5) {
			handle_vgic_sgi1r(regs->regs[rt]);
			goto sys_fin;
		}
#endif
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
#define ESR_EL2_EC_IABT_LOW    32  /* Instruction Abort from lower EL */

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
	case ESR_EL2_EC_IABT_LOW:
		/* Instruction Abort from lower EL
		 * 处理方式与 Data Abort 类似：
		 *   - AF fault → 设置 AF 位
		 *   - Translation fault → demand paging
		 *   - Permission fault → 注入异常给 Guest
		 *
		 * 注意：FAR_EL2 是出错的指令虚拟地址，
		 * 用 get_ipa() 转换成 IPA 来查 Stage-2 表。
		 */
		if (handle_mem_abort(far, esr) < 0)
			PANIC("handle_instr_abort() failed.");
		break;
	default: {
		uint64_t hpfar = 0;
		asm volatile("mrs %0, hpfar_el2" : "=r"(hpfar));
		PANIC("uncaught synchronous exception:\n%s\n"
		      "esr=%lx elr_el2=%lx far_el2=%lx hpfar_el2=%lx\n"
		      "vmid=%d pc=0x%lx",
		      sync_error_reasons[eclass], esr, elr, far, hpfar,
		      current->pid, task_pt_regs(current)->pc);
		break;
	}
	}
}
