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
#define HVC_MEM_STATS       22  /* 返回 x0=user_pages x1=kernel_pages */
#define HVC_MEM_PROTECT     23  /* 修改页权限：x0=ipa x1=prot(0=RW 1=RO 2=NONE)，返回 0/-1 */

/* Phase 22: 内存超售与大页 hypercall */
#define HVC_MEM_QUERY        24  /* 查询内存：返回 x0=使用(字节) x1=配额(字节) x2=大页数 */
#define HVC_MEM_SET_QUOTA    25  /* 设置配额：x0=max_bytes，返回 0/-1 */
#define HVC_MEM_HUGE_MAP     26  /* 映射 2MB 大页：x0=ipa x1=prot，返回 0/-1 */
#define HVC_MEM_HUGE_UNMAP   27  /* 取消 2MB 大页：x0=ipa，返回 0/-1 */
#define HVC_MEM_HUGE_PROTECT 28  /* 修改 2MB 大页权限：x0=ipa x1=prot，返回 0/-1 */

/* Phase 23: 页面回收 hypercall */
#define HVC_MEM_RECLAIM_STATS 29  /* 查询回收统计：返回 x0=回收页数 x1=失败次数 */
#define HVC_MEM_RECLAIM       34  /* 主动回收：x0=目标页数，返回实际回收页数 */

/* Phase 24: KSM 内存去重 */
#define HVC_MEM_KSM_SCAN      35  /* 触发 KSM 扫描：x0=最大页数，返回合并页数 */
#define HVC_MEM_KSM_STATS     36  /* 查询 KSM 统计：返回 x0=共享页 x1=共享映射数 x2=累计合并 x3=累计分离 */

#define HVC_SHM_CREATE      30
#define HVC_SHM_ATTACH      31
#define HVC_SHM_DETACH      32
#define HVC_SHM_NOTIFY      33

/* Phase 12: 虚拟 SGI（vSGI）hypercall */
#define HVC_VSGI_SEND       40  /* x0=target_vcpu_id x1=sgi_id，返回 0/-1 */
#define HVC_VSGI_GET_CPUID  41  /* 返回当前 vCPU ID */

/* Phase 15: 虚拟 GICD 测试 hypercall */
#define HVC_GICD_INJECT     42  /* x0=vintid，直接注入虚拟 SPI，返回 0/-1 */

/* Phase 16: 虚拟 GICR 测试 hypercall */
#define HVC_GICR_INJECT     43  /* x0=vPPI(16-31)，直接经 vgic_inject_virq 注入，返回 0/-1 */

/* 共享内存 notify 使用的虚拟中断号 */
#define SHM_NOTIFY_VIRQ     60

/* Phase 15: 虚拟 GICD 测试用虚拟 SPI（避开 48/49/50/60 已有设备 SPI） */
#define VGICD_TEST_VIRQ     52

/* Phase 16: 虚拟 GICR 测试用虚拟 PPI（避开 vtimer=27） */
#define VGICR_TEST_PPI      24

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
/* Phase 17: 标准 SGI 发送（写 ICC_SGI1R_EL1，触发 Hypervisor trap-and-emulate） */
#define icc_sgi1r_write(v)  SYSREG_WRITE(S3_0_C12_C11_5, v)

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

/* Phase 20: 返回 x0 和 x1 两个返回值的 hvc_call
 * 用于一次返回两个值的 hypercall（如 HVC_MEM_STATS） */
static inline u64 hvc_call_ret2(u64 nr, u64 arg0, u64 *ret1)
{
	register u64 x0 __asm__("x0") = arg0;
	register u64 x1 __asm__("x1");
	register u64 x8 __asm__("x8") = nr;

	asm volatile("hvc #0"
		     : "+r"(x0), "=r"(x1)
		     : "r"(x8)
		     : "memory");

	if (ret1)
		*ret1 = x1;
	return x0;
}

/* Phase 22: 返回 x0, x1, x2 三个返回值的 hvc_call */
static inline u64 hvc_call_ret3(u64 nr, u64 arg0, u64 *ret1, u64 *ret2)
{
	register u64 x0 __asm__("x0") = arg0;
	register u64 x1 __asm__("x1");
	register u64 x2 __asm__("x2");
	register u64 x8 __asm__("x8") = nr;

	asm volatile("hvc #0"
		     : "+r"(x0), "=r"(x1), "=r"(x2)
		     : "r"(x8)
		     : "memory");

	if (ret1)
		*ret1 = x1;
	if (ret2)
		*ret2 = x2;
	return x0;
}

/* Phase 24: 返回 x0, x1, x2, x3 四个返回值的 hvc_call */
static inline u64 hvc_call_ret4(u64 nr, u64 arg0, u64 *ret1, u64 *ret2, u64 *ret3)
{
	register u64 x0 __asm__("x0") = arg0;
	register u64 x1 __asm__("x1");
	register u64 x2 __asm__("x2");
	register u64 x3 __asm__("x3");
	register u64 x8 __asm__("x8") = nr;

	asm volatile("hvc #0"
		     : "+r"(x0), "=r"(x1), "=r"(x2), "=r"(x3)
		     : "r"(x8)
		     : "memory");

	if (ret1)
		*ret1 = x1;
	if (ret2)
		*ret2 = x2;
	if (ret3)
		*ret3 = x3;
	return x0;
}

