// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Guest - EL1 测试程序 (QEMU virt 平台)
 *
 * 这是运行在 Hypervisor 之上的第一个 Guest VM。
 * 它是一个极简的"裸机 OS"：
 *   - 运行在 EL1（Hypervisor 在 EL2）
 *   - 使用自己的 Stage-1 视角：MMU 关闭，VA == IPA
 *   - 不访问任何真实 MMIO 外设，所有服务来自 Hypervisor：
 *       * 控制台输出 → HVC 半虚拟化调用（HVC_CONSOLE_PUTC）
 *       * 定时器     → 架构虚拟定时器 CNTV（寄存器直通）
 *       * 中断       → GICv3 虚拟 CPU 接口（ICC_*_EL1 自动重定向）
 *
 * 做了什么：
 *   1. 打印启动 banner（通过 HVC）
 *   2. 初始化虚拟 GIC CPU 接口（PMR / IGRPEN1）
 *   3. 启动 10Hz 虚拟定时器，开 IRQ
 *   4. 主循环 WFI 睡眠（会 trap 给 EL2 顺便触发调度）
 *   5. 每秒打印一次心跳（证明虚拟定时器 + vGIC 注入正常工作）
 */

#pragma once

typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef int bool;

#define true  1
#define false 0

/* Guest 内存布局（IPA，Hypervisor 通过 Stage 2 映射到真实物理页） */
#define GUEST_LOAD_ADDR  0x40000000UL /* 镜像加载/入口地址 */
#define GUEST_STACK_TOP  0x40400000UL /* 栈顶（4MB 处） */

/* QEMU virt 架构定时器频率（也可以用 HVC_GET_TIMER_FREQ 查询） */
#define GUEST_TIMER_HZ   10UL         /* 虚拟定时器：10 Hz */

/* 虚拟定时器 PPI 中断号（GICv3 规范，Guest 视角） */
#define VIRT_TIMER_IRQ   27

/* Hypercall 功能号（与 include/common/hvc.h 保持一致） */
#define HVC_CONSOLE_PUTC   1
#define HVC_GET_TIMER_FREQ 2
#define HVC_GET_VMID       3
#define HVC_PCI_DEBUG      4

/* Phase 10: 新增 hypercall */
#define HVC_SCHED_YIELD     10
#define HVC_SCHED_GET_PRIO  11
#define HVC_SCHED_SET_PRIO  12
#define HVC_MEM_MAP         20
#define HVC_MEM_UNMAP       21
#define HVC_SHM_CREATE      30
#define HVC_SHM_ATTACH      31
#define HVC_SHM_DETACH      32
#define HVC_SHM_NOTIFY      33

/* 共享内存 notify 使用的虚拟中断号 */
#define SHM_NOTIFY_VIRQ     60

/* ---- 内联汇编访问系统寄存器 ---- */

static inline void write_sysreg(u64 val, const char *name)
{
	asm volatile("msr %0, %1" ::"r"(val), "i"(0) : "memory");
}

#define SYSREG_WRITE(reg, val)                                                \
	do {                                                                  \
		u64 _v = (val);                                               \
		asm volatile("msr " #reg ", %0" ::"r"(_v) : "memory");        \
	} while (0)

#define SYSREG_READ(reg)                                                      \
	({                                                                    \
		u64 _v;                                                        \
		asm volatile("mrs %0, " #reg : "=r"(_v));                      \
		_v;                                                            \
	})

/* GICv3 CPU 接口寄存器（在 EL1 访问时自动重定向到虚拟接口） */
#define ICC_PMR_EL1_VAL     SYSREG_READ(ICC_PMR_EL1)
#define icc_pmr_write(v)    SYSREG_WRITE(S3_0_C4_C6_0, v)
#define icc_igrpen1_write(v) SYSREG_WRITE(S3_0_C12_C12_7, v)
#define icc_iar1_read()     SYSREG_READ(S3_0_C12_C12_0)
#define icc_eoir1_write(v)  SYSREG_WRITE(S3_0_C12_C12_1, v)

/* 虚拟定时器寄存器（直通，不 trap） */
#define cntvct_read()       SYSREG_READ(CNTVCT_EL0)
#define cntv_ctl_read()     SYSREG_READ(CNTV_CTL_EL0)
#define cntv_ctl_write(v)   SYSREG_WRITE(CNTV_CTL_EL0, v)
#define cntv_cval_write(v)  SYSREG_WRITE(CNTV_CVAL_EL0, v)

/* ---- Hypercall ---- */

/*
 * HVC 调用：x8 = 功能号，x0 = 参数
 * （与 Hypervisor 的 handle_hvc64 约定一致）
 */
static inline u64 hvc_call(u64 nr, u64 arg)
{
	register u64 x0 __asm__("x0") = arg;
	register u64 x8 __asm__("x8") = nr;

	asm volatile("hvc #0"
		     : "+r"(x0)
		     : "r"(x8)
		     : "memory");

	return x0;
}

/* 两参数版本的 hvc_call（x0, x1） */
static inline u64 hvc_call2(u64 nr, u64 arg0, u64 arg1)
{
	register u64 x0 __asm__("x0") = arg0;
	register u64 x1 __asm__("x1") = arg1;
	register u64 x8 __asm__("x8") = nr;

	asm volatile("hvc #0"
		     : "+r"(x0)
		     : "r"(x1), "r"(x8)
		     : "memory");

	return x0;
}

static inline void hvc_putc(char c)
{
	hvc_call(HVC_CONSOLE_PUTC, (u64)c);
}

void guest_puts(const char *s);
void guest_fatal(const char *msg, u64 esr, u64 elr);
u64 guest_timer_freq(void);
u64 guest_vmid(void);

/* 入口（start.S） */
extern char vectors;

void guest_main(void);