static inline void hvc_putc(char c)
{
	hvc_call(HVC_CONSOLE_PUTC, (u64)c);
}

/* Phase 12: vSGI 辅助函数（依赖 hvc_call，放在后面） */
static inline int vsgi_send(int target_vcpu, int sgi_id)
{
    return (int)hvc_call2(HVC_VSGI_SEND, (u64)target_vcpu, (u64)sgi_id);
}

static inline int vsgi_get_cpuid(void)
{
    return (int)hvc_call(HVC_VSGI_GET_CPUID, 0);
}

void guest_puts(const char *s);
void guest_fatal(const char *msg, u64 esr, u64 elr);
u64 guest_timer_freq(void);
u64 guest_vmid(void);

/* 入口（start.S） */
extern char vectors;

void guest_main(void);

/* ---- Phase 11: SMP 支持 ---- */

/*
 * Ticket 自旋锁（FIFO 公平锁）
 *
 * 两个 16 位字段：
 *   - owner: 当前服务的 ticket（谁持有锁）
 *   - next:  下一个可用的 ticket
 *
 * 取锁：原子地取走 next ticket，然后自旋等待 owner == 自己的 ticket
 * 放锁：owner++（下一个 ticket 持有者获得锁）
 */
typedef struct spinlock {
    volatile u32 owner;   /* 当前持锁 ticket */
    volatile u32 next;    /* 下一个 ticket */
} spinlock_t;

#define SPINLOCK_INIT { 0, 0 }

static inline void spin_lock_init(spinlock_t *lock)
{
    lock->owner = 0;
    lock->next = 0;
}

static inline void spin_lock(spinlock_t *lock)
{
    /* 原子地获取 next ticket（ldaxr + stlxr = acquire-release 语义） */
    u32 ticket;
    u32 new_next;
    u32 status;
    asm volatile(
        "1: ldaxr %w0, [%3]\n"      /* 原子读 next (acquire) -> ticket */
        "   add %w1, %w0, #1\n"     /* new_next = ticket + 1 */
        "   stlxr %w2, %w1, [%3]\n" /* 原子写回 next (release)，status = 0/1 */
        "   cbnz %w2, 1b\n"         /* 失败重试 */
        : "=&r"(ticket), "=&r"(new_next), "=&r"(status)
        : "r"(&lock->next)
        : "memory"
    );
    (void)new_next;

    /* 自旋等待 owner == ticket */
    while (lock->owner != ticket) {
        asm volatile("yield" ::: "memory");
    }
    asm volatile("dmb ish" ::: "memory");  /* 确保锁内操作在获取锁之后 */
}

static inline void spin_unlock(spinlock_t *lock)
{
    asm volatile("dmb ish" ::: "memory");  /* 确保锁内操作在释放锁之前 */
    lock->owner++;
}

/* SMP 测试用的共享变量（通过固定地址共享）
 *
 * 内存布局（在 0x40300000 处的一页内）：
 *   offset 0x00: ready_flag (u64)   主核初始化后置 1
 *   offset 0x08: done_flag  (u64)   次核完成后置 1
 *   offset 0x10: counter    (u64)   共享计数器
 *   offset 0x18: lock       (spinlock_t)  自旋锁
 */
#define SMP_TEST_ADDR        0x40300000UL
#define SMP_READY_FLAG_OFF   0x00
#define SMP_DONE_FLAG_OFF    0x08
#define SMP_COUNTER_OFF      0x10
#define SMP_LOCK_OFF         0x18
#define SMP_BOOT_DONE_OFF    0x38    /* 主核完成 BSS 清零 + 页表建立后置 1 */
#define SMP_ITERATIONS       10000     /* 每核累加次数 */

/* ---- Phase 12: vSGI 测试共享内存布局 ----
 *
 * 继续使用 0x40300000 页面（和 SMP 测试复用）：
 *   offset 0x20: vsgi_received (u64)   vSGI 收到标志 bitmap
 *   offset 0x28: vsgi_count    (u64)   vSGI 收到次数
 *   offset 0x30: vsgi_sender   (u64)   上次发送者 vcpu_id（预留）
 */
#define VSGI_RECEIVED_OFF   0x20
#define VSGI_COUNT_OFF      0x28
#define VSGI_SENDER_OFF     0x30
#define VSGI_TEST_SGI_ID    5     /* 用 SGI#5 做测试 */

/* Phase 17: 虚拟 SGI trap-and-emulate 测试用 SGI 号（区别于 Phase 12 的 #5） */
#define VSGI_TRAP_TEST_SGI_ID 6
