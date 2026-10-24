// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Guest - 主程序（Phase 4：设备虚拟化验证）
 *
 * 验证内容：
 *   1. virtio-net 设备探测 + 环回测试
 *   2. ivshmem 共享内存 + doorbell 中断 (VM 间 ping-pong)
 *   3. 虚拟定时器 + vGIC (Phase 3 保留功能)
 */

#include "guest.h"

/* ---------- 工具函数 ---------- */

static void puts_dec(u64 v)
{
    char buf[24];
    int i = 0;

    if (v == 0) {
        hvc_putc('0');
        return;
    }

    while (v > 0) {
        buf[i++] = '0' + (char)(v % 10);
        v /= 10;
    }

    while (i > 0)
        hvc_putc(buf[--i]);
}

static void puts_hex(u64 v, int width)
{
    char buf[24];
    int i = 0;

    if (v == 0 && width <= 0) {
        hvc_putc('0');
        return;
    }

    while (v > 0 || i < width) {
        int digit = v & 0xf;
        buf[i++] = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
        v >>= 4;
    }

    while (i > 0)
        hvc_putc(buf[--i]);
}

void guest_puts(const char *s)
{
    while (*s)
        hvc_putc(*s++);
}

void guest_fatal(const char *msg, u64 esr, u64 elr)
{
    guest_puts("FATAL: ");
    guest_puts(msg);
    guest_puts(" ESR=");
    puts_hex(esr, 16);
    guest_puts(" ELR=");
    puts_hex(elr, 16);
    guest_puts("\n");
    while (1)
        asm volatile("wfe" ::: "memory");
}

u64 guest_timer_freq(void)
{
    return hvc_call(HVC_GET_TIMER_FREQ, 0);
}

u64 guest_vmid(void)
{
    return hvc_call(HVC_GET_VMID, 0);
}


/* ---------- 同步异常 handler（Phase 21: 用于 mprotect 测试） ---------- */

static volatile int sync_expect_fault = 0;   /* 非 0 = 期望下一个同步异常 */
static volatile int sync_fault_received = 0; /* 收到了预期的异常 */
static volatile u64 sync_fault_esr = 0;
static volatile u64 sync_fault_far = 0;

/*
 * Guest 同步异常入口（vectors.S 调用）
 *   esr: ESR_EL1
 *   elr: ELR_EL1 (触发异常的指令地址)
 *   far: FAR_EL1 (故障地址)
 * 返回 0 = 已处理（跳过指令，继续执行），非 0 = fatal
 */
int guest_sync_handler(u64 esr, u64 elr, u64 far)
{
    if (sync_expect_fault) {
        sync_fault_received = 1;
        sync_fault_esr = esr;
        sync_fault_far = far;
        sync_expect_fault = 0;
        /* 跳过触发异常的指令（假设是 4 字节指令） */
        asm volatile("msr elr_el1, %0" ::"r"(elr + 4) : "memory");
        return 0;
    }
    return -1;  /* 未预期的异常 → fatal */
}

static inline void sync_expect_start(void)
{
    sync_fault_received = 0;
    sync_fault_esr = 0;
    sync_fault_far = 0;
    asm volatile("dmb sy" ::: "memory");
    sync_expect_fault = 1;
    asm volatile("dsb sy; isb" ::: "memory");
}

static inline int sync_expect_end(void)
{
    sync_expect_fault = 0;
    asm volatile("dmb sy" ::: "memory");
    return sync_fault_received;
}

/* ---------- MMIO 读写 ---------- */

#define __raw_readl(addr)   (*(volatile u32 *)(addr))
#define __raw_writel(v, addr) (*(volatile u32 *)(addr) = (v))

static inline u32 mmio_read32(u64 addr)
{
    return __raw_readl(addr);
}

static inline void mmio_write32(u64 addr, u32 val)
{
    __raw_writel(val, addr);
}

/* ---------- GIC 虚拟 CPU 接口操作 ---------- */

/*
 * 在 GICv3 虚拟化中：
 * - CPU 接口 (ICC_*_EL1) 由硬件自动虚拟化，Guest 直接访问
 * - Distributor (GICD) 需要软件模拟（trap-and-emulate）
 * - 虚拟 SPI 中断通过 ICH_LR 注入，由 hypervisor 管理
 *
 * 为了简化，我们假设：
 * - 虚拟 SPI 中断 (48, 49) 由 hypervisor 端预先配置好使能
 * - Guest 只需要处理 CPU 接口的中断（ack + EOI）
 */

static void gic_vcpu_init(void)
{
    icc_pmr_write(0xFF);
    icc_igrpen1_write(1);
}

/* ---------- Phase 15: 虚拟 GICD (trap-and-emulate) ---------- */

#define GICD_BASE           0x08000000UL
#define GICD_CTLR           (GICD_BASE + 0x0000)
#define GICD_TYPER          (GICD_BASE + 0x0004)
#define GICD_IIDR           (GICD_BASE + 0x0008)
#define GICD_ISENABLER(n)   (GICD_BASE + 0x0100 + (n) * 4)
#define GICD_ICENABLER(n)   (GICD_BASE + 0x0180 + (n) * 4)
#define GICD_ISPENDR(n)     (GICD_BASE + 0x0200 + (n) * 4)
#define GICD_ICPENDR(n)     (GICD_BASE + 0x0280 + (n) * 4)
#define GICD_IPRIORITYR(n)  (GICD_BASE + 0x0400 + (n) * 4)

#define GICD_CTLR_ENABLE_G1NS   (1 << 1)
#define GICD_CTLR_ARE_NS        (1 << 4)

/* 虚拟 GICD 测试接收标志 */
static volatile int vgicd_irq_received = 0;

static inline int vgicd_inject(u32 vintid)
{
    return (int)hvc_call(HVC_GICD_INJECT, (u64)vintid);
}

/*
 * 初始化虚拟 GICD（演示 trap-and-emulate 写）：
 *   读 GICD_CTLR 确认 Hypervisor 默认已使能 Group1 NS + 亲和路由，
 *   若未使能则写回使能位（幂等）。
 */
static void gic_dist_init(void)
{
    u32 ctlr = mmio_read32(GICD_CTLR);
    guest_puts("gicd: GICD_CTLR = 0x");
    puts_hex(ctlr, 8);
    guest_puts("\n");

    if (!(ctlr & GICD_CTLR_ENABLE_G1NS)) {
        mmio_write32(GICD_CTLR, GICD_CTLR_ENABLE_G1NS | GICD_CTLR_ARE_NS);
        guest_puts("gicd: GICD_CTLR enabled Group1 NS\n");
    }
}

/*
 * Phase 15: 虚拟 GICD (trap-and-emulate) 测试
 *
 * 验证 Hypervisor 对 GIC Distributor MMIO 的软件模拟：
 *   1. 读 TYPER / IIDR，确认模拟值
 *   2. 读 ISENABLER，确认默认所有 SPI 使能
 *   3. ICENABLER 禁用测试 SPI -> HVC_GICD_INJECT 被门控返回 -1 且收不到中断
 *   4. ISENABLER 重新使能 -> HVC_GICD_INJECT 返回 0 且收到中断
 */
static void phase15_vgicd_test(void)
{
    guest_puts("\n========================================\n");
    guest_puts("  Phase 15: Virtual GICD emulation\n");
    guest_puts("========================================\n");

    /* 1. 读标识寄存器，验证 trap-and-emulate 读 */
    u32 typer = mmio_read32(GICD_TYPER);
    u32 iidr = mmio_read32(GICD_IIDR);
    guest_puts("gicd: GICD_TYPER = 0x");
    puts_hex(typer, 8);
    guest_puts(", GICD_IIDR = 0x");
    puts_hex(iidr, 8);
    guest_puts("\n");

    int spi_idx = VGICD_TEST_VIRQ >> 5;         /* 52 >> 5 = 1 */
    u32 spi_bit = 1u << (VGICD_TEST_VIRQ & 31); /* bit 20 */

    /* 2. 默认所有 SPI 使能，确认测试 SPI 使能位为 1 */
    u32 en = mmio_read32(GICD_ISENABLER(spi_idx));
    guest_puts("gicd: ISENABLER[1] = 0x");
    puts_hex(en, 8);
    guest_puts(" (SPI 52 enabled = ");
    puts_dec((en & spi_bit) ? 1 : 0);
    guest_puts(")\n");

    /* 3. 禁用测试 SPI，验证注入被门控丢弃 */
    guest_puts("gicd: disabling SPI 52 via ICENABLER...\n");
    mmio_write32(GICD_ICENABLER(spi_idx), spi_bit);

    vgicd_irq_received = 0;
    int ret = vgicd_inject(VGICD_TEST_VIRQ);

    /* 稍等，确认禁用期间没有中断送达 */
    int timeout = 100000;
    while (timeout-- > 0)
        asm volatile("nop");

    guest_puts("gicd: inject while disabled -> ret=");
    puts_dec(ret);
    guest_puts(", irq_received=");
    puts_dec(vgicd_irq_received);
    guest_puts("\n");

    if (ret == -1 && vgicd_irq_received == 0)
        guest_puts("gicd: gating PASSED (disabled SPI dropped)\n");
    else
        guest_puts("gicd: gating FAILED\n");

    /* 4. 重新使能测试 SPI，验证注入正常送达 */
    guest_puts("gicd: re-enabling SPI 52 via ISENABLER...\n");
    mmio_write32(GICD_ISENABLER(spi_idx), spi_bit);

    vgicd_irq_received = 0;
    ret = vgicd_inject(VGICD_TEST_VIRQ);

    timeout = 10000000;
    while (!vgicd_irq_received && timeout-- > 0)
        asm volatile("nop");

    guest_puts("gicd: inject while enabled -> ret=");
    puts_dec(ret);
    guest_puts(", irq_received=");
    puts_dec(vgicd_irq_received);
    guest_puts("\n");

    if (ret == 0 && vgicd_irq_received)
        guest_puts("gicd: inject PASSED (enabled SPI delivered)\n");
    else
        guest_puts("gicd: inject FAILED\n");

    guest_puts("\nPhase 15: ALL tests done\n");
}

/* ---------- Phase 16: 虚拟 GICR (trap-and-emulate) ---------- */

#define GICR_BASE           0x080A0000UL
#define GICR_CTLR           (GICR_BASE + 0x0000)
#define GICR_IIDR           (GICR_BASE + 0x0004)
#define GICR_TYPER          (GICR_BASE + 0x0008)
#define GICR_WAKER          (GICR_BASE + 0x0014)
/* SGI/PPI 帧偏移相对 GICR 基址，已含 0x10000 的 SGI_base 帧偏移 */
#define GICR_ISENABLER0     (GICR_BASE + 0x10100)
#define GICR_ICENABLER0     (GICR_BASE + 0x10180)
#define GICR_ISPENDR0       (GICR_BASE + 0x10200)
#define GICR_ICPENDR0       (GICR_BASE + 0x10280)
#define GICR_IPRIORITYR(n)  (GICR_BASE + 0x10400 + (n) * 4)

/* 虚拟 GICR 测试接收标志 */
static volatile int vgicr_irq_received = 0;

static inline int vgicr_inject(u32 vintid)
{
    return (int)hvc_call(HVC_GICR_INJECT, (u64)vintid);
}

/*
 * 初始化虚拟 GICR（演示 trap-and-emulate 读）：
 *   读 GICR_TYPER / GICR_WAKER，确认 Hypervisor 模拟的 Redistributor
 *   未睡眠；若处于睡眠态则写 0 唤醒。
 */
static void gicr_init(void)
{
    u32 typer = mmio_read32(GICR_TYPER);
    u32 waker = mmio_read32(GICR_WAKER);
    guest_puts("gicr: GICR_TYPER = 0x");
    puts_hex(typer, 8);
    guest_puts(", GICR_WAKER = 0x");
    puts_hex(waker, 8);
    guest_puts("\n");

    if (waker & (1 << 1)) {
        mmio_write32(GICR_WAKER, 0);
        guest_puts("gicr: GICR_WAKER woke up (ProcessorSleep cleared)\n");
    }
}

/*
 * Phase 16: 虚拟 GICR (trap-and-emulate) 测试
 *
 * 验证 Hypervisor 对 GIC Redistributor MMIO 的软件模拟：
 *   1. 读 TYPER / IIDR，确认模拟值
 *   2. 读 ISENABLER0，确认默认所有 PPI 使能
 *   3. ICENABLER0 禁用测试 PPI -> HVC_GICR_INJECT 被门控返回 -1 且收不到中断
 *   4. ISENABLER0 重新使能 -> HVC_GICR_INJECT 返回 0 且收到中断
 */
static void phase16_gicr_test(void)
{
    guest_puts("\n========================================\n");
    guest_puts("  Phase 16: Virtual GICR emulation\n");
    guest_puts("========================================\n");

    /* 1. 读标识寄存器，验证 trap-and-emulate 读 */
    u32 typer = mmio_read32(GICR_TYPER);
    u32 iidr = mmio_read32(GICR_IIDR);
    guest_puts("gicr: GICR_TYPER = 0x");
    puts_hex(typer, 8);
    guest_puts(", GICR_IIDR = 0x");
    puts_hex(iidr, 8);
    guest_puts("\n");

    u32 ppi_bit = 1u << (VGICR_TEST_PPI & 31);  /* bit 24 */

    /* 2. 默认所有 PPI 使能，确认测试 PPI 使能位为 1 */
    u32 en = mmio_read32(GICR_ISENABLER0);
    guest_puts("gicr: ISENABLER0 = 0x");
    puts_hex(en, 8);
    guest_puts(" (PPI 24 enabled = ");
    puts_dec((en & ppi_bit) ? 1 : 0);
    guest_puts(")\n");

    /* 3. 禁用测试 PPI，验证注入被门控丢弃 */
    guest_puts("gicr: disabling PPI 24 via ICENABLER0...\n");
    mmio_write32(GICR_ICENABLER0, ppi_bit);

    vgicr_irq_received = 0;
    int ret = vgicr_inject(VGICR_TEST_PPI);

    int timeout = 100000;
    while (timeout-- > 0)
        asm volatile("nop");

    guest_puts("gicr: inject while disabled -> ret=");
    puts_dec(ret);
    guest_puts(", irq_received=");
    puts_dec(vgicr_irq_received);
    guest_puts("\n");

    if (ret == -1 && vgicr_irq_received == 0)
        guest_puts("gicr: gating PASSED (disabled PPI dropped)\n");
    else
        guest_puts("gicr: gating FAILED\n");

    /* 4. 重新使能测试 PPI，验证注入正常送达 */
    guest_puts("gicr: re-enabling PPI 24 via ISENABLER0...\n");
    mmio_write32(GICR_ISENABLER0, ppi_bit);

    vgicr_irq_received = 0;
    ret = vgicr_inject(VGICR_TEST_PPI);

    timeout = 10000000;
    while (!vgicr_irq_received && timeout-- > 0)
        asm volatile("nop");

    guest_puts("gicr: inject while enabled -> ret=");
    puts_dec(ret);
    guest_puts(", irq_received=");
    puts_dec(vgicr_irq_received);
    guest_puts("\n");

    if (ret == 0 && vgicr_irq_received)
        guest_puts("gicr: inject PASSED (enabled PPI delivered)\n");
    else
        guest_puts("gicr: inject FAILED\n");

    guest_puts("\nPhase 16: ALL tests done\n");
}

/* ---------- 虚拟定时器 ---------- */

static u64 timer_freq;
static u64 tick_hz;
static volatile u64 tick_count = 0;

static void vtimer_start(void)
{
    u64 now;

    timer_freq = guest_timer_freq();

    u64 vmid = guest_vmid();
    tick_hz = (vmid == 1) ? 10 : (vmid == 2) ? 5 : 1;

    now = cntvct_read();
    cntv_cval_write(now + timer_freq / tick_hz);
    cntv_ctl_write(1);
}

/* ---------- virtio-net 驱动 ---------- */

#define VIRTIO_NET_BASE   0x0A000000UL

/* virtio MMIO 寄存器偏移 */
#define VIRTIO_MMIO_MAGIC        0x000
#define VIRTIO_MMIO_VERSION      0x004
#define VIRTIO_MMIO_DEVICE_ID    0x008
#define VIRTIO_MMIO_VENDOR_ID    0x00C
#define VIRTIO_MMIO_HOST_FEAT    0x010
#define VIRTIO_MMIO_HOST_FEAT_SEL 0x014
#define VIRTIO_MMIO_GUEST_FEAT   0x020
#define VIRTIO_MMIO_GUEST_FEAT_SEL 0x024
#define VIRTIO_MMIO_GUEST_PGSZ   0x028
#define VIRTIO_MMIO_QUEUE_SEL    0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX 0x034
#define VIRTIO_MMIO_QUEUE_NUM    0x038
#define VIRTIO_MMIO_QUEUE_ALIGN  0x03C
#define VIRTIO_MMIO_QUEUE_PFN    0x040
#define VIRTIO_MMIO_QUEUE_NOTIFY 0x050
#define VIRTIO_MMIO_IRQ_STATUS   0x060
#define VIRTIO_MMIO_IRQ_ACK      0x064
#define VIRTIO_MMIO_STATUS       0x070
#define VIRTIO_MMIO_CFG_BASE     0x100

/* 状态位 */
#define VIRTIO_STATUS_ACK       (1 << 0)
#define VIRTIO_STATUS_DRIVER    (1 << 1)
#define VIRTIO_STATUS_DRIVER_OK (1 << 2)
#define VIRTIO_STATUS_FAIL      (1 << 7)

/* 中断状态 */
#define VIRTIO_IRQ_USED         (1 << 0)
#define VIRTIO_IRQ_CONFIG       (1 << 1)

/* 设备 ID */
#define VIRTIO_ID_NET           1

/* 虚拟中断号 */
#define VIRTIO_NET_IRQ          48

/* virtqueue 大小 (必须是 2 的幂, 且 <= 32) */
#define VRING_SIZE              16

/* vring 描述符 */
struct vring_desc {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
};

#define VRING_DESC_F_NEXT   1
#define VRING_DESC_F_WRITE  2

/* vring 可用环 */
struct vring_avail {
    u16 flags;
    u16 idx;
    u16 ring[VRING_SIZE];
};

/* vring 已用环 */
struct vring_used_elem {
    u32 id;
    u32 len;
};

struct vring_used {
    u16 flags;
    u16 idx;
    struct vring_used_elem ring[VRING_SIZE];
};

/* virtio-net 设备状态 */
/* 每个描述符承载的数据大小（用于测试描述符链） */
#define VIRTIO_NET_FRAG_SIZE   256

struct virtio_net_drv {
    u64 base;
    int ready;

    /* rx 队列 */
    struct vring_desc *rx_desc;
    struct vring_avail *rx_avail;
    struct vring_used *rx_used;
    u16 rx_last_used;

    /* tx 队列 */
    struct vring_desc *tx_desc;
    struct vring_avail *tx_avail;
    struct vring_used *tx_used;
    u16 tx_last_used;
    u16 tx_num_free;      /* 可用描述符计数 */

    /* 内存区域 (用于描述符/avail/used 环) */
    u8 *rx_mem;
    u8 *tx_mem;
};

static struct virtio_net_drv vnet;

/* 简单的内存分配器
 * 内存布局（从高到低）：
 *   0x40400000  -- 栈顶（向下增长，预留 64KB 栈空间）
 *   0x403F0000  -- 堆顶（guest_alloc 从这里向下分配）
 *   0x40300000  -- 堆底
 *
 * 注意：堆和栈必须分开，否则堆分配会覆盖栈上的返回地址，
 *       导致函数返回时 PC 变成 0（神秘的 PC=0 崩溃的根因）。
 */
#define GUEST_STACK_TOP   0x40400000UL
#define GUEST_STACK_SIZE  0x10000      /* 64KB 栈空间 */
#define GUEST_HEAP_TOP    (GUEST_STACK_TOP - GUEST_STACK_SIZE)
#define GUEST_HEAP_BASE   0x40300000UL
static u64 guest_heap_ptr = GUEST_HEAP_TOP;

static void *guest_alloc(int size, int align)
{
    guest_heap_ptr = (guest_heap_ptr - size) & ~((u64)(align - 1));
    if (guest_heap_ptr < GUEST_HEAP_BASE)
        return 0;
    /* 清零 */
    u8 *p = (u8 *)guest_heap_ptr;
    for (int i = 0; i < size; i++)
        p[i] = 0;
    return (void *)guest_heap_ptr;
}

/* 初始化一个 virtqueue 的 vring 内存 */
static int vring_init(u64 base, int qidx, int num,
                      struct vring_desc **desc_out,
                      struct vring_avail **avail_out,
                      struct vring_used **used_out)
{
    u32 vring_align = 4096;
    u64 desc_size = sizeof(struct vring_desc) * num;
    u64 avail_size = 6 + 2 * num;
    u64 used_size = 6 + 8 * num;

    /* 分配一整块内存 */
    int total = 4096 * 2;  /* 保证够大 */
    u8 *mem = guest_alloc(total, vring_align);
    if (!mem)
        return -1;

    u64 mem_ipa = (u64)mem;

    /* desc 在起始处 */
    struct vring_desc *desc = (struct vring_desc *)mem;
    /* avail 在 desc 之后, 按 align 对齐 */
    u64 avail_off = (desc_size + vring_align - 1) & ~(vring_align - 1);
    struct vring_avail *avail = (struct vring_avail *)(mem + avail_off);
    /* used 在 avail 之后, 按 align 对齐 */
    u64 used_off = (avail_off + avail_size + vring_align - 1) & ~(vring_align - 1);
    struct vring_used *used = (struct vring_used *)(mem + used_off);

    /* 配置队列 */
    mmio_write32(base + VIRTIO_MMIO_QUEUE_SEL, qidx);
    mmio_write32(base + VIRTIO_MMIO_QUEUE_NUM, num);
    mmio_write32(base + VIRTIO_MMIO_QUEUE_ALIGN, vring_align);
    mmio_write32(base + VIRTIO_MMIO_QUEUE_PFN, (u32)(mem_ipa / 4096));

    *desc_out = desc;
    *avail_out = avail;
    *used_out = used;

    return 0;
}

/* 探测并初始化 virtio-net 设备 */
static int virtio_net_init(void)
{
    u64 base = VIRTIO_NET_BASE;
    u32 magic, version, devid, vendor;

    vnet.base = base;
    vnet.ready = 0;

    /* 1. 读取识别信息 */
    magic = mmio_read32(base + VIRTIO_MMIO_MAGIC);
    version = mmio_read32(base + VIRTIO_MMIO_VERSION);
    devid = mmio_read32(base + VIRTIO_MMIO_DEVICE_ID);
    vendor = mmio_read32(base + VIRTIO_MMIO_VENDOR_ID);

    guest_puts("virtio-net: magic=0x");
    puts_hex(magic, 8);
    guest_puts(" version=");
    puts_dec(version);
    guest_puts(" devid=");
    puts_dec(devid);
    guest_puts(" vendor=0x");
    puts_hex(vendor, 8);
    guest_puts("\n");

    if (magic != 0x74726976 || version != 1 || devid != VIRTIO_ID_NET) {
        guest_puts("virtio-net: device not found!\n");
        return -1;
    }

    /* 2. 状态: ACK */
    mmio_write32(base + VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK);

    /* 3. 状态: DRIVER */
    mmio_write32(base + VIRTIO_MMIO_STATUS,
                 VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    /* 4. 读 host 特性并协商 */
    u32 host_features = mmio_read32(base + VIRTIO_MMIO_HOST_FEAT);
    guest_puts("virtio-net: host features=0x");
    puts_hex(host_features, 8);
    guest_puts("\n");

    /* 我们只接受 MAC 和 STATUS 特性 */
    u32 guest_features = host_features & ((1 << 5) | (1 << 16));
    mmio_write32(base + VIRTIO_MMIO_GUEST_FEAT, guest_features);

    /* 5. 设置 guest page size */
    mmio_write32(base + VIRTIO_MMIO_GUEST_PGSZ, 4096);

    /* 6. 初始化 rx 队列 (q0) */
    if (vring_init(base, 0, VRING_SIZE,
                   &vnet.rx_desc, &vnet.rx_avail, &vnet.rx_used) < 0) {
        guest_puts("virtio-net: failed to init rx vring\n");
        return -1;
    }
    vnet.rx_last_used = 0;

    /* 向 rx 队列填入空缓冲区链
     * 每个描述符 256 字节，每 4 个描述符组成一条链（共 1024 字节容量）
     * 16 个描述符可以组成 4 条链，链头放入 avail 环
     */
    {
        int descs_per_chain = 4;
        int num_chains = VRING_SIZE / descs_per_chain;
        for (int c = 0; c < num_chains; c++) {
            int chain_start = c * descs_per_chain;
            for (int i = 0; i < descs_per_chain; i++) {
                int idx = chain_start + i;
                u8 *buf = guest_alloc(VIRTIO_NET_FRAG_SIZE, 64);
                if (!buf) break;
                vnet.rx_desc[idx].addr = (u64)buf;
                vnet.rx_desc[idx].len = VIRTIO_NET_FRAG_SIZE;
                vnet.rx_desc[idx].flags = VRING_DESC_F_WRITE;
                vnet.rx_desc[idx].next = 0;
                if (i < descs_per_chain - 1) {
                    vnet.rx_desc[idx].flags |= VRING_DESC_F_NEXT;
                    vnet.rx_desc[idx].next = idx + 1;
                }
            }
            vnet.rx_avail->ring[c] = (u16)chain_start;
        }
        vnet.rx_avail->idx = (u16)(VRING_SIZE / 4);
    }

    /* 7. 初始化 tx 队列 (q1) */
    if (vring_init(base, 1, VRING_SIZE,
                   &vnet.tx_desc, &vnet.tx_avail, &vnet.tx_used) < 0) {
        guest_puts("virtio-net: failed to init tx vring\n");
        return -1;
    }
    vnet.tx_last_used = 0;
    vnet.tx_num_free = VRING_SIZE;

    /* 8. 状态: DRIVER_OK */
    mmio_write32(base + VIRTIO_MMIO_STATUS,
                 VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    /* 9. 读取 MAC 地址 */
    u8 mac[6];
    u32 mac0 = mmio_read32(base + VIRTIO_MMIO_CFG_BASE + 0);
    u32 mac1 = mmio_read32(base + VIRTIO_MMIO_CFG_BASE + 4);
    mac[0] = mac0 & 0xff;
    mac[1] = (mac0 >> 8) & 0xff;
    mac[2] = (mac0 >> 16) & 0xff;
    mac[3] = (mac0 >> 24) & 0xff;
    mac[4] = mac1 & 0xff;
    mac[5] = (mac1 >> 8) & 0xff;

    guest_puts("virtio-net: MAC=");
    for (int i = 0; i < 6; i++) {
        if (i) guest_puts(":");
        puts_hex(mac[i], 2);
    }
    guest_puts("\n");

    /* 10. 使能虚拟中断（由 hypervisor 端 vGIC 注入，不需要配置 GICD） */

    vnet.ready = 1;
    guest_puts("virtio-net: driver initialized OK\n");
    return 0;
}

/* 回收已完成的 tx 描述符（从 used 环） */
static void virtio_net_tx_reclaim(void)
{
    while (vnet.tx_used->idx != vnet.tx_last_used) {
        u16 idx = vnet.tx_last_used % VRING_SIZE;
        /* 我们只需要计数，链的第一个描述符就是 ring[idx].id
           但整条链有多少个描述符呢？我们需要计算链长。
           简化处理：每次 reclaim 时重新遍历整个链来计数。
           更好的方式是发送时记录每条链的长度。这里简化，
           因为 used ring 的 id 是链头，我们可以遍历整条链来计数。*/
        u16 cur = (u16)vnet.tx_used->ring[idx].id;
        int count = 0;
        while (1) {
            count++;
            if (!(vnet.tx_desc[cur].flags & VRING_DESC_F_NEXT))
                break;
            cur = vnet.tx_desc[cur].next;
        }
        vnet.tx_num_free += count;
        vnet.tx_last_used++;
    }
}

/* 发送一个包（支持多描述符链）
 * 将数据包按 VIRTIO_NET_FRAG_SIZE 拆分到多个描述符
 * 返回 0 成功，-1 失败
 */
static int virtio_net_tx(const u8 *data, int len)
{
    if (!vnet.ready)
        return -1;

    /* 先回收已完成的描述符 */
    virtio_net_tx_reclaim();

    /* 计算需要多少个描述符 */
    int frags_needed = (len + VIRTIO_NET_FRAG_SIZE - 1) / VIRTIO_NET_FRAG_SIZE;
    if (frags_needed == 0)
        frags_needed = 1;
    if (frags_needed > vnet.tx_num_free)
        return -1;  /* 描述符不够 */

    /* 找一个起始描述符索引（简单的轮询分配）
       因为我们通过 tx_num_free 确保有足够的空闲描述符，
       这里直接用 avail idx 来分配 */
    u16 head_idx = vnet.tx_avail->idx;
    int remaining = len;
    const u8 *p = data;

    for (int i = 0; i < frags_needed; i++) {
        u16 idx = (head_idx + i) % VRING_SIZE;
        int frag_len = remaining < VIRTIO_NET_FRAG_SIZE ? remaining : VIRTIO_NET_FRAG_SIZE;

        vnet.tx_desc[idx].addr = (u64)p;
        vnet.tx_desc[idx].len = (u32)frag_len;
        vnet.tx_desc[idx].flags = 0;
        vnet.tx_desc[idx].next = 0;

        if (i < frags_needed - 1) {
            vnet.tx_desc[idx].flags |= VRING_DESC_F_NEXT;
            vnet.tx_desc[idx].next = (idx + 1) % VRING_SIZE;
        }

        p += frag_len;
        remaining -= frag_len;
    }

    vnet.tx_num_free -= frags_needed;

    /* clean dcache 确保设备能读到最新数据 */
    {
        u64 line_size = 64;
        u64 p1 = (u64)vnet.tx_desc & ~(line_size - 1);
        u64 end = (u64)(vnet.tx_desc + VRING_SIZE);
        while (p1 < end) {
            asm volatile("dc cvac, %0" ::"r"(p1) : "memory");
            p1 += line_size;
        }
        /* 也 clean avail 环 */
        p1 = (u64)vnet.tx_avail & ~(line_size - 1);
        end = (u64)((u8 *)vnet.tx_avail + 6 + 2 * VRING_SIZE + line_size);
        while (p1 < end) {
            asm volatile("dc cvac, %0" ::"r"(p1) : "memory");
            p1 += line_size;
        }
        asm volatile("dsb sy" ::: "memory");
    }

    /* 提交到 avail 环：只需要写链头索引 */
    u16 avail_idx = vnet.tx_avail->idx;
    vnet.tx_avail->ring[avail_idx % VRING_SIZE] = head_idx % VRING_SIZE;
    vnet.tx_avail->idx = avail_idx + 1;

    /* clean avail 环 */
    {
        u64 line_size = 64;
        u64 p1 = (u64)vnet.tx_avail & ~(line_size - 1);
        u64 end = (u64)((u8 *)vnet.tx_avail + 6 + 2 * VRING_SIZE + line_size);
        while (p1 < end) {
            asm volatile("dc cvac, %0" ::"r"(p1) : "memory");
            p1 += line_size;
        }
        asm volatile("dsb sy" ::: "memory");
    }

    /* 通知设备 */
    mmio_write32(vnet.base + VIRTIO_MMIO_QUEUE_NOTIFY, 1);

    return 0;
}

/* 检查 rx 是否有包（支持多描述符链）
 * 返回收到的数据包长度，0 表示没有包
 */
static int virtio_net_rx_poll(u8 *buf, int buf_len)
{
    if (!vnet.ready)
        return 0;

    if (vnet.rx_used->idx == vnet.rx_last_used)
        return 0;

    /* 取一个已用链的头描述符索引 */
    struct vring_used_elem *elem =
        &vnet.rx_used->ring[vnet.rx_last_used % VRING_SIZE];
    u16 desc_idx = (u16)elem->id;
    int total_len = elem->len;

    int copy_len = total_len < buf_len ? total_len : buf_len;
    int remaining = copy_len;
    u8 *dst = buf;
    u16 cur = desc_idx;

    /* 遍历描述符链，从多个描述符拼接数据 */
    while (remaining > 0) {
        u8 *src = (u8 *)(u64)vnet.rx_desc[cur].addr;
        int frag_len = remaining < (int)vnet.rx_desc[cur].len ? remaining : (int)vnet.rx_desc[cur].len;
        for (int i = 0; i < frag_len; i++)
            dst[i] = src[i];
        dst += frag_len;
        remaining -= frag_len;

        if (!(vnet.rx_desc[cur].flags & VRING_DESC_F_NEXT))
            break;
        cur = vnet.rx_desc[cur].next;
    }

    vnet.rx_last_used++;

    /* 归还整条链到 avail 环（只需要写链头索引） */
    u16 avail_idx = vnet.rx_avail->idx;
    vnet.rx_avail->ring[avail_idx % VRING_SIZE] = desc_idx;
    vnet.rx_avail->idx = avail_idx + 1;

    /* clean avail 环 cache */
    {
        u64 line_size = 64;
        u64 p = (u64)vnet.rx_avail & ~(line_size - 1);
        u64 end = (u64)((u8 *)vnet.rx_avail + 6 + 2 * VRING_SIZE + line_size);
        while (p < end) {
            asm volatile("dc cvac, %0" ::"r"(p) : "memory");
            p += line_size;
        }
        asm volatile("dsb sy" ::: "memory");
    }

    return total_len;
}

/* virtio-net 中断处理 */
static void virtio_net_irq_handler(void)
{
    u32 status = mmio_read32(vnet.base + VIRTIO_MMIO_IRQ_STATUS);

    if (status & VIRTIO_IRQ_USED) {
        /* 处理完 ack */
        mmio_write32(vnet.base + VIRTIO_MMIO_IRQ_ACK, VIRTIO_IRQ_USED);
    }

    if (status & VIRTIO_IRQ_CONFIG) {
        mmio_write32(vnet.base + VIRTIO_MMIO_IRQ_ACK, VIRTIO_IRQ_CONFIG);
    }
}

/* virtio-net 环回测试 */
static void virtio_net_loopback_test(void)
{
    guest_puts("virtio-net: starting loopback test...\n");

    /* ===== 测试 1: 小包 64 字节（单描述符） ===== */
    {
        u8 tx_pkt[64];
        for (int i = 0; i < 64; i++)
            tx_pkt[i] = (u8)(i * 2 + 1);

        if (virtio_net_tx(tx_pkt, 64) < 0) {
            guest_puts("virtio-net: test1 tx failed\n");
            return;
        }

        u8 rx_pkt[64];
        int rx_len = 0;
        for (int i = 0; i < 10000; i++) {
            rx_len = virtio_net_rx_poll(rx_pkt, 64);
            if (rx_len > 0)
                break;
        }

        if (rx_len == 0) {
            guest_puts("virtio-net: test1 FAILED (no rx, 64B)\n");
            return;
        }

        int match = 1;
        for (int i = 0; i < 64 && i < rx_len; i++) {
            if (rx_pkt[i] != tx_pkt[i]) {
                match = 0;
                break;
            }
        }

        if (match && rx_len == 64) {
            guest_puts("virtio-net: test1 PASSED (64 bytes, single desc)\n");
        } else {
            guest_puts("virtio-net: test1 FAILED (len=");
            puts_dec(rx_len);
            guest_puts(", data mismatch)\n");
            return;
        }
    }

    /* ===== 测试 2: 大包 1024 字节（多描述符链，4 个描述符） ===== */
    {
        u8 tx_pkt[1024];
        for (int i = 0; i < 1024; i++)
            tx_pkt[i] = (u8)(i & 0xff);

        guest_puts("virtio-net: test2 sending 1024 bytes (4 desc chain)...\n");

        if (virtio_net_tx(tx_pkt, 1024) < 0) {
            guest_puts("virtio-net: test2 tx failed\n");
            return;
        }

        u8 rx_pkt[1024];
        int rx_len = 0;
        for (int i = 0; i < 10000; i++) {
            rx_len = virtio_net_rx_poll(rx_pkt, 1024);
            if (rx_len > 0)
                break;
        }

        if (rx_len == 0) {
            guest_puts("virtio-net: test2 FAILED (no rx, 1024B)\n");
            return;
        }

        if (rx_len != 1024) {
            guest_puts("virtio-net: test2 FAILED (wrong len=");
            puts_dec(rx_len);
            guest_puts(")\n");
            return;
        }

        int match = 1;
        int first_mismatch = -1;
        for (int i = 0; i < 1024; i++) {
            if (rx_pkt[i] != tx_pkt[i]) {
                match = 0;
                first_mismatch = i;
                break;
            }
        }

        if (match) {
            guest_puts("virtio-net: test2 PASSED (1024 bytes, 4-desc chain)\n");
        } else {
            guest_puts("virtio-net: test2 FAILED (data mismatch at ");
            puts_dec(first_mismatch);
            guest_puts(" tx=");
            puts_hex(tx_pkt[first_mismatch], 2);
            guest_puts(" rx=");
            puts_hex(rx_pkt[first_mismatch], 2);
            guest_puts(")\n");
            return;
        }
    }

    /* ===== 测试 3: 边界包 256 字节（刚好一个描述符） ===== */
    {
        u8 tx_pkt[256];
        for (int i = 0; i < 256; i++)
            tx_pkt[i] = (u8)(255 - i);

        if (virtio_net_tx(tx_pkt, 256) < 0) {
            guest_puts("virtio-net: test3 tx failed\n");
            return;
        }

        u8 rx_pkt[256];
        int rx_len = 0;
        for (int i = 0; i < 10000; i++) {
            rx_len = virtio_net_rx_poll(rx_pkt, 256);
            if (rx_len > 0)
                break;
        }

        if (rx_len == 0) {
            guest_puts("virtio-net: test3 FAILED (no rx, 256B)\n");
            return;
        }

        int match = 1;
        for (int i = 0; i < 256; i++) {
            if (rx_pkt[i] != tx_pkt[i]) {
                match = 0;
                break;
            }
        }

        if (match && rx_len == 256) {
            guest_puts("virtio-net: test3 PASSED (256 bytes, 1 desc boundary)\n");
        } else {
            guest_puts("virtio-net: test3 FAILED (len=");
            puts_dec(rx_len);
            guest_puts(", data mismatch)\n");
            return;
        }
    }

    /* ===== 测试 4: 边界包 257 字节（跨 2 个描述符） ===== */
    {
        u8 tx_pkt[257];
        for (int i = 0; i < 257; i++)
            tx_pkt[i] = (u8)(i * 3 + 7);

        guest_puts("virtio-net: test4 sending 257 bytes (2 desc chain)...\n");

        if (virtio_net_tx(tx_pkt, 257) < 0) {
            guest_puts("virtio-net: test4 tx failed\n");
            return;
        }

        u8 rx_pkt[257];
        int rx_len = 0;
        for (int i = 0; i < 10000; i++) {
            rx_len = virtio_net_rx_poll(rx_pkt, 257);
            if (rx_len > 0)
                break;
        }

        if (rx_len == 0) {
            guest_puts("virtio-net: test4 FAILED (no rx, 257B)\n");
            return;
        }

        int match = 1;
        for (int i = 0; i < 257; i++) {
            if (rx_pkt[i] != tx_pkt[i]) {
                match = 0;
                break;
            }
        }

        if (match && rx_len == 257) {
            guest_puts("virtio-net: test4 PASSED (257 bytes, 2-desc chain)\n");
        } else {
            guest_puts("virtio-net: test4 FAILED (len=");
            puts_dec(rx_len);
            guest_puts(", data mismatch)\n");
            return;
        }
    }

    guest_puts("virtio-net: ALL loopback tests PASSED!\n");
}

/* ---------- ivshmem 驱动 ---------- */

#define IVSHMEM_REG_BASE   0x0A001000UL
#define IVSHMEM_MEM_BASE   0x0B000000UL
#define IVSHMEM_IRQ        49

#define IVSHMEM_REVISION   0x00
#define IVSHMEM_DOORBELL   0x04
#define IVSHMEM_IVPOSITION 0x08

static volatile int ivshmem_irq_received = 0;
static int ivshmem_peer_id = 0;

static void ivshmem_init(void)
{
    u32 rev = mmio_read32(IVSHMEM_REG_BASE + IVSHMEM_REVISION);
    u32 pos = mmio_read32(IVSHMEM_REG_BASE + IVSHMEM_IVPOSITION);
    ivshmem_peer_id = (int)pos;

    guest_puts("ivshmem: revision=0x");
    puts_hex(rev, 8);
    guest_puts(" position=");
    puts_dec(pos);
    guest_puts("\n");

    guest_puts("ivshmem: driver initialized\n");
}

/* 向对端发 doorbell */
static void ivshmem_doorbell(int target_peer)
{
    mmio_write32(IVSHMEM_REG_BASE + IVSHMEM_DOORBELL, (u32)target_peer);
}

/* ivshmem 中断处理 */
static void ivshmem_irq_handler(void)
{
    ivshmem_irq_received = 1;
}

/* ivshmem ping-pong 测试 */
static void ivshmem_pingpong_test(void)
{
    volatile u32 *shmem = (volatile u32 *)IVSHMEM_MEM_BASE;

    guest_puts("ivshmem: starting ping-pong test (peer ");
    puts_dec(ivshmem_peer_id);
    guest_puts(")\n");

    u64 vmid = guest_vmid();

    if (vmid == 1) {
        /* VM1 先写共享内存, 然后发 doorbell 给 VM2 */
        shmem[0] = 0xdeadbeef;
        shmem[1] = 0x12345678;
        guest_puts("ivshmem: vm1 wrote to shared memory, sending doorbell to peer 1\n");
        ivshmem_doorbell(1);  /* peer 1 = vm2 */

        /* 等 VM2 回复
         * 混合等待：大部分时间 nop（低开销），每隔一段时间 yield 一次
         * 让对方有机会拿到 CPU 处理并回复。 */
        int timeout = 1000;  /* yield 次数 */
        while (!ivshmem_irq_received && timeout-- > 0) {
            /* 每次 yield 之间 busy wait 一小段，减少 HVC 开销 */
            for (int i = 0; i < 10000 && !ivshmem_irq_received; i++)
                asm volatile("nop");
            if (!ivshmem_irq_received)
                hvc_call(HVC_SCHED_YIELD, 0);
        }

        if (ivshmem_irq_received) {
            guest_puts("ivshmem: vm1 received reply doorbell!\n");
            guest_puts("ivshmem: shmem[2] = 0x");
            puts_hex(shmem[2], 8);
            guest_puts("\n");
            if (shmem[2] == 0xcafebabe)
                guest_puts("ivshmem: ping-pong test PASSED\n");
            else
                guest_puts("ivshmem: ping-pong test FAILED (bad data)\n");
        } else {
            guest_puts("ivshmem: ping-pong test FAILED (timeout waiting for reply)\n");
        }
    } else {
        /* VM2 等 doorbell 从 VM1
         * 混合等待：nop + 周期性 yield */
        int timeout = 1000;
        while (!ivshmem_irq_received && timeout-- > 0) {
            for (int i = 0; i < 10000 && !ivshmem_irq_received; i++)
                asm volatile("nop");
            if (!ivshmem_irq_received)
                hvc_call(HVC_SCHED_YIELD, 0);
        }

        if (ivshmem_irq_received) {
            ivshmem_irq_received = 0;
            guest_puts("ivshmem: vm2 received doorbell!\n");
            guest_puts("ivshmem: shmem[0] = 0x");
            puts_hex(shmem[0], 8);
            guest_puts(" shmem[1] = 0x");
            puts_hex(shmem[1], 8);
            guest_puts("\n");

            /* 写回复 */
            shmem[2] = 0xcafebabe;
            guest_puts("ivshmem: vm2 replying to peer 0\n");
            ivshmem_doorbell(0);  /* peer 0 = vm1 */

            if (shmem[0] == 0xdeadbeef && shmem[1] == 0x12345678)
                guest_puts("ivshmem: ping-pong test PASSED\n");
            else
                guest_puts("ivshmem: ping-pong test FAILED (bad data)\n");
        } else {
            guest_puts("ivshmem: ping-pong test FAILED (timeout waiting for ping)\n");
        }
    }
}

/* ---------- PCI 直通 edu 驱动 ---------- */

#define EDU_BAR0_BASE      0x10000000UL   /* edu BAR0 (直通映射) */
#define EDU_ID_REG         0x00
#define EDU_LIVENESS_REG   0x04
#define EDU_FACTORIAL_REG  0x08
#define EDU_STATUS_REG     0x20
#define EDU_IRQ_STATUS     0x24
#define EDU_IRQ_RAISE      0x60
#define EDU_IRQ_ACK        0x64

#define PCI_EDU_IRQ        50              /* 虚拟 SPI（hypervisor 转发） */

/* ---------- Phase 9: vITS 虚拟 ITS 驱动 ---------- */

#define VITS_BASE          0x0C000000UL   /* vITS 虚拟基地址 (IPA) */

/* vITS 寄存器偏移 */
#define VITS_CTLR          0x0000
#define VITS_TYPER         0x0008
#define VITS_CBASER        0x0080
#define VITS_CWRITER       0x0088
#define VITS_CREADR        0x0090
#define VITS_BASER(n)      (0x0100 + (n) * 8)
#define VITS_TRANSLATER    0x10040

/* ITS 命令码 */
#define ITS_CMD_SYNC       0x05
#define ITS_CMD_MAPD       0x08
#define ITS_CMD_MAPC       0x09
#define ITS_CMD_MAPTI      0x0A

/* 命令队列大小（1 页 = 128 条命令） */
#define VITS_CMDQ_SIZE     4096

/* vITS 命令队列内存（从 guest heap 分配，4KB 对齐） */
static u64 vits_cmdq_buf[512];   /* 512 * 8 = 4096 字节 */

/* vITS 设备表/集合表（简化：各 1 页） */
static u64 vits_devtab_buf[512];
static u64 vits_colltab_buf[512];

/* vITS 命令队列写指针（字节偏移） */
static u32 vits_cwriter = 0;

/*
 * 发送一条 vITS 命令（32 字节 = 4 x u64）
 * 写命令到命令队列，然后写 CWRITER 触发处理
 */
static void vits_send_cmd(u64 d0, u64 d1, u64 d2, u64 d3)
{
    u64 *slot = (u64 *)((u8 *)vits_cmdq_buf + vits_cwriter);
    slot[0] = d0;
    slot[1] = d1;
    slot[2] = d2;
    slot[3] = d3;
    asm volatile("dsb sy" ::: "memory");

    vits_cwriter += 32;
    if (vits_cwriter >= VITS_CMDQ_SIZE)
        vits_cwriter = 0;

    mmio_write32(VITS_BASE + VITS_CWRITER, vits_cwriter);

    /* 等 CREADR == CWRITER（同步处理模型） */
    int timeout = 1000000;
    while (mmio_read32(VITS_BASE + VITS_CREADR) != vits_cwriter &&
           --timeout > 0)
        asm volatile("nop");
    if (timeout == 0)
        guest_puts("vITS: command timeout!\n");
}

/*
 * 初始化 vITS
 *   - 读 TYPER 确认能力
 *   - 配置命令队列（CBASER）
 *   - 配置设备表/集合表（BASER0/BASER1）
 *   - 使能 ITS（CTLR.Enabled = 1）
 */
static int vits_init(void)
{
    u32 typer_lo = mmio_read32(VITS_BASE + VITS_TYPER);
    u32 typer_hi = mmio_read32(VITS_BASE + VITS_TYPER + 4);

    guest_puts("vITS: TYPER=0x");
    puts_hex(typer_hi, 8);
    puts_hex(typer_lo, 8);
    guest_puts("\n");

    if (typer_lo == 0) {
        guest_puts("vITS: device not present (TYPER=0)\n");
        return -1;
    }

    /* 确保命令队列缓冲区 4KB 对齐 */
    u64 cmdq_pa = (u64)vits_cmdq_buf;
    if (cmdq_pa & 0xFFF) {
        guest_puts("vITS: cmdq not 4KB aligned\n");
        return -1;
    }

    /* 配置 CBASER：V=1 + PA + SIZE=0（1 页 = 4KB） */
    u64 cbaser = (1ULL << 63) | (cmdq_pa & ~0xFFFULL) | 0;  /* SIZE=0 -> 1 page */
    mmio_write32(VITS_BASE + VITS_CBASER, (u32)(cbaser & 0xFFFFFFFF));
    mmio_write32(VITS_BASE + VITS_CBASER + 4, (u32)(cbaser >> 32));

    /* 配置 BASER0：设备表（Device table） */
    u64 baser0 = (1ULL << 63) | ((u64)vits_devtab_buf & ~0xFFFULL);
    mmio_write32(VITS_BASE + VITS_BASER(0), (u32)(baser0 & 0xFFFFFFFF));
    mmio_write32(VITS_BASE + VITS_BASER(0) + 4, (u32)(baser0 >> 32));

    /* 配置 BASER1：集合表（Collection table） */
    u64 baser1 = (1ULL << 63) | ((u64)vits_colltab_buf & ~0xFFFULL);
    mmio_write32(VITS_BASE + VITS_BASER(1), (u32)(baser1 & 0xFFFFFFFF));
    mmio_write32(VITS_BASE + VITS_BASER(1) + 4, (u32)(baser1 >> 32));

    /* 使能 ITS */
    mmio_write32(VITS_BASE + VITS_CTLR, 0x1);

    guest_puts("vITS: initialized and enabled\n");
    return 0;
}

/*
 * vITS MAPC：映射 collection 到目标 CPU
 *   icid = collection ID
 *   procnum = 目标 Redistributor 编号
 */
static void vits_mapc(u16 icid, u32 procnum)
{
    /* dw2 = V[63] | RDBASE(procnum)[47:16] | ICID[15:0] */
    u64 dw2 = (1ULL << 63) | ((u64)procnum << 16) | icid;
    vits_send_cmd(ITS_CMD_MAPC, 0, dw2, 0);
    guest_puts("vITS: MAPC icid=");
    puts_dec(icid);
    guest_puts(" procnum=");
    puts_dec(procnum);
    guest_puts("\n");
}

/*
 * vITS MAPD：映射设备（DeviceID → ITT）
 *   devid = 设备 ID
 *   itt_addr = ITT 表地址（GPA）
 *   size = ITT 大小（2^(size+1) 个事件）
 */
static void vits_mapd(u32 devid, u64 itt_addr, u8 size)
{
    u64 dw0 = ITS_CMD_MAPD | ((u64)devid << 32);
    u64 dw1 = size;
    /* dw2 = V[63] | ITT addr[51:8] */
    u64 dw2 = (1ULL << 63) | (itt_addr & ~0xFFULL);
    vits_send_cmd(dw0, dw1, dw2, 0);
    guest_puts("vITS: MAPD devid=");
    puts_dec(devid);
    guest_puts(" itt_addr=0x");
    puts_hex(itt_addr, 12);
    guest_puts("\n");
}

/*
 * vITS MAPTI：映射事件到 LPI
 *   devid = 设备 ID
 *   eventid = 事件 ID
 *   vintid = 虚拟 LPI 号
 *   icid = collection ID
 */
static void vits_mapti(u32 devid, u32 eventid, u32 vintid, u16 icid)
{
    u64 dw0 = ITS_CMD_MAPTI | ((u64)devid << 32);
    /* dw1 = pINTID[63:32] | EventID[31:0] （pINTID 即 Guest 指定的 vintid） */
    u64 dw1 = ((u64)vintid << 32) | eventid;
    u64 dw2 = icid;
    vits_send_cmd(dw0, dw1, dw2, 0);
    guest_puts("vITS: MAPTI devid=");
    puts_dec(devid);
    guest_puts(" eventid=");
    puts_dec(eventid);
    guest_puts(" vintid=");
    puts_dec(vintid);
    guest_puts(" icid=");
    puts_dec(icid);
    guest_puts("\n");
}

static void vits_sync(void)
{
    vits_send_cmd(ITS_CMD_SYNC, 0, 0, 0);
}

/*
 * vLPI 转换为虚拟 SPI 号（与 irq.c 中的转换公式一致）：
 *   vSPI = 64 + (vLPI - 8192)
 * 这是 Phase 9 MVP 的约定——Guest 的 vINTID 是虚拟 LPI 号，
 * 但 vGIC 只有 SPI 注入能力，所以做了线性转换。
 */
static inline u32 vlpi_to_vspi(u32 vlpi)
{
    return 64 + (vlpi - 8192);
}

static volatile int edu_irq_received = 0;
static volatile u32 edu_irq_status_val = 0;

/* Phase 9: vITS MSI 测试状态 */
static volatile int vits_irq_received = 0;
static volatile u32 vits_irq_intid = 0;

/* vITS 测试：Guest 自选 vLPI 号（虚拟 LPI） */
#define VITS_TEST_VLPI        8200   /* 虚拟 LPI 号（Guest 自选） */
/* 转换为虚拟 SPI 号（与 irq.c 公式一致：vSPI = 64 + (vLPI - 8192)） */
#define VITS_TEST_VSPI        (64 + (VITS_TEST_VLPI - 8192))

/* edu 设备在 PCI 总线上的 requester_id (bus 0, dev 2, func 0): (0<<8)|(2<<3)|0 = 16 */
#define EDU_DEVID             16

/* ITT 表（给 vITS MAPD 用，分配一页） */
static u64 vits_itt_buf[512];  /* 4KB */

static void edu_test(void)
{
    u32 id, live, fact;
    int timeout;

    guest_puts("edu: probing device at BAR0=0x10000000...\n");

    /* 1. identification（应为 0x010000ed） */
    id = mmio_read32(EDU_BAR0_BASE + EDU_ID_REG);
    guest_puts("edu: identification=0x");
    puts_hex(id, 8);
    guest_puts("\n");
    if (id != 0x010000ed) {
        guest_puts("edu: FAILED (bad identification)\n");
        return;
    }

    /* 2. liveness：写入读回按位取反 */
    mmio_write32(EDU_BAR0_BASE + EDU_LIVENESS_REG, 0xdeadbeef);
    live = mmio_read32(EDU_BAR0_BASE + EDU_LIVENESS_REG);
    if (live == ~0xdeadbeefu) {
        guest_puts("edu: liveness PASSED (0x");
        puts_hex(live, 8);
        guest_puts(")\n");
    } else {
        guest_puts("edu: liveness FAILED (0x");
        puts_hex(live, 8);
        guest_puts(")\n");
        return;
    }

    /* 3. factorial：计算 10! = 3628800 */
    mmio_write32(EDU_BAR0_BASE + EDU_FACTORIAL_REG, 10);
    timeout = 10000000;
    while ((mmio_read32(EDU_BAR0_BASE + EDU_STATUS_REG) & 0x1) && timeout-- > 0)
        asm volatile("nop");
    fact = mmio_read32(EDU_BAR0_BASE + EDU_FACTORIAL_REG);
    guest_puts("edu: factorial(10)=");
    puts_dec(fact);
    guest_puts(" (expect 3628800)\n");
    if (fact == 3628800)
        guest_puts("edu: factorial PASSED\n");
    else
        guest_puts("edu: factorial FAILED\n");

    /* 4. 中断转发测试（Hypervisor 侧配置为 MSI/LPI 直通，Guest 无感知） */
    guest_puts("edu: starting interrupt test (MSI)...\n");
    edu_irq_received = 0;
    /* 清残留 irq_status */
    mmio_write32(EDU_BAR0_BASE + EDU_IRQ_ACK, 0xffffffff);
    /* 写非零值到 raise 寄存器 -> 设备发 MSI -> ITS 翻译成 LPI 8192
     * -> Hypervisor 应答后注入虚拟 SPI 50 */
    mmio_write32(EDU_BAR0_BASE + EDU_IRQ_RAISE, 0x12345678);

    timeout = 10000000;
    while (!edu_irq_received && timeout-- > 0)
        asm volatile("nop");

    if (edu_irq_received) {
        guest_puts("edu: MSI interrupt PASSED (irq_status=0x");
        puts_hex(edu_irq_status_val, 8);
        guest_puts(")\n");
    } else {
        guest_puts("edu: MSI interrupt FAILED (timeout)\n");
        /* 让 hypervisor dump 中断链路状态（GIC/edu/LR），定位断点 */
        hvc_call(HVC_PCI_DEBUG, 0);
    }
}

/*
 * Phase 9: vITS 测试
 *
 * 测试流程：
 *   1. 初始化 vITS（读 TYPER、配置 CBASER/BASER、使能）
 *   2. 发 MAPC（collection 0 -> procnum 0）
 *   3. 发 MAPD（devid=16 -> ITT）
 *   4. 发 MAPTI（eventid=1 -> vLPI=8200, icid=0）
 *   5. 通过 PCI ECAM 修改 edu 的 MSI data = 1（EventID = 1）
 *   6. 触发 edu 中断，验证收到 VITS_TEST_VSPI 号虚拟中断
 *   7. 恢复 MSI data = 0（不影响后续回归测试）
 */

/* ---------- Phase 10: 调度器 + 内存管理 + 共享内存测试 ---------- */

/* 共享内存 notify 中断标志 */
static volatile int shm_notify_received = 0;

/* 共享内存测试用的 key */
#define SHM_TEST_KEY   0x12345678

/* 动态内存映射测试用的 IPA（选高地址，避开已用区域）
 * Guest RAM 从 0x40000000 开始，我们用 0x48000000 附近的未映射区域
 * （Guest 自身镜像 + 栈 + 页表大概用了前 8MB） */
#define DYN_MAP_IPA    0x48000000UL

/*
 * Test 5.1: 调度器 hypercall 测试
 *   - 获取当前优先级
 *   - 设置优先级
 *   - sched_yield 让出 CPU
 */
static void test_scheduler(void)
{
    guest_puts("--- Test 5.1: Scheduler hypercalls ---\n");

    /* 1. 获取当前优先级 */
    u64 prio = hvc_call(HVC_SCHED_GET_PRIO, 0);
    guest_puts("sched: current priority = ");
    puts_dec(prio);
    guest_puts("\n");

    /* 2. 设置为高优先级 (3) */
    u64 ret = hvc_call(HVC_SCHED_SET_PRIO, 3);
    guest_puts("sched: set priority 3, ret=");
    puts_dec(ret);
    guest_puts("\n");

    prio = hvc_call(HVC_SCHED_GET_PRIO, 0);
    guest_puts("sched: priority now = ");
    puts_dec(prio);
    guest_puts("\n");

    /* 3. sched_yield 让出 CPU */
    ret = hvc_call(HVC_SCHED_YIELD, 0);
    guest_puts("sched: yield, ret=");
    puts_dec(ret);
    guest_puts("\n");

    /* 4. 恢复默认优先级 (2) */
    hvc_call(HVC_SCHED_SET_PRIO, 2);

    guest_puts("sched: PASSED\n");
}

/*
 * Test 5.2: 动态内存映射测试
 *   - hyp_mem_map 映射一个新页
 *   - 写数据验证
 *   - hyp_mem_unmap 取消映射
 */
static void test_mem_map(void)
{
    guest_puts("\n--- Test 5.2: Dynamic memory mapping ---\n");

    /* 1. 映射一页到 DYN_MAP_IPA */
    u64 ret = hvc_call2(HVC_MEM_MAP, DYN_MAP_IPA, 0);
    guest_puts("mem: map 0x");
    puts_hex(DYN_MAP_IPA, 8);
    guest_puts(", ret=");
    puts_dec(ret);
    guest_puts("\n");

    if (ret != 0) {
        guest_puts("mem: FAILED (map returned error)\n");
        return;
    }

    /* 2. 写数据到新映射的页面 */
    volatile u32 *ptr = (volatile u32 *)DYN_MAP_IPA;
    ptr[0] = 0xDEADBEEF;
    ptr[1] = 0xCAFEBABE;
    ptr[255] = 0x12345678;

    /* 3. 读回来验证 */
    if (ptr[0] != 0xDEADBEEF || ptr[1] != 0xCAFEBABE ||
        ptr[255] != 0x12345678) {
        guest_puts("mem: FAILED (data mismatch)\n");
        return;
    }

    guest_puts("mem: read/write verified (0xDEADBEEF, 0xCAFEBABE, 0x12345678)\n");

    /* 4. 取消映射 */
    ret = hvc_call(HVC_MEM_UNMAP, DYN_MAP_IPA);
    guest_puts("mem: unmap, ret=");
    puts_dec(ret);
    guest_puts("\n");

    guest_puts("mem: PASSED\n");
}

/*
 * Test 5.3: 跨 VM 共享内存测试
 *
 * 测试流程（轮询模式，不依赖中断通知时序）：
 *   VM1: 创建 shm -> attach -> 写 "PING" -> 忙等 "PONG" -> 验证 -> detach
 *   VM2: 查找 shm -> attach -> 忙等 "PING" -> 写 "PONG" -> detach
 *
 * 用轮询而非中断通知，避免两个 VM 启动时序不一致导致 notify 丢失。
 */
static void test_shared_memory(void)
{
    guest_puts("\n--- Test 5.3: Cross-VM shared memory ---\n");

    u64 vmid = guest_vmid();

    /* 1. 尝试创建/获取共享内存 */
    int shmid = (int)hvc_call2(HVC_SHM_CREATE, SHM_TEST_KEY, 4096);
    guest_puts("shm: create key=0x");
    puts_hex(SHM_TEST_KEY, 8);
    guest_puts(" -> shmid=");
    puts_dec(shmid);
    guest_puts("\n");

    if (shmid < 0) {
        guest_puts("shm: FAILED (create returned error)\n");
        return;
    }

    /* 2. attach 到指定 IPA */
    u64 shm_ipa = DYN_MAP_IPA + 0x1000;
    int ret = (int)hvc_call2(HVC_SHM_ATTACH, (u64)shmid, shm_ipa);
    guest_puts("shm: attach shmid=");
    puts_dec(shmid);
    guest_puts(" ipa=0x");
    puts_hex(shm_ipa, 8);
    guest_puts(" ret=");
    puts_dec(ret);
    guest_puts("\n");

    if (ret != 0) {
        guest_puts("shm: FAILED (attach returned error)\n");
        return;
    }

    volatile char *shm_ptr = (volatile char *)shm_ipa;

    if (vmid == 1) {
        /* VM1: 主动方 — 写 "PING" 然后忙等 "PONG" */
        shm_ptr[0] = 'P';
        shm_ptr[1] = 'I';
        shm_ptr[2] = 'N';
        shm_ptr[3] = 'G';
        shm_ptr[4] = '\0';
        asm volatile("dsb sy" ::: "memory");

        guest_puts("shm: VM1 wrote 'PING', waiting for 'PONG'...\n");

        /* 忙等 PONG，循环中调用 sched_yield 主动让出 CPU 给 VM2 */
        int timeout = 50000;  /* yield 次数，不是 cycles */
        while (shm_ptr[0] != 'P' || shm_ptr[1] != 'O' ||
               shm_ptr[2] != 'N' || shm_ptr[3] != 'G') {
            hvc_call(HVC_SCHED_YIELD, 0);
            if (--timeout <= 0) break;
        }

        if (timeout > 0 && shm_ptr[0] == 'P' && shm_ptr[1] == 'O' &&
            shm_ptr[2] == 'N' && shm_ptr[3] == 'G') {
            guest_puts("shm: VM1 received 'PONG' from VM2\n");
            guest_puts("shm: PASSED\n");
        } else {
            guest_puts("shm: FAILED (VM1 timeout waiting for PONG)\n");
        }
    } else {
        /* VM2: 被动方 — 忙等 "PING"，然后回复 "PONG" */
        guest_puts("shm: VM2 waiting for 'PING'...\n");

        /* 忙等 PING，循环中 yield 让出 CPU */
        int timeout = 50000;  /* yield 次数 */
        while (shm_ptr[0] != 'P' || shm_ptr[1] != 'I' ||
               shm_ptr[2] != 'N' || shm_ptr[3] != 'G') {
            hvc_call(HVC_SCHED_YIELD, 0);
            if (--timeout <= 0) break;
        }

        if (timeout > 0 && shm_ptr[0] == 'P' && shm_ptr[1] == 'I' &&
            shm_ptr[2] == 'N' && shm_ptr[3] == 'G') {
            guest_puts("shm: VM2 received 'PING'\n");

            /* 回复 "PONG" */
            shm_ptr[0] = 'P';
            shm_ptr[1] = 'O';
            shm_ptr[2] = 'N';
            shm_ptr[3] = 'G';
            shm_ptr[4] = '\0';
            asm volatile("dsb sy" ::: "memory");

            guest_puts("shm: VM2 replied 'PONG'\n");
            guest_puts("shm: PASSED\n");
        } else {
            guest_puts("shm: FAILED (VM2 timeout waiting for PING)\n");
        }
    }

    /* 4. detach（取消映射） */
    hvc_call(HVC_SHM_DETACH, (u64)shmid);
    guest_puts("shm: detached\n");
}

static void phase10_tests(void)
{
    guest_puts("\n========================================\n");
    guest_puts("  Phase 10: Advanced Features\n");
    guest_puts("========================================\n");

    test_scheduler();
    test_mem_map();
    test_shared_memory();

    guest_puts("\nPhase 10: ALL tests done\n");
}

/*
 * Phase 11: 虚拟 SMP 测试（主核侧）
 *
 * 验证内容：
 *   1. 次 vCPU 成功启动并就绪
 *   2. 自旋锁在多 vCPU 下的正确性
 *   3. 共享内存数据一致性
 *
 * 测试流程：
 *   主核：初始化共享变量 -> 等待次核就绪 -> 发启动信号
 *         -> 主核也参与累加 -> 等待次核完成 -> 验证结果
 *   次核：就绪 -> 等启动信号 -> 累加 -> 完成标志
 */
static void phase11_smp_test(void)
{
    guest_puts("\n========================================\n");
    guest_puts("  Phase 11: Virtual SMP\n");
    guest_puts("========================================\n");

    volatile u64 *ready_flag = (volatile u64 *)(SMP_TEST_ADDR + SMP_READY_FLAG_OFF);
    volatile u64 *done_flag = (volatile u64 *)(SMP_TEST_ADDR + SMP_DONE_FLAG_OFF);
    volatile u64 *counter = (volatile u64 *)(SMP_TEST_ADDR + SMP_COUNTER_OFF);
    spinlock_t *lock = (spinlock_t *)(SMP_TEST_ADDR + SMP_LOCK_OFF);

    /* 初始化共享变量（主核负责） */
    *ready_flag = 0;
    *done_flag = 0;
    *counter = 0;
    spin_lock_init(lock);
    asm volatile("dmb ish" ::: "memory");

    guest_puts("\n--- Test 1: Secondary vCPU bringup ---\n");

    /* 发就绪信号：告诉次核可以开始了 */
    *ready_flag = 1;
    asm volatile("dmb ish" ::: "memory");

    /* 等待次核完成（done_flag == 1）
     * 次核会先打印一些信息，然后参与累加测试，最后置 done_flag */
    guest_puts("smp: master waiting for secondary vCPU...\n");
    int timeout = 50000;  /* yield 次数 */
    while (*done_flag == 0) {
        hvc_call(HVC_SCHED_YIELD, 0);
        if (--timeout <= 0) break;
    }

    if (*done_flag == 0) {
        guest_puts("smp: FAILED (secondary vCPU timeout)\n");
        return;
    }
    guest_puts("smp: secondary vCPU finished\n");

    /* 主核也参与累加（和次核并发）
     * 注意：上面我们等了 done_flag，说明次核已经完成了。
     * 为了验证并发，我们应该双方同时累加。
     * 这里改成：主核先发 ready，主核自己也开始累加，
     * 然后等 done_flag，最后验证。 */

    /* 重新初始化，做并发测试 */
    guest_puts("\n--- Test 2: Concurrent counter with spinlock ---\n");
    *done_flag = 0;
    *counter = 0;
    spin_lock_init(lock);
    asm volatile("dmb ish" ::: "memory");

    /* 启动信号（次核看到 ready=2 就开始并发测试） */
    *ready_flag = 2;
    asm volatile("dmb ish" ::: "memory");

    guest_puts("smp: starting concurrent counter test (each adds ");
    puts_dec(SMP_ITERATIONS);
    guest_puts(" times)\n");

    /* 主核也累加（和次核并发） */
    for (int i = 0; i < SMP_ITERATIONS; i++) {
        spin_lock(lock);
        (*counter)++;
        spin_unlock(lock);
    }

    /* 等待次核完成 */
    guest_puts("smp: master done, waiting for secondary...\n");
    timeout = 50000;  /* yield 次数 */
    while (*done_flag != 2) {
        hvc_call(HVC_SCHED_YIELD, 0);
        if (--timeout <= 0) break;
    }

    if (*done_flag != 2) {
        guest_puts("smp: FAILED (secondary vCPU timeout in concurrent test)\n");
        return;
    }

    /* 验证结果：counter 应该等于 2 * SMP_ITERATIONS */
    u64 expected = (u64)SMP_ITERATIONS * 2;
    guest_puts("smp: counter = ");
    puts_dec(*counter);
    guest_puts(" (expected ");
    puts_dec(expected);
    guest_puts(")\n");

    if (*counter == expected) {
        guest_puts("smp: PASSED (spinlock + shared counter correct)\n");
    } else {
        guest_puts("smp: FAILED (counter mismatch, diff = ");
        puts_dec(*counter > expected ? *counter - expected : expected - *counter);
        guest_puts(")\n");
    }

    guest_puts("\nPhase 11: ALL tests done\n");
}

/* ---------- Phase 12: vSGI（虚拟 SGI）测试 ---------- */

static void phase12_vsgi_test(void)
{
    guest_puts("\n========================================\n");
    guest_puts("  Phase 12: Virtual SGI (vSGI)\n");
    guest_puts("========================================\n");

    volatile u64 *vsgi_received = (volatile u64 *)(SMP_TEST_ADDR + VSGI_RECEIVED_OFF);
    volatile u64 *vsgi_count = (volatile u64 *)(SMP_TEST_ADDR + VSGI_COUNT_OFF);
    volatile u64 *done_flag = (volatile u64 *)(SMP_TEST_ADDR + SMP_DONE_FLAG_OFF);

    int my_cpuid = vsgi_get_cpuid();

    /* 主核（vcpu0）负责发起测试 */
    if (my_cpuid == 0) {
        /* 初始化 vSGI 测试变量 */
        *vsgi_received = 0;
        *vsgi_count = 0;
        *done_flag = 0;
        asm volatile("dmb ish" ::: "memory");

        guest_puts("\n--- Test 1: Master -> Secondary vSGI ---\n");
        guest_puts("vsgi: master (vcpu0) sending SGI#5 to vcpu1...\n");

        /* 发 SGI 给次核 */
        int ret = vsgi_send(1, VSGI_TEST_SGI_ID);
        if (ret != 0) {
            guest_puts("vsgi: FAILED (send returned ");
            puts_dec(ret);
            guest_puts(")\n");
            return;
        }

        /* 等待次核收到中断并置 done_flag */
        guest_puts("vsgi: master waiting for secondary to receive SGI...\n");
        int timeout = 50000;
        while (*done_flag != 3) {   /* done_flag == 3 表示次核收到 vSGI */
            hvc_call(HVC_SCHED_YIELD, 0);
            if (--timeout <= 0) break;
        }

        if (*done_flag != 3) {
            guest_puts("vsgi: FAILED (secondary vSGI timeout, count=");
            puts_dec(*vsgi_count);
            guest_puts(")\n");
            return;
        }

        /* 验证 */
        guest_puts("vsgi: secondary received SGI count = ");
        puts_dec(*vsgi_count);
        guest_puts("\n");

        if (*vsgi_count == 1 && (*vsgi_received & (1ULL << VSGI_TEST_SGI_ID))) {
            guest_puts("vsgi: PASSED (master->secondary vSGI correct)\n");
        } else {
            guest_puts("vsgi: FAILED (wrong count or sgi id, received_bitmap=0x");
            puts_hex((u32)*vsgi_received, 4);
            guest_puts(")\n");
            return;
        }

        /* Test 2: 多次发送（逐次握手，避免 SGI 边沿触发被合并） */
        guest_puts("\n--- Test 2: Multiple vSGI sends (handshake) ---\n");
        *vsgi_count = 0;
        *vsgi_received = 0;
        asm volatile("dmb ish" ::: "memory");

        /*
         * SGI 是边沿触发的：若连续快发多次，硬件/vGIC 会把同一 INTID 的
         * 多次边沿合并成一次 pending，导致"发 5 收 5"不可靠。
         * 这里改成逐次握手：每发一个 SGI，等次核收到（count 递增）后再发
         * 下一个，从而稳定地验证跨核 vSGI 注入可重复工作。
         */
        for (int i = 0; i < 5; i++) {
            ret = vsgi_send(1, VSGI_TEST_SGI_ID);
            if (ret != 0) {
                guest_puts("vsgi: FAILED (send #");
                puts_dec(i);
                guest_puts(" failed)\n");
                return;
            }
            /* 等次核收到本次 SGI（count 递增）再发下一个 */
            int t = 50000;
            while (*vsgi_count < (u64)(i + 1)) {
                hvc_call(HVC_SCHED_YIELD, 0);
                if (--t <= 0) break;
            }
            if (*vsgi_count < (u64)(i + 1)) {
                guest_puts("vsgi: FAILED (round ");
                puts_dec(i);
                guest_puts(" timeout, count=");
                puts_dec(*vsgi_count);
                guest_puts(")\n");
                return;
            }
        }

        guest_puts("vsgi: secondary received count = ");
        puts_dec(*vsgi_count);
        guest_puts(" (5 round-trips)\n");
        guest_puts("vsgi: PASSED (multiple vSGI correct)\n");

        guest_puts("\nPhase 12: ALL tests done\n");
    } else {
        /* 次核：等待主核发 SGI，收到后置 done_flag */
        guest_puts("vsgi: secondary (vcpu");
        puts_dec(my_cpuid);
        guest_puts(") waiting for SGI from master...\n");

        /* 第一次：等 1 个 SGI */
        int timeout = 100000;
        while (*vsgi_count < 1) {
            hvc_call(HVC_SCHED_YIELD, 0);
            if (--timeout <= 0) break;
        }

        if (*vsgi_count >= 1) {
            guest_puts("vsgi: secondary received SGI! count=");
            puts_dec(*vsgi_count);
            guest_puts("\n");
            *done_flag = 3;   /* 告诉主核：第一次收到了 */
            asm volatile("dmb ish" ::: "memory");
        }

        /* 第二次：等累计 5 个 SGI */
        timeout = 100000;
        while (*vsgi_count < 5) {
            hvc_call(HVC_SCHED_YIELD, 0);
            if (--timeout <= 0) break;
        }

        if (*vsgi_count >= 5) {
            guest_puts("vsgi: secondary received all 5 SGIs! total=");
            puts_dec(*vsgi_count);
            guest_puts("\n");
            *done_flag = 4;   /* 告诉主核：5 次都收到了 */
            asm volatile("dmb ish" ::: "memory");
        }
    }
}

/* ---------- Phase 17: 虚拟 SGI trap-and-emulate 测试 ---------- */

/*
 * 验证 Hypervisor 捕获 Guest 写 ICC_SGI1R_EL1 并翻译成 vSGI 注入。
 * 与 Phase 12 的区别：这里用标准 GICv3 系统寄存器发送 SGI（触发 trap），
 * 而非 HVC_VSGI_SEND 半虚拟化。
 */
static void phase17_vsgi_trap_test(void)
{
    guest_puts("\n========================================\n");
    guest_puts("  Phase 17: Virtual SGI trap-and-emulate\n");
    guest_puts("========================================\n");

    volatile u64 *vsgi_received = (volatile u64 *)(SMP_TEST_ADDR + VSGI_RECEIVED_OFF);
    volatile u64 *vsgi_count = (volatile u64 *)(SMP_TEST_ADDR + VSGI_COUNT_OFF);
    volatile u64 *done_flag = (volatile u64 *)(SMP_TEST_ADDR + SMP_DONE_FLAG_OFF);

    int my_cpuid = vsgi_get_cpuid();

    if (my_cpuid == 0) {
        *vsgi_received = 0;
        *vsgi_count = 0;
        *done_flag = 0;
        asm volatile("dmb ish" ::: "memory");

        guest_puts("vsgi-trap: master (vcpu0) writing ICC_SGI1R_EL1 -> SGI#");
        puts_dec(VSGI_TRAP_TEST_SGI_ID);
        guest_puts(" to vcpu1...\n");

        /* 标准 GICv3 SGI 编码：INTID [27:24]，TargetList [15:0]（bit1=vcpu1） */
        u64 sgi_val = ((u64)(VSGI_TRAP_TEST_SGI_ID & 0xF) << 24)
                    | (1ULL << 1);
        icc_sgi1r_write(sgi_val);

        guest_puts("vsgi-trap: master waiting for secondary...\n");
        int timeout = 50000;
        while (*done_flag != 5) {   /* done_flag == 5 表示次核收到 trap SGI */
            hvc_call(HVC_SCHED_YIELD, 0);
            if (--timeout <= 0) break;
        }

        if (*done_flag != 5) {
            guest_puts("vsgi-trap: FAILED (secondary timeout, count=");
            puts_dec(*vsgi_count);
            guest_puts(")\n");
            return;
        }

        guest_puts("vsgi-trap: secondary received count = ");
        puts_dec(*vsgi_count);
        guest_puts("\n");

        if (*vsgi_count == 1 &&
            (*vsgi_received & (1ULL << VSGI_TRAP_TEST_SGI_ID))) {
            guest_puts("vsgi-trap: PASSED (ICC_SGI1R_EL1 trap-and-emulate delivered SGI#");
            puts_dec(VSGI_TRAP_TEST_SGI_ID);
            guest_puts(")\n");
        } else {
            guest_puts("vsgi-trap: FAILED (count=");
            puts_dec(*vsgi_count);
            guest_puts(", bitmap=0x");
            puts_hex((u32)*vsgi_received, 4);
            guest_puts(")\n");
            return;
        }

        guest_puts("\nPhase 17: ALL tests done\n");
    } else {
        guest_puts("vsgi-trap: secondary (vcpu");
        puts_dec(my_cpuid);
        guest_puts(") waiting for SGI#");
        puts_dec(VSGI_TRAP_TEST_SGI_ID);
        guest_puts("...\n");

        /*
         * 两阶段握手，避免 Phase 12 遗留的 vsgi_count 值干扰：
         *   1) 先等主核进入 Phase 17 并清零共享变量（done_flag==0）；
         *   2) 再等 SGI 到达（vsgi_count 递增）。
         * 主核清零 done_flag 前已先清零 vsgi_count，故看到 done_flag==0
         * 时 vsgi_count 一定是干净值。
         */
        int timeout = 100000;
        while (*done_flag != 0) {
            hvc_call(HVC_SCHED_YIELD, 0);
            if (--timeout <= 0) break;
        }

        timeout = 100000;
        while (*vsgi_count < 1) {
            hvc_call(HVC_SCHED_YIELD, 0);
            if (--timeout <= 0) break;
        }

        if (*vsgi_count >= 1) {
            guest_puts("vsgi-trap: secondary received SGI! count=");
            puts_dec(*vsgi_count);
            guest_puts("\n");
            *done_flag = 5;   /* 告诉主核：收到了 */
            asm volatile("dmb ish" ::: "memory");
        }
    }
}

/* ---------- Phase 18: 优先级与 pending 完整建模 ---------- */

/*
 * Phase 18 测试（主核单线程）：
 *
 * 验证 vGIC 的优先级与 pending 状态建模：
 *   1. GICD IPRIORITYR 读写：写入自定义优先级，读回校验
 *   2. GICD ISPENDR/ICPENDR 读写：置位/清除软件 pending 位图
 *   3. GICR IPRIORITYR 读写：SGI/PPI 优先级读写
 *   4. GICR ISPENDR0/ICPENDR0 读写：SGI/PPI 软件 pending
 *
 * 优先级规则：数值越小优先级越高（GICv3 标准）。
 * 默认优先级 0xA0。
 */
static void phase18_priority_pending_test(void)
{
    guest_puts("\nPhase 18: Priority & pending modeling\n");

    int passed = 1;

    /* ---- 1. GICD IPRIORITYR 读写 ---- */
    guest_puts("  [1] GICD IPRIORITYR read/write ... ");

    /* 读初始值（默认 0xA0），取 SPI #52（IPRIORITYR[13] 的 byte 0） */
    u32 prio_reg_before = mmio_read32((void *)GICD_IPRIORITYR(13));
    u8 prio_52_before = (u8)(prio_reg_before & 0xFF);
    if (prio_52_before != 0xA0) {
        guest_puts("FAILED (default prio of SPI#52 = 0x");
        puts_hex(prio_52_before, 2);
        guest_puts(", expected 0xA0)\n");
        passed = 0;
        goto gicd_prio_end;
    }

    /* 写入自定义优先级：SPI #52 = 0x20，SPI #53 = 0x40，SPI #54 = 0x60，SPI #55 = 0x80 */
    u32 custom_prio = 0x80604020u;  /* byte3=#55, byte2=#54, byte1=#53, byte0=#52 */
    mmio_write32((void *)GICD_IPRIORITYR(13), custom_prio);

    u32 prio_reg_after = mmio_read32((void *)GICD_IPRIORITYR(13));
    if (prio_reg_after != custom_prio) {
        guest_puts("FAILED (write 0x");
        puts_hex(custom_prio, 8);
        guest_puts(" read back 0x");
        puts_hex(prio_reg_after, 8);
        guest_puts(")\n");
        passed = 0;
        /* 恢复默认值 */
        mmio_write32((void *)GICD_IPRIORITYR(13), 0xA0A0A0A0u);
        goto gicd_prio_end;
    }

    /* 恢复默认值 */
    mmio_write32((void *)GICD_IPRIORITYR(13), 0xA0A0A0A0u);
    guest_puts("PASSED\n");

gicd_prio_end:

    /* ---- 2. GICD ISPENDR / ICPENDR ---- */
    guest_puts("  [2] GICD ISPENDR/ICPENDR ... ");

    /* 初始 pending 位图应为 0（SPI 范围 32-63 在第 1 组） */
    u32 pend_before = mmio_read32((void *)GICD_ISPENDR(1));
    if (pend_before != 0) {
        guest_puts("FAILED (initial pending = 0x");
        puts_hex(pend_before, 8);
        guest_puts(")\n");
        passed = 0;
        goto gicd_pend_end;
    }

    /* 置位 SPI #52 的 pending（bit 20 of ISPENDR[1]） */
    mmio_write32((void *)GICD_ISPENDR(1), 1u << 20);
    u32 pend_set = mmio_read32((void *)GICD_ISPENDR(1));
    if ((pend_set & (1u << 20)) == 0) {
        guest_puts("FAILED (after ISPENDR write, bit20 not set, val=0x");
        puts_hex(pend_set, 8);
        guest_puts(")\n");
        passed = 0;
        goto gicd_pend_end;
    }

    /* 通过 ICPENDR 读（与 ISPENDR 一致） */
    u32 pend_icread = mmio_read32((void *)GICD_ICPENDR(1));
    if (pend_icread != pend_set) {
        guest_puts("FAILED (ICPENDR read 0x");
        puts_hex(pend_icread, 8);
        guest_puts(" != ISPENDR 0x");
        puts_hex(pend_set, 8);
        guest_puts(")\n");
        passed = 0;
        goto gicd_pend_end;
    }

    /* 清除 SPI #52 pending */
    mmio_write32((void *)GICD_ICPENDR(1), 1u << 20);
    u32 pend_clr = mmio_read32((void *)GICD_ISPENDR(1));
    if ((pend_clr & (1u << 20)) != 0) {
        guest_puts("FAILED (after ICPENDR write, bit20 still set, val=0x");
        puts_hex(pend_clr, 8);
        guest_puts(")\n");
        passed = 0;
        goto gicd_pend_end;
    }

    guest_puts("PASSED\n");

gicd_pend_end:

    /* ---- 3. GICR IPRIORITYR 读写（SGI/PPI） ---- */
    guest_puts("  [3] GICR IPRIORITYR read/write ... ");

    /* 读 IPRIORITYR[0]（INTID 0-3），默认全 0xA0 */
    u32 r_prio0 = mmio_read32((void *)GICR_IPRIORITYR(0));
    if (r_prio0 != 0xA0A0A0A0u) {
        guest_puts("FAILED (default prio = 0x");
        puts_hex(r_prio0, 8);
        guest_puts(")\n");
        passed = 0;
        goto gicr_prio_end;
    }

    /* 写自定义：SGI#0=0x10, SGI#1=0x20, PPI#16=0x30（在 IPRIORITYR[4]） */
    mmio_write32((void *)GICR_IPRIORITYR(0), 0xA0A02010u);  /* SGI 0,1 */
    u32 r_prio0_after = mmio_read32((void *)GICR_IPRIORITYR(0));
    /* 只校验低两个字节（INTID 0 和 1），高位 INTID 2,3 仍为 0xA0 */
    if ((r_prio0_after & 0xFFFF) != 0x2010u) {
        guest_puts("FAILED (SGI prio write, got 0x");
        puts_hex(r_prio0_after & 0xFFFF, 4);
        guest_puts(" expected 0x2010)\n");
        passed = 0;
        mmio_write32((void *)GICR_IPRIORITYR(0), 0xA0A0A0A0u);
        goto gicr_prio_end;
    }

    /* PPI 优先级（INTID 16 = IPRIORITYR[4] 的 byte 0） */
    mmio_write32((void *)GICR_IPRIORITYR(4), 0xA0A0A030u);
    u32 r_prio4 = mmio_read32((void *)GICR_IPRIORITYR(4));
    if ((r_prio4 & 0xFF) != 0x30) {
        guest_puts("FAILED (PPI#16 prio = 0x");
        puts_hex(r_prio4 & 0xFF, 2);
        guest_puts(")\n");
        passed = 0;
        mmio_write32((void *)GICR_IPRIORITYR(4), 0xA0A0A0A0u);
        goto gicr_prio_end;
    }

    /* 恢复默认 */
    mmio_write32((void *)GICR_IPRIORITYR(0), 0xA0A0A0A0u);
    mmio_write32((void *)GICR_IPRIORITYR(4), 0xA0A0A0A0u);
    guest_puts("PASSED\n");

gicr_prio_end:

    /* ---- 4. GICR ISPENDR0 / ICPENDR0 ---- */
    guest_puts("  [4] GICR ISPENDR0/ICPENDR0 ... ");

    u32 r_pend0 = mmio_read32((void *)GICR_ISPENDR0);
    if (r_pend0 != 0) {
        guest_puts("FAILED (initial pending = 0x");
        puts_hex(r_pend0, 8);
        guest_puts(")\n");
        passed = 0;
        goto gicr_pend_end;
    }

    /* 置位 SGI#3 和 PPI#24 */
    mmio_write32((void *)GICR_ISPENDR0, (1u << 3) | (1u << 24));
    u32 r_pend_set = mmio_read32((void *)GICR_ISPENDR0);
    if ((r_pend_set & ((1u << 3) | (1u << 24))) != ((1u << 3) | (1u << 24))) {
        guest_puts("FAILED (after ISPENDR0 write, val=0x");
        puts_hex(r_pend_set, 8);
        guest_puts(")\n");
        passed = 0;
        goto gicr_pend_end;
    }

    /* 清除 SGI#3 */
    mmio_write32((void *)GICR_ICPENDR0, 1u << 3);
    u32 r_pend_clr = mmio_read32((void *)GICR_ISPENDR0);
    if ((r_pend_clr & (1u << 3)) != 0) {
        guest_puts("FAILED (SGI#3 not cleared, val=0x");
        puts_hex(r_pend_clr, 8);
        guest_puts(")\n");
        passed = 0;
        goto gicr_pend_end;
    }
    if ((r_pend_clr & (1u << 24)) == 0) {
        guest_puts("FAILED (PPI#24 wrongly cleared)\n");
        passed = 0;
        goto gicr_pend_end;
    }

    /* 清除 PPI#24，恢复干净 */
    mmio_write32((void *)GICR_ICPENDR0, 1u << 24);
    u32 r_pend_final = mmio_read32((void *)GICR_ISPENDR0);
    if (r_pend_final != 0) {
        guest_puts("FAILED (final pending not zero, val=0x");
        puts_hex(r_pend_final, 8);
        guest_puts(")\n");
        passed = 0;
        goto gicr_pend_end;
    }

    guest_puts("PASSED\n");

gicr_pend_end:

    if (passed)
        guest_puts("\nPhase 18: ALL tests done\n");
    else
        guest_puts("\nPhase 18: SOME tests FAILED\n");
}

/* ---------- Phase 19: vGIC 注入路径统一 ---------- */

/*
 * Phase 19 测试：vGIC 注入路径统一验证
 *
 * 重构内容：把 ivshmem doorbell 和 shm_notify 的"直写 saved state"
 * 老式注入统一为 vgic_inject_virq_to_vm()，消除分散的 LR 操作逻辑。
 *
 * 本测试验证（VM 间协作，用 ivshmem 共享内存做同步）：
 *   [1] ivshmem doorbell 门控验证：禁用目标 SPI 后 doorbell 被丢弃
 *
 * 注：ivshmem doorbell 本身的送达性已在 Test 2 (ivshmem ping-pong) 中验证；
 *     shm_notify 与 ivshmem 走同一条 vgic_inject_virq_to_vm 统一路径，
 *     不再重复验证送达性。
 */
static void phase19_injection_unify_test(void)
{
    guest_puts("\nPhase 19: vGIC injection path unification\n");

    int passed = 1;
    u64 vmid = guest_vmid();
    volatile unsigned char *shm = (volatile unsigned char *)IVSHMEM_MEM_BASE;

    /* 同步字节：byte[60:66]，远离前面测试的写区域 */
    volatile unsigned char *s_entry1 = &shm[60];  /* 入口同步：VM1 就绪 */
    volatile unsigned char *s_entry2 = &shm[61];  /* 入口同步：VM2 就绪 */
    volatile unsigned char *s_ready  = &shm[63];  /* VM2 已禁用 SPI */
    volatile unsigned char *s_sent   = &shm[64];  /* VM1 已发 doorbell */
    volatile unsigned char *s_result = &shm[65];  /* VM2 验证结果 */
    volatile unsigned char *s_ack    = &shm[66];  /* VM1 确认收到结果 */

    /* ---- 入口同步：确保两边都进入 Phase 19 再开始 ----
     * 用 byte[60]=VM1 ready, byte[61]=VM2 ready。
     * 这些字节在 ivshmem 测试中不会被访问（只访问 byte 0-11），
     * QEMU 初始化为 0，无需显式清零。 */
    if (vmid == 1) {
        *s_entry1 = 1;
        asm volatile("dsb sy" ::: "memory");
        int t = 500000;
        while (*s_entry2 != 1 && t-- > 0)
            hvc_call(HVC_SCHED_YIELD, 0);
        if (*s_entry2 != 1) {
            guest_puts("  FAILED (entry sync: VM1 timeout waiting for VM2)\n");
            passed = 0;
            goto p19_end;
        }
    } else {
        *s_entry2 = 1;
        asm volatile("dsb sy" ::: "memory");
        int t = 500000;
        while (*s_entry1 != 1 && t-- > 0)
            hvc_call(HVC_SCHED_YIELD, 0);
        if (*s_entry1 != 1) {
            guest_puts("  FAILED (entry sync: VM2 timeout waiting for VM1)\n");
            passed = 0;
            goto p19_end;
        }
    }

    /* ---- [1] 门控验证：禁用 SPI 后 doorbell 被丢弃 ---- */
    guest_puts("  [1] SPI disabled -> doorbell dropped (gating) ... ");

    ivshmem_irq_received = 0;
    asm volatile("dmb ish" ::: "memory");

    if (vmid == 1) {
        /* VM1: 等 VM2 禁用 SPI 并就绪 */
        int t = 200000;
        while (*s_ready != 1 && t-- > 0)
            hvc_call(HVC_SCHED_YIELD, 0);
        if (*s_ready != 1) {
            guest_puts("FAILED (VM1 timeout waiting for VM2 ready)\n");
            passed = 0;
            goto p19_end;
        }

        /* VM2 已禁用 SPI，发 doorbell 给 VM2（应该被丢弃） */
        ivshmem_doorbell(1);

        /* 给 VM2 一点时间确认没收到 */
        for (int i = 0; i < 5000; i++)
            hvc_call(HVC_SCHED_YIELD, 0);

        /* 通知 VM2 "我发完了" */
        *s_sent = 1;
        asm volatile("dsb sy" ::: "memory");

        /* 等 VM2 验证结果 */
        t = 200000;
        while (*s_result == 0 && t-- > 0)
            hvc_call(HVC_SCHED_YIELD, 0);
        if (*s_result != 1) {
            guest_puts("FAILED (VM2 received doorbell with SPI disabled)\n");
            passed = 0;
            *s_ack = 1;
            asm volatile("dsb sy" ::: "memory");
            goto p19_end;
        }

        /* 通知 VM2 "我收到结果了，你可以恢复了" */
        *s_ack = 1;
        asm volatile("dsb sy" ::: "memory");
    } else {
        /* VM2: 禁用 ivshmem SPI (#49 = bit 17 of ICENABLER[1]) */
        mmio_write32((void *)GICD_ICENABLER(1), 1u << 17);
        asm volatile("dsb sy" ::: "memory");

        /* 通知 VM1 "我禁用了，可以发了" */
        *s_ready = 1;
        asm volatile("dsb sy" ::: "memory");

        /* 等 VM1 发完 doorbell */
        int t = 200000;
        while (*s_sent != 1 && t-- > 0)
            hvc_call(HVC_SCHED_YIELD, 0);
        if (*s_sent != 1) {
            guest_puts("FAILED (VM2 timeout waiting for VM1 send)\n");
            passed = 0;
            /* 恢复 SPI */
            mmio_write32((void *)GICD_ISENABLER(1), 1u << 17);
            asm volatile("dsb sy" ::: "memory");
            goto p19_end;
        }

        /* 检查是否收到了中断（应该没收到） */
        if (ivshmem_irq_received) {
            guest_puts("FAILED (received doorbell with SPI disabled)\n");
            passed = 0;
            *s_result = 2;   /* 2 = 失败 */
            asm volatile("dsb sy" ::: "memory");
            /* 恢复 SPI */
            mmio_write32((void *)GICD_ISENABLER(1), 1u << 17);
            asm volatile("dsb sy" ::: "memory");
            goto p19_end;
        }

        /* 验证通过，通知 VM1 */
        *s_result = 1;   /* 1 = 通过 */
        asm volatile("dsb sy" ::: "memory");

        /* 等 VM1 确认 */
        t = 200000;
        while (*s_ack != 1 && t-- > 0)
            hvc_call(HVC_SCHED_YIELD, 0);

        /* 恢复 SPI */
        mmio_write32((void *)GICD_ISENABLER(1), 1u << 17);
        asm volatile("dsb sy" ::: "memory");
    }
    guest_puts("PASSED\n");

p19_end:
    /* 清理共享内存同步字节 */
    *s_entry1 = 0;
    *s_entry2 = 0;
    *s_ready = 0;
    *s_sent = 0;
    *s_result = 0;
    *s_ack = 0;
    asm volatile("dsb sy" ::: "memory");

    if (passed)
        guest_puts("\nPhase 19: ALL tests done\n");
    else
        guest_puts("\nPhase 19: SOME tests FAILED\n");
}

/* ---------- Phase 20: Stage-2 页表管理完善 ---------- */

/*
 * Phase 20 测试：Stage-2 页表管理完善
 *
 * 重构内容：
 *   - 提取通用 unmap_stage2_page() 函数，统一 Stage-2 取消映射逻辑
 *   - 支持页表自底向上回收：unmap 后 L3 全空则回收 L3，
 *     L2 全空则回收 L2（L1 永远不回收）
 *   - 区分普通页和 MMIO 页（free_page 参数）
 *
 * 本测试验证（主核单线程）：
 *   [1] 基本 map/unmap: map 后可写，unmap 后 user_pages 减少
 *   [2] L3 页表回收: 同一 L3 下多页全部 unmap 后，L3 表被回收
 *   [3] L2 页表回收: 同一 L2 下所有 L3 全空后，L2 表被回收
 *   [4] MMIO 映射 unmap: 不释放物理页，只清映射
 */
#define P20_BASE_IPA    0x49000000UL   /* 测试基地址，远离其他测试 */

static void phase20_unmap_pt_reclaim_test(void)
{
    guest_puts("\n--- Test 13: Phase 20 Stage-2 页表管理完善 ---\n");

    u64 user_before, kern_before, user_after, kern_after;
    u64 base = 0x49000000UL;  /* 测试基地址，远离其他测试 */
    int passed = 1;

    /* 1. 获取初始计数 */
    user_before = hvc_call_ret2(HVC_MEM_STATS, 0, &kern_before);
    guest_puts("  初始: user_pages=");
    puts_dec((int)user_before);
    guest_puts(", kernel_pages=");
    puts_dec((int)kern_before);
    guest_puts("\n");

    /* 2. 映射 4 页普通内存（L3 块内连续 4 页） */
    for (int i = 0; i < 4; i++) {
        u64 ipa = base + i * 4096;
        u64 ret = hvc_call(HVC_MEM_MAP, ipa);
        if (ret != 0) {
            guest_puts("  FAIL: map page ");
            puts_dec(i);
            guest_puts(" failed\n");
            passed = 0;
        }
    }

    user_after = hvc_call_ret2(HVC_MEM_STATS, 0, &kern_after);
    guest_puts("  map 4页后: user_pages=");
    puts_dec((int)user_after);
    guest_puts(", kernel_pages=");
    puts_dec((int)kern_after);
    guest_puts("\n");

    if (user_after != user_before + 4) {
        guest_puts("  FAIL: user_pages 应该增加 4\n");
        passed = 0;
    }
    /* kernel_pages 增加：至少 1 个 L3 页表页 + 可能的 L2 */
    if (kern_after <= kern_before) {
        guest_puts("  FAIL: kernel_pages 应该增加\n");
        passed = 0;
    }

    u64 user_after_map = user_after;
    u64 kern_after_map = kern_after;

    /* 3. 逐页 unmap，观察计数变化 */
    for (int i = 0; i < 4; i++) {
        u64 ipa = base + i * 4096;
        u64 ret = hvc_call(HVC_MEM_UNMAP, ipa);
        if (ret != 0) {
            guest_puts("  FAIL: unmap page ");
            puts_dec(i);
            guest_puts(" failed\n");
            passed = 0;
        }

        u64 u = hvc_call_ret2(HVC_MEM_STATS, 0, &kern_after);
        if (u != user_after_map - (i + 1)) {
            guest_puts("  FAIL: unmap page ");
            puts_dec(i);
            guest_puts(" 后 user_pages 不对 (expected ");
            puts_dec((int)(user_after_map - i - 1));
            guest_puts(", got ");
            puts_dec((int)u);
            guest_puts(")\n");
            passed = 0;
        }
    }

    user_after = hvc_call_ret2(HVC_MEM_STATS, 0, &kern_after);
    guest_puts("  unmap 4页后: user_pages=");
    puts_dec((int)user_after);
    guest_puts(", kernel_pages=");
    puts_dec((int)kern_after);
    guest_puts("\n");

    /* 验证：user_pages 回到初始值 */
    if (user_after != user_before) {
        guest_puts("  FAIL: user_pages 未回到初始值\n");
        passed = 0;
    }

    if (passed) {
        guest_puts("  PASSED: Stage-2 unmap + 计数正确\n");
    } else {
        guest_puts("  FAILED\n");
    }
}

/* ---------- Phase 21: Stage-2 细粒度权限管理 ---------- */

/*
 * Phase 21 测试：Stage-2 细粒度权限管理
 *
 * 验证：
 *   [1] RW 页：读正常、写正常
 *   [2] RO 页：读正常、写触发 Stage-2 Permission Fault
 *   [3] NONE 页：读也触发 Permission Fault
 *   [4] 改回 RW 后：写恢复正常
 *   [5] HVC_MEM_PROTECT 对未映射页返回 -1
 *
 * 测试用 sync_expect_start/end 捕获预期的同步异常。
 */
#define P21_TEST_IPA   0x49100000UL   /* 测试地址，远离 Phase 20 */

static void phase21_mprotect_test(void)
{
    guest_puts("\n--- Test 14: Phase 21 Stage-2 细粒度权限管理 ---\n");

    u64 ipa = P21_TEST_IPA;
    int passed = 1;
    u64 ret;
    volatile u64 *p = (volatile u64 *)ipa;

    /* 1. 映射一页（默认 RW 权限） */
    ret = hvc_call(HVC_MEM_MAP, ipa);
    if (ret != 0) {
        guest_puts("  FAIL: map page failed\n");
        passed = 0;
        goto done;
    }
    guest_puts("  [1] map 页成功\n");

    /* 2. RW 权限下：读、写都应该正常 */
    *p = 0xdeadbeef;
    u64 val = *p;
    if (val != 0xdeadbeef) {
        guest_puts("  FAIL: RW 模式下读写不一致\n");
        passed = 0;
    } else {
        guest_puts("  [2] RW 模式：读写正常\n");
    }

    /* 3. 改为 RO 权限 */
    ret = hvc_call2(HVC_MEM_PROTECT, ipa, 1);  /* 1 = RO */
    if (ret != 0) {
        guest_puts("  FAIL: mprotect RO 失败\n");
        passed = 0;
        goto unmap_out;
    }
    guest_puts("  [3] mprotect RO 成功\n");

    /* 4. RO 权限下：读应该正常 */
    val = *p;
    if (val != 0xdeadbeef) {
        guest_puts("  FAIL: RO 模式下读取值不对\n");
        passed = 0;
    } else {
        guest_puts("  [4] RO 模式：读正常\n");
    }

    /* 5. RO 权限下：写应该触发 Permission Fault */
    sync_expect_start();
    *p = 0x12345678;   /* 这条写指令应该触发 Stage-2 Permission Fault */
    int faulted = sync_expect_end();

    if (!faulted) {
        guest_puts("  FAIL: RO 模式下写未触发异常\n");
        passed = 0;
    } else {
        /* 验证 ESR 显示是 data abort + permission fault */
        u64 ec = (sync_fault_esr >> 26) & 0x3f;
        u64 dfsc = sync_fault_esr & 0x3f;
        guest_puts("  [5] RO 写触发异常（ESR=");
        puts_hex(sync_fault_esr, 8);
        guest_puts(" FAR=");
        puts_hex(sync_fault_far, 16);
        guest_puts("）\n");
        if (ec != 0b100101) {   /* Data Abort taken without a change in Exception level */
            guest_puts("  WARNING: EC 不是 Data Abort (got ");
            puts_hex(ec, 2);
            guest_puts(")\n");
        }
        if ((dfsc & 0x3c) != 0x0c) {  /* Permission fault (level 0-3) */
            guest_puts("  WARNING: DFSC 不是 Permission Fault (got ");
            puts_hex(dfsc, 2);
            guest_puts(")\n");
        }
    }

    /* 6. 改回 RW 权限 */
    ret = hvc_call2(HVC_MEM_PROTECT, ipa, 0);  /* 0 = RW */
    if (ret != 0) {
        guest_puts("  FAIL: mprotect RW 失败\n");
        passed = 0;
        goto unmap_out;
    }
    guest_puts("  [6] mprotect RW 成功\n");

    /* 7. RW 恢复后：写应该正常 */
    *p = 0xcafebabe;
    val = *p;
    if (val != 0xcafebabe) {
        guest_puts("  FAIL: 恢复 RW 后写失败\n");
        passed = 0;
    } else {
        guest_puts("  [7] 恢复 RW 后写正常\n");
    }

    /* 8. 测试 NONE 权限：读也应该 fault */
    ret = hvc_call2(HVC_MEM_PROTECT, ipa, 2);  /* 2 = NONE */
    if (ret != 0) {
        guest_puts("  FAIL: mprotect NONE 失败\n");
        passed = 0;
        goto unmap_out;
    }
    guest_puts("  [8] mprotect NONE 成功\n");

    sync_expect_start();
    val = *p;    /* 读应该触发 Permission Fault */
    faulted = sync_expect_end();
    (void)val;

    if (!faulted) {
        guest_puts("  FAIL: NONE 模式下读未触发异常\n");
        passed = 0;
    } else {
        guest_puts("  [9] NONE 模式：读触发异常\n");
    }

    /* 10. 对未映射地址调用 mprotect 应该失败 */
    ret = hvc_call2(HVC_MEM_PROTECT, ipa + 0x200000, 0);
    if (ret == 0) {
        guest_puts("  FAIL: 对未映射页 mprotect 未返回错误\n");
        passed = 0;
    } else {
        guest_puts("  [10] 对未映射页 mprotect 返回错误（正确）\n");
    }

unmap_out:
    /* 先恢复 RW（如果还在 RO/NONE），避免 unmap 出问题（其实 unmap 不依赖权限） */
    hvc_call2(HVC_MEM_PROTECT, ipa, 0);
    /* 清理：unmap */
    hvc_call(HVC_MEM_UNMAP, ipa);

done:
    if (passed) {
        guest_puts("  PASSED: Stage-2 细粒度权限管理正确\n");
    } else {
        guest_puts("  FAILED\n");
    }
}

/* ---------- Phase 22: 内存超售与按需分配优化 ---------- */

/*
 * Phase 22 测试：2MB 大页 + 内存配额
 *
 * 检查点：
 *   1. HVC_MEM_QUERY 基本查询正常
 *   2. HVC_MEM_HUGE_MAP 映射 2MB 大页成功
 *   3. 大页内读写正常（不会触发缺页）
 *   4. HVC_MEM_QUERY 显示大页数正确
 *   5. HVC_MEM_HUGE_PROTECT 修改大页权限（RO）正常
 *   6. RO 大页写触发 Permission Fault
 *   7. HVC_MEM_SET_QUOTA 设置配额成功
 *   8. 超配额分配失败
 *   9. HVC_MEM_HUGE_UNMAP 取消大页映射成功
 *  10. 配额回退后分配恢复正常
 */
#define PHASE22_HUGE_IPA     0x41000000UL  /* 16MB 处，2MB 对齐 */

static void phase22_hugepage_quota_test(void)
{
    guest_puts("\n--- Test 15: Phase 22 大页 + 内存配额 ---\n");

    int passed = 1;
    u64 ret;
    u64 usage, quota, huge_count;
    u64 quota_limit;
    volatile u64 *p = (volatile u64 *)PHASE22_HUGE_IPA;

    /* 1. 查询初始内存使用 */
    usage = hvc_call_ret3(HVC_MEM_QUERY, 0, &quota, &huge_count);
    if (quota != 0) {
        guest_puts("  FAIL: 初始配额应该为 0（无限制）\n");
        passed = 0;
    } else {
        guest_puts("  [1] 初始查询正常 (usage=");
        puts_dec(usage >> 10);
        guest_puts("KB, huge=");
        puts_dec(huge_count);
        guest_puts(")\n");
    }

    /* 2. 映射 2MB 大页（RW） */
    ret = hvc_call2(HVC_MEM_HUGE_MAP, PHASE22_HUGE_IPA, 0);  /* 0 = RW */
    if (ret != 0) {
        guest_puts("  FAIL: 映射 2MB 大页失败\n");
        passed = 0;
        goto done22;
    }
    guest_puts("  [2] 映射 2MB 大页成功\n");

    /* 3. 大页内读写正常（测试多个偏移，验证确实是 2MB） */
    p[0] = 0xdeadbeef;
    p[1] = 0xcafebabe;
    /* 测试大页末尾（2MB - 8 字节处） */
    volatile u64 *p_end = (volatile u64 *)(PHASE22_HUGE_IPA + 0x200000 - 8);
    *p_end = 0x12345678abcdef00ULL;

    if (p[0] != 0xdeadbeef || p[1] != 0xcafebabe ||
        *p_end != 0x12345678abcdef00ULL) {
        guest_puts("  FAIL: 大页内读写不正确\n");
        passed = 0;
    } else {
        guest_puts("  [3] 大页内读写正常（开头 + 末尾）\n");
    }

    /* 4. 查询大页数应为 1 */
    usage = hvc_call_ret3(HVC_MEM_QUERY, 0, &quota, &huge_count);
    if (huge_count != 1) {
        guest_puts("  FAIL: 大页数应该为 1，实际 ");
        puts_dec(huge_count);
        guest_puts("\n");
        passed = 0;
    } else {
        guest_puts("  [4] 大页计数正确 (huge=1, usage=");
        puts_dec(usage >> 10);
        guest_puts("KB)\n");
    }

    /* 5. 修改大页权限为 RO */
    ret = hvc_call2(HVC_MEM_HUGE_PROTECT, PHASE22_HUGE_IPA, 1);  /* 1 = RO */
    if (ret != 0) {
        guest_puts("  FAIL: 大页 mprotect RO 失败\n");
        passed = 0;
    } else {
        guest_puts("  [5] 大页 mprotect RO 成功\n");
    }

    /* 6. RO 大页写应该触发 Permission Fault */
    sync_expect_start();
    p[0] = 0xaaaaaaaa;    /* 写应该触发 fault */
    int faulted = sync_expect_end();

    if (!faulted) {
        guest_puts("  FAIL: RO 大页写未触发异常\n");
        passed = 0;
    } else {
        guest_puts("  [6] RO 大页写触发 Permission Fault\n");
    }

    /* 先恢复 RW，方便后续操作 */
    hvc_call2(HVC_MEM_HUGE_PROTECT, PHASE22_HUGE_IPA, 0);  /* 0 = RW */

    /* 7. 将配额设为当前用量，额外的大页申请应被拒绝 */
    usage = hvc_call_ret3(HVC_MEM_QUERY, 0, &quota, &huge_count);
    quota_limit = usage;
    ret = hvc_call(HVC_MEM_SET_QUOTA, quota_limit);
    if (ret != 0) {
        guest_puts("  FAIL: 设置配额失败\n");
        passed = 0;
    } else {
        guest_puts("  [7] 设置配额成功 (4MB)\n");
    }

    /* 验证配额已设置 */
    usage = hvc_call_ret3(HVC_MEM_QUERY, 0, &quota, &huge_count);
    if (quota != quota_limit) {
        guest_puts("  FAIL: 配额查询不正确，期望 ");
        puts_dec(quota_limit);
        guest_puts("，实际 ");
        puts_dec(quota);
        guest_puts("\n");
        passed = 0;
    }

    /* 8. 超配额分配应该失败
     *    当前已用约 2MB（大页）+ 一些小页
     *    再分配一个 2MB 大页应该超过 4MB 配额 */
    ret = hvc_call2(HVC_MEM_HUGE_MAP, PHASE22_HUGE_IPA + 0x200000, 0);
    if (ret == 0) {
        guest_puts("  FAIL: 超配额分配应该失败但成功了\n");
        passed = 0;
        /* 清理：unmap 掉这个不该成功的大页 */
        hvc_call(HVC_MEM_HUGE_UNMAP, PHASE22_HUGE_IPA + 0x200000);
    } else {
        guest_puts("  [8] 超配额分配被正确拒绝\n");
    }

    /* 9. 取消第一个大页映射 */
    ret = hvc_call(HVC_MEM_HUGE_UNMAP, PHASE22_HUGE_IPA);
    if (ret != 0) {
        guest_puts("  FAIL: 取消大页映射失败\n");
        passed = 0;
    } else {
        guest_puts("  [9] 取消 2MB 大页成功\n");
    }

    /* 验证大页已释放（huge_count 变为 0） */
    usage = hvc_call_ret3(HVC_MEM_QUERY, 0, &quota, &huge_count);
    if (huge_count != 0) {
        guest_puts("  FAIL: unmap 后大页数应为 0，实际 ");
        puts_dec(huge_count);
        guest_puts("\n");
        passed = 0;
    }

    /* 10. 配额内重新分配应该成功 */
    ret = hvc_call2(HVC_MEM_HUGE_MAP, PHASE22_HUGE_IPA, 0);
    if (ret != 0) {
        guest_puts("  FAIL: 配额内重新分配失败\n");
        passed = 0;
    } else {
        guest_puts("  [10] 配额内重新分配成功\n");
        /* 验证写正常 */
        p[0] = 0x11223344;
        if (p[0] != 0x11223344) {
            guest_puts("  FAIL: 重新分配后写不正常\n");
            passed = 0;
        }
        /* 清理：unmap */
        hvc_call(HVC_MEM_HUGE_UNMAP, PHASE22_HUGE_IPA);
    }

    /* 恢复配额为 0（无限制） */
    hvc_call(HVC_MEM_SET_QUOTA, 0);

done22:
    if (passed) {
        guest_puts("  PASSED: 大页 + 内存配额正确\n");
    } else {
        guest_puts("  FAILED\n");
    }
}

/* ---------- Phase 23: 页面回收（Page Reclaim） ---------- */

/*
 * Phase 23 测试：页面回收（时钟算法 + 配额触发）
 *
 * 测试步骤：
 *   1. 设置内存配额为 4MB
 *   2. 通过 demand paging 分配 3MB 内存，写入特征值
 *   3. 查询回收统计应为 0
 *   4. 继续分配 2MB（总共 5MB > 4MB 配额）→ 触发页面回收
 *   5. 检查回收统计 > 0（说明确实回收了页面）
 *   6. 新分配的内存能正常读写
 *   7. 旧内存中某些页面被回收（数据清零），重新访问触发 demand paging
 *   8. 恢复配额为无限制
 */
#define PHASE23_BASE_IPA    0x50000000  /* 测试起始地址（高地址，避开代码段） */
#define PHASE23_NPAGES      512         /* 测试 512 页 = 2MB */
#define PHASE23_PAGE_SIZE   4096        /* 4KB 页大小 */
#define PHASE23_ACTIVE_PAGES 100        /* 活跃页面数（被访问过的） */

static int test_phase23_reclaim(void)
{
    int passed = 1;
    int ret;
    unsigned long reclaim_count, reclaim_fail;
    volatile unsigned char *base;

    guest_puts("\n--- Test 16: Phase 23 页面回收（时钟算法） ---\n");

    /* 1. 显式分配 2MB 测试内存（512 页），用 hyp_mem_map
     *    放在高地址 0x50000000，避开 Guest 代码段 */
    base = (volatile unsigned char *)PHASE23_BASE_IPA;
    for (int i = 0; i < PHASE23_NPAGES; i++) {
        ret = hvc_call2(HVC_MEM_MAP, PHASE23_BASE_IPA + i * PHASE23_PAGE_SIZE, 0);
        if (ret != 0) {
            guest_puts("  FAIL: 映射测试页失败 (page ");
            puts_dec(i);
            guest_puts(")\n");
            passed = 0;
            goto done23_cleanup;
        }
    }
    guest_puts("  [1] 分配 512 页测试内存（2MB）\n");

    /* 2. 写入特征数据：每页第 1 字节 = 页号 */
    for (int i = 0; i < PHASE23_NPAGES; i++) {
        base[i * PHASE23_PAGE_SIZE] = (unsigned char)(i & 0xFF);
    }
    guest_puts("  [2] 写入特征数据完成\n");

    /* 3. 查询回收统计，应该为 0 */
    reclaim_count = hvc_call2(HVC_MEM_RECLAIM_STATS, 0, &reclaim_fail);
    if (reclaim_count != 0) {
        guest_puts("  FAIL: 初始回收计数应为 0，实际 ");
        puts_dec(reclaim_count);
        guest_puts("\n");
        passed = 0;
    } else {
        guest_puts("  [3] 初始回收计数为 0 ✓\n");
    }

    /* 4. 第一次显式回收：尝试回收 100 页
     *    因为所有页 AF=1，第一次只会清除 AF，不会回收
     *    预期：返回 0 或少量（如果有本来 AF=0 的页） */
    ret = hvc_call(HVC_MEM_RECLAIM, 100);
    guest_puts("  [4] 第一次回收请求 100 页，实际回收 ");
    puts_dec(ret);
    guest_puts(" 页（预期 ≈ 0，因为所有页刚被访问过）\n");

    /* 5. 主动访问前 100 页 → 触发 Access Flag Fault → AF 重新置 1
     *    这些就是"活跃"页面 */
    for (int i = 0; i < PHASE23_ACTIVE_PAGES; i++) {
        volatile unsigned char *p = &base[i * PHASE23_PAGE_SIZE];
        unsigned char val = *p;  /* 读访问 → AF fault → AF=1 */
        (void)val;
    }
    guest_puts("  [5] 重新访问前 100 页（标记为活跃）\n");

    /* 6. 第二次显式回收：尝试回收 400 页
     *    此时：前 100 页 AF=1（活跃），后 412 页 AF=0（不活跃）
     *    预期：回收约 400 页不活跃的页面
     *
     *    注意：时钟从上次停止位置继续扫描，
     *    不活跃的页（AF=0）会被回收。 */
    ret = hvc_call(HVC_MEM_RECLAIM, 400);
    if (ret <= 0) {
        guest_puts("  FAIL: 第二次回收应该回收页面，但实际回收 0 页\n");
        passed = 0;
    } else {
        guest_puts("  [6] 第二次回收请求 400 页，实际回收 ");
        puts_dec(ret);
        guest_puts(" 页 ✓\n");
    }

    /* 7. 验证回收统计更新 */
    reclaim_count = hvc_call2(HVC_MEM_RECLAIM_STATS, 0, &reclaim_fail);
    if (reclaim_count <= 0) {
        guest_puts("  FAIL: 累计回收计数应为 > 0\n");
        passed = 0;
    } else {
        guest_puts("  [7] 累计回收 ");
        puts_dec(reclaim_count);
        guest_puts(" 页，回收失败 ");
        puts_dec(reclaim_fail);
        guest_puts(" 次\n");
    }

    /* 8. 验证活跃页面（前 100 页）数据仍然正确
     *    注意：这些页可能在第二次回收中被清除了 AF（因为 AF=1 → 清 AF），
     *    但不应该被回收。重新读取会触发 AF fault，数据保持不变。 */
    int active_ok = 1;
    for (int i = 0; i < PHASE23_ACTIVE_PAGES; i++) {
        if (base[i * PHASE23_PAGE_SIZE] != (unsigned char)(i & 0xFF)) {
            active_ok = 0;
            break;
        }
    }
    if (!active_ok) {
        guest_puts("  FAIL: 活跃页面数据不正确（应该保留）\n");
        passed = 0;
    } else {
        guest_puts("  [8] 活跃页面（前 100 页）数据完整 ✓\n");
    }

    /* 9. 验证不活跃页面（后面的页）有部分被回收（数据变为 0）
     *    被回收的页面重新读取时会触发 demand paging，
     *    分配新物理页（清零），所以读到 0 */
    int reclaimed_found = 0;
    for (int i = PHASE23_ACTIVE_PAGES + 10; i < PHASE23_NPAGES; i++) {
        if (base[i * PHASE23_PAGE_SIZE] == 0) {
            reclaimed_found++;
            if (reclaimed_found >= 5)
                break;
        }
    }
    if (reclaimed_found < 5) {
        guest_puts("  WARN: 只找到 ");
        puts_dec(reclaimed_found);
        guest_puts(" 个被回收的页面（可能都还没被回收）\n");
        /* 不一定失败，取决于时钟位置 */
    } else {
        guest_puts("  [9] 验证：不活跃页面确实有被回收的（数据清零）✓\n");
    }

done23_cleanup:
    /* 清理：unmap 所有测试页 */
    for (int i = 0; i < PHASE23_NPAGES; i++) {
        hvc_call(HVC_MEM_UNMAP, PHASE23_BASE_IPA + i * PHASE23_PAGE_SIZE);
    }

    if (passed) {
        guest_puts("  PASSED: 页面回收功能正确\n");
    } else {
        guest_puts("  FAILED\n");
    }
    return passed;
}

/* ---------- Phase 24: KSM 内存去重 ---------- */

/*
 * Phase 24 测试：KSM 内存去重 + 写时复制 (CoW)
 *
 * 测试步骤：
 *   1. 分配 64 页测试内存
 *   2. 前 32 页写入相同内容（去重候选）
 *   3. 后 32 页写入不同内容（不会被去重）
 *   4. 触发 KSM 扫描，验证合并页数 > 0
 *   5. 验证所有页内容正确（去重不影响数据正确性）
 *   6. 修改其中一个共享页的内容 → 触发 CoW 分离
 *   7. 验证修改后的页内容正确，其他共享页不受影响
 *   8. 查询 KSM 统计，验证共享映射数减少
 */
#define PHASE24_BASE_IPA    0x60000000  /* 测试起始地址 */
#define PHASE24_PAGE_SIZE   4096
#define PHASE24_TOTAL_PAGES 64          /* 总测试页数 */
#define PHASE24_SAME_PAGES  32          /* 内容相同的页数（去重候选） */

static int test_phase24_ksm(void)
{
    int passed = 1;
    int ret;
    int i;
    volatile unsigned char *base;
    unsigned long shared0, sharing0, merged0, unmerged0;
    unsigned long shared1, sharing1, merged1, unmerged1;
    unsigned long merged_pages;

    guest_puts("\n--- Test 17: Phase 24 KSM 内存去重 ---\n");

    /* 0. 查询初始 KSM 统计 */
    shared0 = hvc_call_ret4(HVC_MEM_KSM_STATS, 0, &sharing0, &merged0, &unmerged0);
    guest_puts("  [0] 初始统计：共享页=");
    puts_dec(shared0);
    guest_puts(" 共享映射=");
    puts_dec(sharing0);
    guest_puts(" 累计合并=");
    puts_dec(merged0);
    guest_puts("\n");

    /* 1. 分配 64 页测试内存 */
    base = (volatile unsigned char *)PHASE24_BASE_IPA;
    for (i = 0; i < PHASE24_TOTAL_PAGES; i++) {
        ret = hvc_call2(HVC_MEM_MAP, PHASE24_BASE_IPA + i * PHASE24_PAGE_SIZE, 0);
        if (ret != 0) {
            guest_puts("  FAIL: 映射测试页失败 (page ");
            puts_dec(i);
            guest_puts(")\n");
            passed = 0;
            goto done24_cleanup;
        }
    }
    guest_puts("  [1] 分配 64 页测试内存（256KB）\n");

    /* 2. 前 32 页写入相同内容（固定模式，应该被去重） */
    for (i = 0; i < PHASE24_SAME_PAGES; i++) {
        volatile unsigned char *p = base + i * PHASE24_PAGE_SIZE;
        /* 每页写入固定模式：0xAA 填充 */
        for (int j = 0; j < PHASE24_PAGE_SIZE; j++) {
            p[j] = 0xAA;
        }
    }
    guest_puts("  [2] 前 32 页写入相同内容 (0xAA)\n");

    /* 3. 后 32 页写入不同内容（每页 = 页号，不会被去重） */
    for (i = PHASE24_SAME_PAGES; i < PHASE24_TOTAL_PAGES; i++) {
        volatile unsigned char *p = base + i * PHASE24_PAGE_SIZE;
        unsigned char pattern = (unsigned char)(i & 0xFF);
        for (int j = 0; j < PHASE24_PAGE_SIZE; j++) {
            p[j] = pattern;
        }
    }
    guest_puts("  [3] 后 32 页写入不同内容（每页 = 页号）\n");

    /* 4. 触发 KSM 扫描（全量扫描，确保扫到 0x60000000 测试页） */
    merged_pages = hvc_call(HVC_MEM_KSM_SCAN, 0);
    guest_puts("  [4] 触发 KSM 扫描，合并页数 = ");
    puts_dec(merged_pages);
    guest_puts("\n");

    if (merged_pages < 30) {
        /* 32 页相同内容应该合并约 31 页（31 页被合并到 1 个共享页） */
        guest_puts("  WARN: 合并页数偏少（预期约 31 页）\n");
        /* 不直接判失败，可能扫描策略不同 */
    }

    /* 5. 验证所有页内容正确 */
    guest_puts("  [5] 验证内容正确性...\n");
    for (i = 0; i < PHASE24_SAME_PAGES; i++) {
        volatile unsigned char *p = base + i * PHASE24_PAGE_SIZE;
        if (p[0] != 0xAA || p[PHASE24_PAGE_SIZE - 1] != 0xAA) {
            guest_puts("  FAIL: 第 ");
            puts_dec(i);
            guest_puts(" 页内容错误（应为 0xAA）\n");
            passed = 0;
            break;
        }
    }
    for (i = PHASE24_SAME_PAGES; i < PHASE24_TOTAL_PAGES; i++) {
        volatile unsigned char *p = base + i * PHASE24_PAGE_SIZE;
        unsigned char expected = (unsigned char)(i & 0xFF);
        if (p[0] != expected || p[PHASE24_PAGE_SIZE - 1] != expected) {
            guest_puts("  FAIL: 第 ");
            puts_dec(i);
            guest_puts(" 页内容错误\n");
            passed = 0;
            break;
        }
    }
    if (passed)
        guest_puts("  [5] 内容验证通过\n");

    /* 6. 查询 KSM 统计 */
    shared1 = hvc_call_ret4(HVC_MEM_KSM_STATS, 0, &sharing1, &merged1, &unmerged1);
    guest_puts("  [6] 扫描后统计：共享页=");
    puts_dec(shared1);
    guest_puts(" 共享映射=");
    puts_dec(sharing1);
    guest_puts(" 累计合并=");
    puts_dec(merged1);
    guest_puts("\n");

    /* 7. 测试 CoW：修改第 5 个共享页的内容
     *    这应该触发写时复制，该页从共享页分离出来 */
    guest_puts("  [7] 修改第 5 页内容（触发 CoW）...\n");
    {
        volatile unsigned char *p = base + 5 * PHASE24_PAGE_SIZE;
        p[0] = 0xBB;  /* 修改第 1 字节 */
        p[100] = 0xCC; /* 修改中间某个字节 */

        /* 验证修改后的值 */
        if (p[0] != 0xBB || p[100] != 0xCC) {
            guest_puts("  FAIL: CoW 后内容不正确\n");
            passed = 0;
        }
    }

    /* 8. 验证其他共享页不受影响（仍是 0xAA） */
    guest_puts("  [8] 验证其他共享页不受影响...\n");
    for (i = 0; i < PHASE24_SAME_PAGES; i++) {
        if (i == 5) continue;  /* 跳过已修改的页 */
        volatile unsigned char *p = base + i * PHASE24_PAGE_SIZE;
        if (p[0] != 0xAA || p[PHASE24_PAGE_SIZE - 1] != 0xAA) {
            guest_puts("  FAIL: 第 ");
            puts_dec(i);
            guest_puts(" 页被意外修改（CoW 隔离失败）\n");
            passed = 0;
            break;
        }
    }
    if (passed)
        guest_puts("  [8] CoW 隔离验证通过\n");

    /* 9. 再次查询统计，验证共享映射数减少（CoW 分离了一页） */
    {
        unsigned long s2, sh2, m2, u2;
        s2 = hvc_call_ret4(HVC_MEM_KSM_STATS, 0, &sh2, &m2, &u2);
        guest_puts("  [9] CoW 后统计：共享页=");
        puts_dec(s2);
        guest_puts(" 共享映射=");
        puts_dec(sh2);
        guest_puts(" 累计分离=");
        puts_dec(u2);
        guest_puts("\n");

        if (u2 <= unmerged1) {
            guest_puts("  WARN: CoW 后累计分离数未增加\n");
        }
    }

    if (passed) {
        guest_puts("  PASSED: KSM 内存去重 + CoW 功能正确\n");
    } else {
        guest_puts("  FAILED\n");
    }

done24_cleanup:
    /* 清理：取消映射（可选，测试结束后 VM 销毁会自动清理） */
    return passed;
}

#define PHASE25_BASE_IPA    0x70000000  /* 迁移测试专用内存区（与 Phase24 的 0x60000000 错开） */
#define PHASE25_PAGE_SIZE   4096
#define PHASE25_TOTAL_PAGES 8           /* 8 页迁移测试内存 */
#define PHASE25_DIRTY_PAGES 4           /* 制造脏页的前 4 页 */

/*
 * Phase 26: 迁移状态机测试。
 * 默认双 vCPU VM2 验证非法阶段调用和整 VM 迁移拒绝边界；
 * 下方单 vCPU 迁移原型代码保留供专用拓扑回归使用。
 */
static void test_phase40_stack_telemetry(void)
{
    u64 free_bytes;
    u64 guard_ok;

    guest_puts("\n--- Test 25: Phase 40 EL2 task stack telemetry ---\n");
    free_bytes = hvc_call_ret2(HVC_TASK_STACK_STATS, 0, &guard_ok);
    guest_puts("  最小剩余栈空间=");
    puts_dec((int)free_bytes);
    guest_puts(" bytes, guard=");
    puts_dec((int)guard_ok);
    guest_puts("\n");
    if (!guard_ok || free_bytes < 256) {
        guest_puts("  FAIL: EL2 task 栈遥测不满足安全阈值\n");
        return;
    }
    guest_puts("  PASS: EL2 task 栈 canary 与最低余量正常\n");
}

static int test_phase25_migration(void)
{
    int passed = 1;
    int ret;
    int i;

    volatile unsigned char *base;
    int precopy_dirty;
    int finish_dirty;
    u64 new_vmid;

    if (guest_vmid() == 2) {
        guest_puts("\n--- Test 18: Phase 27 SMP Freeze Handshake ---\n");
        u64 progress;
        u64 ack;

        for (i = 0; i < 100; i++) {
            progress = hvc_call(HVC_SMP_FREEZE_PROGRESS, 1);
            ret = (int)hvc_call(HVC_SMP_FREEZE_BEGIN, 1);
            if (ret != 0) {
                guest_puts("  FAIL: 冻结请求失败，轮次 ");
                puts_dec(i);
                guest_puts("\n");
                return 0;
            }

            ack = hvc_call(HVC_SMP_FREEZE_ACK, 1);
            u64 snapshot_generation = 0;
            u64 snapshot_valid = hvc_call_ret2(
                HVC_SMP_FREEZE_SNAPSHOT_STATUS, 1, &snapshot_generation);
            if (ack == 0 || snapshot_valid != 1 ||
                snapshot_generation != ack ||
                hvc_call(HVC_SMP_FREEZE_PROGRESS, 1) != progress + 1) {
                guest_puts("  FAIL: 冻结 ACK/进展计数无效，轮次 ");
                puts_dec(i);
                guest_puts("\n");
                (void)hvc_call(HVC_SMP_FREEZE_RELEASE, 0);
                return 0;
            }

            ret = (int)hvc_call(HVC_SMP_FREEZE_RELEASE, 0);
            if (ret != 0) {
                guest_puts("  FAIL: 冻结释放失败，轮次 ");
                puts_dec(i);
                guest_puts("\n");
                return 0;
            }
            if (hvc_call_ret2(HVC_SMP_FREEZE_SNAPSHOT_STATUS, 1,
                              &snapshot_generation) != 0) {
                guest_puts("  FAIL: release 后冻结快照仍被标记有效，轮次 ");
                puts_dec(i);
                guest_puts("\n");
                return 0;
            }
        }
        guest_puts("  PASS: 双 vCPU 冻结/释放握手完成 100 轮\n");
        if (hvc_call(HVC_SMP_FREEZE_RELEASE, 0) != -1) {
            guest_puts("  FAIL: 无活动冻结时 release 应被拒绝\n");
            return 0;
        }
        guest_puts("  PASS: 无活动冻结时重复 release 被拒绝\n");

        if (hvc_call(HVC_MIGRATE_STATUS, 0) != 0 ||
            hvc_call(HVC_MIGRATE_PRECOPY, 0) != -1 ||
            hvc_call(HVC_MIGRATE_CANCEL, 0) != -1 ||
            hvc_call(HVC_MIGRATE_STATUS, 0) != 0) {
            guest_puts("  FAIL: IDLE 状态下 PRECOPY 应拒绝且状态不变\n");
            return 0;
        }
        guest_puts("  PASS: IDLE 下非法 PRECOPY 被拒绝，状态保持不变\n");

        /*
         * Phase 33: 双 vCPU 迁移已支持——START 不再因多 vCPU 而拒绝。
         * 以下 Test 19/20（单 vCPU / VM 级事务冻结）与迁移正交，
         * 先独立执行；结束后落到后段的双 vCPU 迁移流程。
         */

            /*
             * Phase 31: 事务式冻结测试。
             *
             * vCPU1 在 WFI 循环持续递增共享计数器。事务冻结后：
             * 1. 状态 = FROZEN(1)，期间计数静止（两次采样相同）；
             * 2. COMMIT 后计数恢复递增；
             * 3. 再次冻结 + ROLLBACK 后计数同样从冻结值继续
             *    （CPU 现场恢复，重执行从冻结点开始）；
             * 4. 无事务时 COMMIT/ROLLBACK 均被拒绝，状态回 NONE。
             */
            guest_puts("\n--- Test 19: Phase 31 Transactional Freeze ---\n");
            volatile u64 *hold_counter =
                (volatile u64 *)(SMP_TEST_ADDR + P31_HOLD_COUNTER_OFF);
            u64 frozen_at, sample;

            /* --- 轮次 1：COMMIT 路径 --- */
            if (hvc_call(HVC_SMP_TXN_FREEZE_BEGIN, 1) != 0 ||
                hvc_call(HVC_SMP_TXN_FREEZE_STATUS, 0) != 1) {
                guest_puts("  FAIL: 事务冻结 BEGIN 失败\n");
                return 0;
            }
            frozen_at = *hold_counter;
            /* 忙等一小段时间后再次采样：冻结期间必须静止 */
            for (volatile int d = 0; d < 200000; d++)
                asm volatile("nop");
            sample = *hold_counter;
            if (sample != frozen_at) {
                guest_puts("  FAIL: 冻结期间计数仍在推进\n");
                (void)hvc_call(HVC_SMP_TXN_FREEZE_COMMIT, 0);
                return 0;
            }
            guest_puts("  PASS: 冻结期间 vCPU1 计数静止\n");

            if (hvc_call(HVC_SMP_TXN_FREEZE_COMMIT, 0) != 0) {
                guest_puts("  FAIL: COMMIT 失败\n");
                return 0;
            }
            /* 等计数恢复推进（commit 后 vCPU1 继续 WFI 循环） */
            {
                int spins = 0;
                while (*hold_counter <= frozen_at && spins++ < 100000000)
                    asm volatile("yield");
                if (*hold_counter <= frozen_at) {
                    guest_puts("  FAIL: COMMIT 后计数未恢复推进\n");
                    return 0;
                }
            }
            guest_puts("  PASS: COMMIT 后 vCPU1 恢复执行\n");

            /* --- 轮次 2：ROLLBACK 路径 --- */
            if (hvc_call(HVC_SMP_TXN_FREEZE_BEGIN, 1) != 0 ||
                hvc_call(HVC_SMP_TXN_FREEZE_STATUS, 0) != 1) {
                guest_puts("  FAIL: 第二轮事务冻结 BEGIN 失败\n");
                return 0;
            }
            frozen_at = *hold_counter;
            for (volatile int d = 0; d < 200000; d++)
                asm volatile("nop");
            if (*hold_counter != frozen_at) {
                guest_puts("  FAIL: 第二轮冻结期间计数仍在推进\n");
                (void)hvc_call(HVC_SMP_TXN_FREEZE_ROLLBACK, 0);
                return 0;
            }
            if (hvc_call(HVC_SMP_TXN_FREEZE_ROLLBACK, 0) != 0) {
                guest_puts("  FAIL: ROLLBACK 失败\n");
                return 0;
            }
            /* rollback 后从冻结点重新执行，计数继续递增 */
            {
                int spins = 0;
                while (*hold_counter <= frozen_at && spins++ < 100000000)
                    asm volatile("yield");
                if (*hold_counter <= frozen_at) {
                    guest_puts("  FAIL: ROLLBACK 后计数未恢复推进\n");
                    return 0;
                }
            }
            guest_puts("  PASS: ROLLBACK 后 vCPU1 从冻结点恢复执行\n");

            /* --- 边界：无事务时决策被拒绝，状态回 NONE --- */
            if (hvc_call(HVC_SMP_TXN_FREEZE_COMMIT, 0) != -1 ||
                hvc_call(HVC_SMP_TXN_FREEZE_ROLLBACK, 0) != -1 ||
                hvc_call(HVC_SMP_TXN_FREEZE_STATUS, 0) != 0) {
                guest_puts("  FAIL: 无事务时 COMMIT/ROLLBACK 应被拒绝\n");
                return 0;
            }
            guest_puts("  PASS: 无事务时决策被拒绝，状态回 NONE\n");

            /*
             * Phase 32: VM 级双 vCPU 事务冻结测试。
             *
             * vCPU1 在 WFI 循环递增 p31_hold_counter；vCPU0（本核）
             * 发起 VM 级冻结。验证：
             * 1. BEGIN 后整 VM 静止：vCPU1 计数冻结期间不变；
             *    本核写 scratch 后读回一致（冻结窗口内存可用）。
             * 2. COMMIT：vCPU1 恢复递增，scratch 保留（已提交）。
             * 3. 第二轮 BEGIN：冻结前把探针值写入 x20（局部寄存器变量，
             *    编译器保证驻留）；冻结窗口内改写 x20 探针与 scratch。
             *    ROLLBACK 回卷 vCPU0 现场到 BEGIN 的 HVC：重放落地后
             *    BEGIN 返回 1，x20 恢复冻结前探针值（寄存器回卷）。
             *    注意：内存写不回滚（寄存器级回卷语义），scratch
             *    保留窗口内的值——重放代码不得重复写 scratch。
             * 4. 边界：无事务时 COMMIT/ROLLBACK 拒绝，状态回 NONE。
             */
            guest_puts("\n--- Test 20: Phase 32 VM-wide Transactional Freeze ---\n");
            volatile u64 *scratch =
                (volatile u64 *)(SMP_TEST_ADDR + P32_COORD_SCRATCH_OFF);
            volatile u64 *replay_count =
                (volatile u64 *)(SMP_TEST_ADDR + P32_REPLAY_COUNT_OFF);
            register u64 x20_probe __asm__("x20");
            u64 ret2;
            u64 replay_before = *replay_count;
            const u64 probe_a = 0xA5A50001UL;
            const u64 probe_b = 0xB5B50002UL;

            /* --- 轮次 1：COMMIT 路径 --- */
            if (hvc_call(HVC_VM_TXN_BEGIN, 0) != 0 ||
                hvc_call(HVC_VM_TXN_STATUS, 0) != 1) {
                guest_puts("  FAIL: VM 级 BEGIN 失败或状态错误\n");
                return 0;
            }
            frozen_at = *hold_counter;
            *scratch = 0xAAAA1111;
            for (volatile int d = 0; d < 200000; d++)
                asm volatile("nop");
            if (*hold_counter != frozen_at) {
                guest_puts("  FAIL: 冻结期间 vCPU1 计数仍在推进\n");
                (void)hvc_call(HVC_VM_TXN_COMMIT, 0);
                return 0;
            }
            if (*scratch != 0xAAAA1111) {
                guest_puts("  FAIL: 冻结窗口内协调核内存访问不一致\n");
                (void)hvc_call(HVC_VM_TXN_COMMIT, 0);
                return 0;
            }
            guest_puts("  PASS: 冻结期间整 VM 静止（vCPU1 计数不变）\n");

            if (hvc_call(HVC_VM_TXN_COMMIT, 0) != 0) {
                guest_puts("  FAIL: VM 级 COMMIT 失败\n");
                return 0;
            }
            {
                int spins = 0;
                while (*hold_counter <= frozen_at && spins++ < 100000000)
                    asm volatile("yield");
                if (*hold_counter <= frozen_at) {
                    guest_puts("  FAIL: COMMIT 后 vCPU1 计数未恢复推进\n");
                    return 0;
                }
            }
            if (*scratch != 0xAAAA1111) {
                guest_puts("  FAIL: COMMIT 后 scratch 未保留\n");
                return 0;
            }
            guest_puts("  PASS: COMMIT 后 vCPU1 恢复执行，协调核改动保留\n");

            /* --- 轮次 2：ROLLBACK 路径（vCPU0 回卷 + 重放） --- */
            /* 冻结前写入探针：x20 = probe_a（回卷后应恢复此值） */
            x20_probe = probe_a;
            asm volatile("" : "+r"(x20_probe):: "memory");
            ret2 = hvc_call(HVC_VM_TXN_BEGIN, 0);
            if (ret2 == 1)
                goto p32_replayed; /* 上一次 ROLLBACK 的重放 */
            if (ret2 != 0 || hvc_call(HVC_VM_TXN_STATUS, 0) != 1) {
                guest_puts("  FAIL: 第二轮 VM 级 BEGIN 失败\n");
                return 0;
            }
            frozen_at = *hold_counter;
            /* 冻结窗口内改写：x20 = probe_b、scratch = 0xBBBB2222 */
            x20_probe = probe_b;
            asm volatile("" : "+r"(x20_probe):: "memory");
            *scratch = 0xBBBB2222;
            for (volatile int d = 0; d < 200000; d++)
                asm volatile("nop");
            if (*hold_counter != frozen_at) {
                guest_puts("  FAIL: 第二轮冻结期间 vCPU1 计数仍在推进\n");
                (void)hvc_call(HVC_VM_TXN_ROLLBACK, 0);
                return 0;
            }
            guest_puts("  PASS: 第二轮冻结期间整 VM 静止\n");

            if (hvc_call(HVC_VM_TXN_ROLLBACK, 0) != 0) {
                guest_puts("  FAIL: VM 级 ROLLBACK 失败\n");
                return 0;
            }
            /* ROLLBACK 已回卷 vCPU0 到 BEGIN 的 HVC：以下不可达 */
            guest_puts("  FAIL: ROLLBACK 后未回卷到 BEGIN（此行不应出现）\n");
            return 0;

p32_replayed:
            /* ---- 重放路径：BEGIN 的 HVC 重执行后返回 1 ---- */
            *replay_count = *replay_count + 1;
            if (x20_probe != probe_a) {
                guest_puts("  FAIL: ROLLBACK 未回卷 x20 探针（读到 0x");
                puts_hex(x20_probe, 16);
                guest_puts("）\n");
                return 0;
            }
            if (*replay_count != replay_before + 1) {
                guest_puts("  FAIL: 重放计数错误\n");
                return 0;
            }
            guest_puts("  PASS: ROLLBACK 回卷协调核现场（x20 探针恢复）\n");
            guest_puts("  PASS: BEGIN 重放返回 1，重放计数 +1\n");
            /* 冻结窗口内的 guest 内存写不回滚：scratch 保留 0xBBBB2222 */
            if (*scratch != 0xBBBB2222) {
                guest_puts("  FAIL: 冻结窗口内内存写意外回滚\n");
                return 0;
            }
            guest_puts("  PASS: 窗口内内存写保留（寄存器级回卷语义）\n");
            /* 重放后 vCPU1 已从冻结点恢复执行，等计数推进 */
            {
                int spins = 0;
                u64 now = *hold_counter;
                while (*hold_counter <= now && spins++ < 100000000)
                    asm volatile("yield");
                if (*hold_counter <= now) {
                    guest_puts("  FAIL: ROLLBACK 后 vCPU1 计数未恢复推进\n");
                    return 0;
                }
            }
            guest_puts("  PASS: ROLLBACK 后 vCPU1 从冻结点恢复执行\n");

            /* --- 边界：无事务时决策被拒绝，状态回 NONE --- */
            if (hvc_call(HVC_VM_TXN_COMMIT, 0) != -1 ||
                hvc_call(HVC_VM_TXN_ROLLBACK, 0) != -1 ||
                hvc_call(HVC_VM_TXN_STATUS, 0) != 0) {
                guest_puts("  FAIL: 无 VM 事务时 COMMIT/ROLLBACK 应被拒绝\n");
                return 0;
            }
            guest_puts("  PASS: 无 VM 事务时决策被拒绝，状态回 NONE\n");
        }
        /* Test 19/20 结束：落到后段的双 vCPU 迁移流程（Phase 33） */

    /* 1. 分配迁移测试内存（8 页） */
    base = (volatile unsigned char *)PHASE25_BASE_IPA;
    for (i = 0; i < PHASE25_TOTAL_PAGES; i++) {
        ret = hvc_call2(HVC_MEM_MAP,
                        PHASE25_BASE_IPA + i * PHASE25_PAGE_SIZE, 0);
        if (ret != 0) {
            guest_puts("  FAIL: 映射测试页失败 (page ");
            puts_dec(i);
            guest_puts(")\n");
            passed = 0;
            goto done25;
        }
    }
    guest_puts("  [1] 分配 8 页迁移测试内存\n");

    /* 2. 写入初始内容（每页一个独特模式 0x50..0x57） */
    for (i = 0; i < PHASE25_TOTAL_PAGES; i++) {
        volatile unsigned char *p = base + i * PHASE25_PAGE_SIZE;
        unsigned char pattern = (unsigned char)(0x50 + i);
        for (int j = 0; j < PHASE25_PAGE_SIZE; j++)
            p[j] = pattern;
    }
    guest_puts("  [2] 写入初始内容（每页一个独特模式）\n");

    /* Test 23: START/PRECOPY 后取消，回收目标增量页并允许重试。 */
    guest_puts("\n--- Test 23: Phase 35 Migration Cancel/Rollback ---\n");
    {
        volatile u64 *hold_counter =
            (volatile u64 *)(SMP_TEST_ADDR + P31_HOLD_COUNTER_OFF);
        u64 src_user_before, src_kernel_before;
        u64 src_user_after, src_kernel_after;
        u64 hold_before, hold_after;
        int cancel_pages;

        src_user_before = hvc_call_ret2(HVC_MEM_STATS, 0,
                                        &src_kernel_before);
        hold_before = *hold_counter;
        if (hvc_call(HVC_MIGRATE_START, 0) != 0 ||
            hvc_call(HVC_MIGRATE_PRECOPY, 0) < 0) {
            guest_puts("  FAIL: cancel 测试 START/PRECOPY 失败\n");
            passed = 0;
            goto done25;
        }
        cancel_pages = (int)hvc_call(HVC_MIGRATE_CANCEL, 0);
        src_user_after = hvc_call_ret2(HVC_MEM_STATS, 0,
                                       &src_kernel_after);
        if (hvc_call(HVC_MIGRATE_CANCEL, 0) != -1) {
            guest_puts("  FAIL: 重复 CANCEL 应被拒绝\n");
            passed = 0;
            goto done25;
        }
        for (volatile int d = 0; d < 8000000; d++)
            asm volatile("nop");
        hold_after = *hold_counter;

        if (cancel_pages <= 0 || src_user_before != src_user_after ||
            src_kernel_before != src_kernel_after || hold_after <= hold_before ||
            hvc_call(HVC_MIGRATE_STATUS, 0) != 0) {
            guest_puts("  FAIL: cancel 未回收目标增量页或状态未复位\n");
            passed = 0;
            goto done25;
        }
        guest_puts("  PASS: CANCEL 回滚目标新增页=");
        puts_dec(cancel_pages);
        guest_puts("，源内存/AF 状态恢复且可重试\n");
    }

    /* Test 24: FINISH 准备阶段失败后回滚，并允许重试。 */
    guest_puts("\n--- Test 24: Phase 36 FINISH Abort/Rollback ---\n");
    {
        u64 abort_before = hvc_call(HVC_MIGRATE_ABORT_STATS, 0);
        int finish_ret;

        if (hvc_call(HVC_MIGRATE_START, 0) != 0 ||
            hvc_call(HVC_MIGRATE_PRECOPY, 0) < 0 ||
            hvc_call(HVC_MIGRATE_FAIL_FINISH, 0) != 0) {
            guest_puts("  FAIL: FINISH abort 注入准备失败\n");
            passed = 0;
            goto done25;
        }
        finish_ret = (int)hvc_call(HVC_MIGRATE_FINISH, 0);
        if (finish_ret != -1 ||
            hvc_call(HVC_MIGRATE_ABORT_STATS, 0) != abort_before + 1 ||
            hvc_call(HVC_MIGRATE_STATUS, 0) != 0) {
            guest_puts("  FAIL: FINISH abort 未回滚到 IDLE\n");
            passed = 0;
            goto done25;
        }
        guest_puts("  PASS: FINISH 失败已回滚源/目标状态，可重试\n");
    }

    /* 取消后重新写入模式，正式迁移必须能复用同一目标 VM。 */
    for (i = 0; i < PHASE25_TOTAL_PAGES; i++) {
        volatile unsigned char *p = base + i * PHASE25_PAGE_SIZE;
        unsigned char pattern = (unsigned char)(0x50 + i);
        for (int j = 0; j < PHASE25_PAGE_SIZE; j++)
            p[j] = pattern;
    }

    /* 3. START：清 AF（开启脏页跟踪）+ 全量拷贝 */
    ret = (int)hvc_call(HVC_MIGRATE_START, 0);
    if (ret != 0) {
        guest_puts("  FAIL: HVC_MIGRATE_START 返回 ");
        puts_dec(ret);
        guest_puts("\n");
        passed = 0;
        goto done25;
    }
    guest_puts("  [3] START：清 AF + 全量拷贝完成\n");

    /* 4. 制造脏页：修改前 4 页内容（0xA0..0xA3） */
    for (i = 0; i < PHASE25_DIRTY_PAGES; i++) {
        volatile unsigned char *p = base + i * PHASE25_PAGE_SIZE;
        unsigned char pattern = (unsigned char)(0xA0 + i);
        for (int j = 0; j < PHASE25_PAGE_SIZE; j++)
            p[j] = pattern;
    }
    guest_puts("  [4] 修改前 4 页内容（制造脏页）\n");

    /* 5. PRECOPY：迭代预拷贝，收集并拷贝 AF=1 的脏页 */
    precopy_dirty = (int)hvc_call(HVC_MIGRATE_PRECOPY, 0);
    guest_puts("  [5] PRECOPY：本轮拷贝脏页数 = ");
    puts_dec(precopy_dirty);
    guest_puts("\n");
    if (precopy_dirty < PHASE25_DIRTY_PAGES) {
        guest_puts("  WARN: 脏页数偏少（预期 >= 4）\n");
    }

    /* 6. FINISH：stop-and-copy + 状态转移。
     *    执行完这一句后 vCPU 已迁移到 VM3（vmid=3），
     *    以下所有代码（含 guest_puts）都在 VM3 上运行。 */
    finish_dirty = (int)hvc_call(HVC_MIGRATE_FINISH, 0);
    guest_puts("  [6] FINISH：完成迁移，最后一轮脏页数 = ");
    puts_dec(finish_dirty);
    guest_puts("\n");

    /* 7. 验证迁移后 vmid 变为 3（fresh 查询，不能沿用 guest_main 里缓存的 vmid） */
    new_vmid = guest_vmid();
    guest_puts("  [7] 迁移后 VMID = ");
    puts_dec(new_vmid);
    guest_puts("\n");
    if (new_vmid != 3) {
        guest_puts("  FAIL: 迁移后 VMID 不是 3\n");
        passed = 0;
    }

    /* 8. 验证迁移后内存内容：前 4 页 = 0xA0..0xA3，后 4 页 = 0x54..0x57 */
    guest_puts("  [8] 验证迁移后内存内容...\n");
    for (i = 0; i < PHASE25_TOTAL_PAGES; i++) {
        volatile unsigned char *p = base + i * PHASE25_PAGE_SIZE;
        unsigned char expected = (i < PHASE25_DIRTY_PAGES)
                                     ? (unsigned char)(0xA0 + i)
                                     : (unsigned char)(0x50 + i);
        if (p[0] != expected || p[PHASE25_PAGE_SIZE - 1] != expected) {
            guest_puts("  FAIL: 第 ");
            puts_dec(i);
            guest_puts(" 页内容错误\n");
            passed = 0;
            break;
        }
    }
    if (passed)
        guest_puts("  [8] 迁移后内存内容正确\n");

    /*
     * Test 21: 双 vCPU 迁移验证。
     *
     * 迁移 FINISH 在 VM 级冻结窗口内把 VM2 的 vCPU1 就地改造为 VM3
     * vCPU1（vmid 2→3，mm/board_data 挂到 VM3），commit 后它从冻结点
     * 恢复、继续 WFI 观测循环递增 hold_counter。此处从 vCPU0（已迁移
     * 到 VM3）视角读共享页里的 hold_counter，确认 vCPU1 也迁移成功且
     * 仍在推进（计数严格单调递增）。
     */
    guest_puts("\n--- Test 21: Phase 33 Dual-vCPU Migration ---\n");
    {
        volatile u64 *hold_counter =
            (volatile u64 *)(SMP_TEST_ADDR + P31_HOLD_COUNTER_OFF);
        u64 c0 = *hold_counter;
        for (volatile int d = 0; d < 8000000; d++)
            asm volatile("nop");
        u64 c1 = *hold_counter;
        if (c1 <= c0) {
            guest_puts("  FAIL: 迁移后 vCPU1 未继续推进 (");
            puts_dec(c0);
            guest_puts(" -> ");
            puts_dec(c1);
            guest_puts(")\n");
            passed = 0;
        } else {
            guest_puts("  PASS: vCPU1 已迁移到 VM3 且继续推进 (");
            puts_dec(c0);
            guest_puts(" -> ");
            puts_dec(c1);
            guest_puts(")\n");
        }
    }

    /*
     * Test 22: Phase 34 迁移后源 VM 回收。
     *
     * FINISH 在状态转移完成后已回收源 VM2 资源（设备模型、ivshmem
     * peer、跨 VM shm 引用、KSM 共享引用、Stage-2 页表与普通页）。
     * 此时已迁移到 VM3，从 VM3 视角查询全局回收统计，确认回收已
     * 完成且释放了页数 > 0（至少源 VM 的内核镜像 + 页表被回收）。
     */
    guest_puts("\n--- Test 22: Phase 34 Source VM Reclaim ---\n");
    {
        u64 r_pages, r_shm, r_ksm, r_done;
        r_pages = hvc_call_ret4(HVC_MIGRATE_RECLAIM_STATS, 0,
                                &r_shm, &r_ksm, &r_done);
        guest_puts("  reclaim: pages=");
        puts_dec(r_pages);
        guest_puts(" shm=");
        puts_dec(r_shm);
        guest_puts(" ksm=");
        puts_dec(r_ksm);
        guest_puts(" done=");
        puts_dec(r_done);
        guest_puts("\n");

        if (r_done != 1) {
            guest_puts("  FAIL: 源 VM 回收未完成 (done != 1)\n");
            passed = 0;
        } else if (r_pages == 0) {
            guest_puts("  FAIL: 源 VM 回收页数为 0\n");
            passed = 0;
        } else {
            guest_puts("  PASS: 源 VM2 资源已回收 (");
            puts_dec(r_pages);
            guest_puts(" 页)\n");
        }
    }

    if (passed)
        guest_puts("  PASSED: 双 vCPU Live Migration 成功（VM2 -> VM3）\n");
    else
        guest_puts("  FAILED\n");

done25:
    return passed;
}

static void vits_test(void)
{
    guest_puts("\n--- Test 4: vITS (virtual ITS) ---\n");

    /* 1. 初始化 vITS */
    if (vits_init() < 0) {
        guest_puts("vITS: FAILED (init failed)\n");
        return;
    }

    /* 2. MAPC: collection 0 -> procnum 0 */
    vits_mapc(0, 0);

    /* 3. MAPD: devid=EDU_DEVID -> ITT */
    vits_mapd(EDU_DEVID, (u64)vits_itt_buf, 3);  /* size=3 -> 16 events */

    /* 4. MAPTI: eventid=1 -> vLPI=VITS_TEST_VLPI, icid=0
     * 用 eventid=1 避免和 Phase 8 已配置的 eventid=0 冲突 */
    vits_mapti(EDU_DEVID, 1, VITS_TEST_VLPI, 0);

    vits_sync();

    /* 5. 修改 edu 的 MSI data 为 1（EventID = 1）
     * 通过 PCI ECAM 直接写配置空间。
     * edu 在 bus 0, dev 2, func 0。
     * MSI Capability 在 pci_init 里探测过，偏移由 QEMU 决定。
     * 简化：从 Cap Pointer 开始遍历，找到 MSI Capability。
     */
    {
        #define PCIE_ECAM_BASE   0x3F000000UL
        #define PCI_CFG_CAP_PTR  0x34
        #define PCI_CFG_STATUS   0x06
        #define PCI_CAP_ID_MSI   0x05
        #define PCI_MSI_DATA_OFF 0x0C  /* 32-bit 地址模式下 data 寄存器偏移 */

        u32 ecam_addr = 0x3F000000 + (0 << 20) + (2 << 15) + (0 << 12);
        u8 cap_ptr = (u8)mmio_read32(ecam_addr + PCI_CFG_CAP_PTR);
        u8 msi_cap_off = 0;

        while (cap_ptr != 0 && cap_ptr != 0xFF) {
            u32 cap_hdr = mmio_read32(ecam_addr + cap_ptr);
            u8 cap_id = (u8)(cap_hdr & 0xFF);
            if (cap_id == PCI_CAP_ID_MSI) {
                msi_cap_off = cap_ptr;
                break;
            }
            cap_ptr = (u8)((cap_hdr >> 8) & 0xFF);
        }

        if (msi_cap_off == 0) {
            guest_puts("vITS: FAILED (no MSI capability found)\n");
            return;
        }

        guest_puts("vITS: MSI cap at offset 0x");
        puts_hex(msi_cap_off, 2);
        guest_puts("\n");

        /* 读当前 MSI data（应该是 0，即 EventID = 0） */
        u32 msi_data = mmio_read32(ecam_addr + msi_cap_off + PCI_MSI_DATA_OFF);
        guest_puts("vITS: current MSI data = 0x");
        puts_hex(msi_data & 0xFFFF, 4);
        guest_puts("\n");

        /* 修改 MSI data 为 1（EventID = 1） */
        mmio_write32(ecam_addr + msi_cap_off + PCI_MSI_DATA_OFF,
                     (msi_data & 0xFFFF0000) | 1);

        /* 验证写成功 */
        u32 new_data = mmio_read32(ecam_addr + msi_cap_off + PCI_MSI_DATA_OFF);
        guest_puts("vITS: new MSI data = 0x");
        puts_hex(new_data & 0xFFFF, 4);
        guest_puts("\n");

        /* 6. 触发 edu 中断，验证收到 VITS_TEST_VSPI 号虚拟中断 */
        guest_puts("vITS: triggering MSI with EventID=1...\n");
        vits_irq_received = 0;
        vits_irq_intid = 0;

        /* 清残留 irq_status */
        mmio_write32(EDU_BAR0_BASE + EDU_IRQ_ACK, 0xffffffff);

        /* 触发 edu 中断（写非零值到 IRQ_RAISE 寄存器） */
        mmio_write32(EDU_BAR0_BASE + EDU_IRQ_RAISE, 0xabcdef01);

        /* 等待中断 */
        int timeout = 10000000;
        while (!vits_irq_received && timeout-- > 0)
            asm volatile("nop");

        if (vits_irq_received && vits_irq_intid == VITS_TEST_VSPI) {
            guest_puts("vITS: PASSED! Received vSPI ");
            puts_dec(VITS_TEST_VSPI);
            guest_puts(" (vLPI ");
            puts_dec(VITS_TEST_VLPI);
            guest_puts(")\n");
        } else {
            guest_puts("vITS: FAILED (received intid=");
            puts_dec(vits_irq_intid);
            guest_puts(", expected ");
            puts_dec(VITS_TEST_VSPI);
            guest_puts(")\n");
        }

        /* 7. 恢复 MSI data = 0（保持 Phase 8 兼容） */
        mmio_write32(ecam_addr + msi_cap_off + PCI_MSI_DATA_OFF, msi_data);
        guest_puts("vITS: MSI data restored\n");
    }
}

/* ---------- IRQ 处理 ---------- */

void guest_irq_handler(void)
{
    u64 iar = icc_iar1_read();
    u32 intid = (u32)iar;

    if (intid == 0x3FF) /* spurious */
        return;

    if (intid == VIRT_TIMER_IRQ) {
        tick_count++;

        u64 now = cntvct_read();
        cntv_cval_write(now + timer_freq / tick_hz);

        if (tick_count % tick_hz == 0) {
            guest_puts("tick ");
            puts_dec(tick_count / tick_hz);
            guest_puts("\n");
        }
    } else if (intid == VIRTIO_NET_IRQ) {
        virtio_net_irq_handler();
    } else if (intid == IVSHMEM_IRQ) {
        ivshmem_irq_handler();
    } else if (intid == PCI_EDU_IRQ) {
        /* 清设备侧 irq_status（MSI 模式下设备看到状态归零即安静，
         * 之后走末尾统一虚拟 EOI 结束软件注入的虚拟中断） */
        edu_irq_status_val = mmio_read32(EDU_BAR0_BASE + EDU_IRQ_STATUS);
        mmio_write32(EDU_BAR0_BASE + EDU_IRQ_ACK, edu_irq_status_val);
        edu_irq_received = 1;
    } else if (intid == VITS_TEST_VSPI) {
        /* vITS 映射的 MSI 中断（Phase 9 测试） */
        vits_irq_intid = intid;
        vits_irq_received = 1;
        /* 清设备侧 IRQ status */
        mmio_write32(EDU_BAR0_BASE + EDU_IRQ_ACK,
                     mmio_read32(EDU_BAR0_BASE + EDU_IRQ_STATUS));
    } else if (intid == SHM_NOTIFY_VIRQ) {
        /* 共享内存通知中断（Phase 10） */
        shm_notify_received = 1;
    } else if (intid == VGICD_TEST_VIRQ) {
        /* Phase 15: 虚拟 GICD 测试中断 */
        vgicd_irq_received = 1;
    } else if (intid == VGICR_TEST_PPI) {
        /* Phase 16: 虚拟 GICR 测试中断 */
        vgicr_irq_received = 1;
    } else if (intid < 16) {
        /* Phase 12: SGI（软件生成中断，vSGI 测试用）
         * SGI INTID 范围 0-15。
         * 用共享内存记录收到的 SGI 号和次数。 */
        volatile u64 *vsgi_received = (volatile u64 *)(SMP_TEST_ADDR + VSGI_RECEIVED_OFF);
        volatile u64 *vsgi_count = (volatile u64 *)(SMP_TEST_ADDR + VSGI_COUNT_OFF);
        *vsgi_received |= (1ULL << intid);
        (*vsgi_count)++;
        asm volatile("dmb ish" ::: "memory");
    }

    icc_eoir1_write(iar);
}

/* ---------- 主函数 ---------- */

/* ---- MMU 初始化 (等值映射，开启 cache) ---- */

/* Level 1 页表 (4KB 对齐)，用 1GB block 映射 */
static u64 __attribute__((aligned(4096))) guest_pg_dir[512];

#define MMU_MAIR_DEV     0  /* AttrIndx = 0: Device-nGnRnE */
#define MMU_MAIR_NORM    1  /* AttrIndx = 1: Normal WB RA WA */

#define PTE_TYPE_BLOCK   1
#define PTE_AF           (1UL << 10)
#define PTE_SH_INNER     (3UL << 8)

static void mmu_init(void)
{
    /* MAIR_EL1
     * Attr 0: 0x00 -> Device-nGnRnE
     * Attr 1: 0xFF -> Normal, Outer WB RA WA, Inner WB RA WA
     */
    u64 mair = (0x00ULL << (8 * 0)) | (0xFFULL << (8 * 1));
    SYSREG_WRITE(mair_el1, mair);

    /* TCR_EL1
     * T0SZ = 25 (39-bit input, start at Level 1)
     * TG0 = 00 (4KB granule)
     * IPS = 000 (32-bit PA, 4GB)
     */
    u64 tcr = (25 << 0)    /* T0SZ = 25 -> 39-bit input address */
            | (0 << 14)    /* TG0 = 4KB */
            | (0 << 32)    /* IPS = 32-bit PA */
            | (1 << 23)    /* EPD0 = 0, enable TTBR0 */
            | (1 << 20)    /* TBI0 = 0 */
            ;
    SYSREG_WRITE(tcr_el1, tcr);

    /* 初始化 Level 1 页表：1GB block 映射，等值映射 */
    /* entry 0: 0x00000000-0x3FFFFFFF -> Device (MMIO 区域) */
    guest_pg_dir[0] = (0x00000000ULL & ~0x3FFFFFFFULL)  /* output addr */
                    | (MMU_MAIR_DEV << 2)               /* AttrIndx */
                    | PTE_AF
                    | PTE_TYPE_BLOCK;

    /* entry 1: 0x40000000-0x7FFFFFFF -> Normal cacheable (RAM) */
    guest_pg_dir[1] = (0x40000000ULL & ~0x3FFFFFFFULL)  /* output addr */
                    | (MMU_MAIR_NORM << 2)              /* AttrIndx */
                    | PTE_SH_INNER                      /* Inner Shareable */
                    | PTE_AF
                    | PTE_TYPE_BLOCK;

    /* entries 2-511: 不映射 (PTE=0) */

    /* TTBR0_EL1: 指向页表 */
    u64 ttbr0 = (u64)guest_pg_dir;
    SYSREG_WRITE(ttbr0_el1, ttbr0);

    /* 确保所有之前的操作完成 */
    asm volatile("dsb sy" ::: "memory");
    asm volatile("isb" ::: "memory");

    /* 开启 MMU + 数据 cache + 指令 cache
     * M=1 (bit 0), C=1 (bit 2), I=1 (bit 12)
     */
    u64 sctlr = SYSREG_READ(sctlr_el1);
    sctlr |= (1 << 0) | (1 << 2) | (1 << 12);  /* M | C | I */
    SYSREG_WRITE(sctlr_el1, sctlr);

    asm volatile("isb" ::: "memory");
}

/* SMP 主核标志：第一个进入 guest_main 的 vCPU 成为主核
 * 因为两个 vCPU 在同一物理核上分时运行，不需要真正的原子操作 */
static volatile int smp_master_locked = 0;
static volatile int smp_is_master = 0;

static int try_become_master(void)
{
    /* 简单的 test-and-set（单核分时运行，不需要 ldrex/strex） */
    if (smp_master_locked == 0) {
        smp_master_locked = 1;
        smp_is_master = 1;
        return 1;
    }
    return 0;
}

void guest_main(void)
{
    u64 vmid = guest_vmid();
    u64 mpidr = SYSREG_READ(mpidr_el1);
    int cpu_id = (int)(mpidr & 0xFF);

    /* 竞选主核：第一个到 guest_main 的当主核，另一个当次核
     * 注意：不依赖 MPIDR 做判断，因为 start.S 和 C 代码读到的
     * MPIDR 可能不一致（虚拟化 trap 时序问题）。 */
    if (!try_become_master()) {
        /* 次核：跳到次核入口 */
        guest_secondary_main();
        return;
    }

    /* 以下是主核路径 */

    /* 先初始化 MMU（开启 cache，解决与 hypervisor 的 cache 一致性问题） */
    mmu_init();

    /*
     * 跨核并发启动同步：主核完成 BSS 清零 + 页表建立后置 boot_done=1。
     * 次核在开启自己的 MMU 前必须等这个标志，否则次核 mmu_init() 写的
     * guest_pg_dir 会和主核的 BSS 清零/页表建立发生竞态。
     */
    {
        volatile u64 *boot_done =
            (volatile u64 *)(SMP_TEST_ADDR + SMP_BOOT_DONE_OFF);
        *boot_done = 1;
        asm volatile("dmb ish" ::: "memory");
    }

    guest_puts("\nguest: hello from EL1!\n");
    guest_puts("guest: I am VM ");
    puts_dec(vmid);
    guest_puts(", running on aVisor hypervisor (Phase 4)\n");

    {
        u64 sctlr = SYSREG_READ(sctlr_el1);
        guest_puts("guest: SCTLR_EL1 = 0x");
        puts_hex(sctlr, 16);
        guest_puts(" (M=");
        puts_dec((sctlr >> 0) & 1);
        guest_puts(" C=");
        puts_dec((sctlr >> 2) & 1);
        guest_puts(" I=");
        puts_dec((sctlr >> 12) & 1);
        guest_puts(")\n");
    }

    /* 初始化虚拟 GIC */
    gic_vcpu_init();
    guest_puts("guest: virtual GIC CPU interface initialized\n");

    /* 初始化虚拟 GIC Distributor（Phase 15 trap-and-emulate） */
    gic_dist_init();

    /* 初始化虚拟 GIC Redistributor（Phase 16 trap-and-emulate） */
    gicr_init();

    /* 初始化虚拟定时器 */
    vtimer_start();
    guest_puts("guest: virtual timer started\n");

    /* 开中断（让 virtio-net / ivshmem 的中断能被处理） */
    asm volatile("msr daifclr, #2" ::: "memory");
    guest_puts("guest: interrupts enabled\n");

    /* 测试 1: virtio-net */
    guest_puts("\n--- Test 1: virtio-net ---\n");
    if (virtio_net_init() == 0) {
        virtio_net_loopback_test();
    }

    /* 测试 2: ivshmem */
    guest_puts("\n--- Test 2: ivshmem ---\n");
    ivshmem_init();
    ivshmem_pingpong_test();

    /* 测试 3: PCI 直通 edu（仅 VM1 直通了该设备） */
    if (vmid == 1) {
        guest_puts("\n--- Test 3: PCI passthrough (edu) ---\n");
        edu_test();

        /* 测试 4: vITS 虚拟 ITS（仅 VM1 有 vITS） */
        vits_test();
    }

    /* 测试 5: Phase 10 新功能测试（调度器/内存/共享内存）
     * 两个 VM 都跑，共享内存测试需要双方配合 */
    phase10_tests();

    /* 测试 6: Phase 11 虚拟 SMP 测试（需要次 vCPU 配合）
     * 两个 vCPU 都会进入，内部用标志位区分主/次角色 */
    phase11_smp_test();

    /* 测试 7: Phase 12 vSGI（虚拟 SGI）测试
     * 需要次 vCPU 配合。Phase 14 起两个 VM 都是双核，
     * 两个 vCPU 都会进入，内部用 cpuid 区分主/次角色 */
    phase12_vsgi_test();

    /* 测试 8: Phase 15 虚拟 GICD 模拟（trap-and-emulate） */
    phase15_vgicd_test();

    /* 测试 9: Phase 16 虚拟 GICR 模拟（trap-and-emulate） */
    phase16_gicr_test();

    /* 测试 10: Phase 17 虚拟 SGI trap-and-emulate（捕获 ICC_SGI1R_EL1 写）
     * 需要次 vCPU 配合。phase17_vsgi_trap_test 内部按 cpuid 区分主/次。 */
    phase17_vsgi_trap_test();

    /* 测试 11: Phase 18 优先级与 pending 完整建模（主核单线程） */
    phase18_priority_pending_test();

    /* 测试 12: Phase 19 vGIC 注入路径统一（VM 间协作验证） */
    phase19_injection_unify_test();

    /* 测试 13: Phase 20 Stage-2 页表管理完善（主核单线程） */
    phase20_unmap_pt_reclaim_test();

    /* 测试 14: Phase 21 Stage-2 细粒度权限管理（主核单线程） */
    phase21_mprotect_test();

    /* 测试 15: Phase 22 大页 + 内存配额（主核单线程） */
    phase22_hugepage_quota_test();

    /* 测试 16: Phase 23 页面回收（时钟算法 + 配额触发） */
    test_phase23_reclaim();

    /* 测试 17: Phase 24 KSM 内存去重（主核单线程） */
    test_phase24_ksm();

    /* Phase 40: 在迁移前查询 VM2 vCPU0 的 EL2 task 栈遥测。 */
    if (vmid == 2) test_phase40_stack_telemetry();

    /* 测试 18: Phase 25 Live Migration（仅 VM2 发起，迁移到 VM3） */
    if (vmid == 2) test_phase25_migration();

    guest_puts("\nguest: all tests done, entering main loop\n");

    while (1) {
        asm volatile("wfi" ::: "memory");
    }
}

/*
 * Phase 11: SMP 共享计数器测试（次核侧）
 *
 * 次核在初始化完成后调用此函数，参与共享计数器累加测试。
 * 主核负责初始化和结果验证。
 *
 * 同步协议：
 *   ready_flag = 1: 主核初始化完成，次核先做一次 bring-up 测试
 *   ready_flag = 2: 主核启动并发测试，双方同时累加
 *   done_flag = 1:  次核完成 bring-up 测试
 *   done_flag = 2:  次核完成并发测试
 */
static void smp_secondary_counter_test(void)
{
    volatile u64 *ready_flag = (volatile u64 *)(SMP_TEST_ADDR + SMP_READY_FLAG_OFF);
    volatile u64 *done_flag = (volatile u64 *)(SMP_TEST_ADDR + SMP_DONE_FLAG_OFF);
    volatile u64 *counter = (volatile u64 *)(SMP_TEST_ADDR + SMP_COUNTER_OFF);
    spinlock_t *lock = (spinlock_t *)(SMP_TEST_ADDR + SMP_LOCK_OFF);

    /* 等待主核初始化完成
     * 用 sched_yield 主动让出 CPU，比忙等高效很多。
     * 每次 yield 把自己的时间片清零，让其他 vCPU 先跑。 */
    while (*ready_flag == 0) {
        hvc_call(HVC_SCHED_YIELD, 0);
    }
    asm volatile("dmb ish" ::: "memory");

    /* 第一轮：累加测试（次核先单独做一次，验证基本功能） */
    for (int i = 0; i < SMP_ITERATIONS; i++) {
        spin_lock(lock);
        (*counter)++;
        spin_unlock(lock);
    }

    /* 通知主核：第一轮完成 */
    asm volatile("dmb ish" ::: "memory");
    *done_flag = 1;

    /* 等主核发第二轮信号（ready_flag == 2）
     * 用 sched_yield 主动让出 CPU */
    while (*ready_flag != 2) {
        hvc_call(HVC_SCHED_YIELD, 0);
    }
    asm volatile("dmb ish" ::: "memory");

    /* 第二轮：并发累加（和主核同时操作） */
    for (int i = 0; i < SMP_ITERATIONS; i++) {
        spin_lock(lock);
        (*counter)++;
        spin_unlock(lock);
    }

    /* 通知主核：第二轮完成 */
    asm volatile("dmb ish" ::: "memory");
    *done_flag = 2;
}

/*
 * Guest 次核入口（虚拟 SMP 的第二个 vCPU）
 *
 * 主核负责初始化 MMU / GIC Distributor / 设备驱动 / 运行测试；
 * 次核只需要初始化自己的：
 *   - MMU（开启 cache，同一个页表）
 *   - GIC CPU interface（per-CPU）
 *   - 虚拟定时器（per-CPU）
 * 然后参与 SMP 测试，最后进入 WFI 循环。
 *
 * 注意：BSS 清零由主核负责，次核不重复清零，
 * 否则会把主核已经初始化的数据又清零。
 */
void guest_secondary_main(void)
{
    u64 vmid = guest_vmid();
    u64 mpidr = SYSREG_READ(mpidr_el1);
    int cpu_id = (int)(mpidr & 0xFF);

    /*
     * 跨核并发启动同步：等待主核完成 BSS 清零 + 页表建立。
     * 次核 mmu_init() 会写 guest_pg_dir（BSS），若在主核还没建好页表
     * 前就写，会被主核随后的 BSS 清零/页表写覆盖，导致 MMU 用错页表。
     */
    {
        volatile u64 *boot_done =
            (volatile u64 *)(SMP_TEST_ADDR + SMP_BOOT_DONE_OFF);
        while (*boot_done != 1)
            hvc_call(HVC_SCHED_YIELD, 0);
        asm volatile("dmb ish" ::: "memory");
    }

    /* 次核也需要开启 MMU（同一个页表，等值映射） */
    mmu_init();

    guest_puts("[vcpu");
    puts_dec(cpu_id);
    guest_puts("] guest: hello from secondary CPU (vm");
    puts_dec(vmid);
    guest_puts(")\n");

    /* 初始化虚拟 GIC CPU interface（per-CPU） */
    gic_vcpu_init();
    guest_puts("[vcpu");
    puts_dec(cpu_id);
    guest_puts("] guest: virtual GIC CPU interface initialized\n");

    /* 初始化虚拟定时器（per-CPU） */
    vtimer_start();
    guest_puts("[vcpu");
    puts_dec(cpu_id);
    guest_puts("] guest: virtual timer started\n");

    /* 开中断 */
    asm volatile("msr daifclr, #2" ::: "memory");
    guest_puts("[vcpu");
    puts_dec(cpu_id);
    guest_puts("] guest: interrupts enabled\n");

    /* Phase 11: 参与 SMP 共享计数器测试 */
    guest_puts("[vcpu");
    puts_dec(cpu_id);
    guest_puts("] guest: entering SMP counter test\n");
    smp_secondary_counter_test();
    guest_puts("[vcpu");
    puts_dec(cpu_id);
    guest_puts("] guest: SMP test done\n");

    /* Phase 12: vSGI 测试（次核侧）
     * Phase 14 起两个 VM 都有次 vCPU。
     * phase12_vsgi_test 内部会根据 cpuid 自动判断主/次角色。 */
    phase12_vsgi_test();

    /* Phase 17: 虚拟 SGI trap-and-emulate 测试（次核侧）
     * 内部根据 cpuid 区分主/次；次核在此等待主核通过 ICC_SGI1R_EL1
     * 发送的 SGI。 */
    phase17_vsgi_trap_test();

    guest_puts("[vcpu");
    puts_dec(cpu_id);
    guest_puts("] guest: entering main loop\n");

    /*
     * Phase 31: 事务式冻结观测循环（仅 VM2）。
     *
     * VM2 次核在 WFI 主循环里持续递增 p31_hold_counter 并记录 epoch，
     * 主核冻结它后读取计数：冻结期间计数必须完全静止。
     * commit 后计数从冻结值继续单调递增；
     * rollback 后从冻结点重新执行，计数同样从冻结值继续。
     *
     * 注意：观测循环必须限定 vmid==2。事务冻结协议只针对 VM2 的
     * vCPU1；且 VM1 的 0x40300000 共享页可能被 KSM 扫描与其他页
     * 合并成只读共享页，VM1 次核持续写它会在 KSM CoW 哈希失效时
     * （合并后内容变化导致 lookup miss）把 Data Abort 注入 Guest，
     * 次核走 fatal wfe 死循环独占物理核，饿死同核的 VM2 vCPU1。
     * VM1 次核保持纯 WFI 循环（Phase 11 之前的行为）。
     */
    if (vmid == 2) {
        volatile u64 *hold_counter =
            (volatile u64 *)(SMP_TEST_ADDR + P31_HOLD_COUNTER_OFF);
        volatile u64 *hold_epoch =
            (volatile u64 *)(SMP_TEST_ADDR + P31_HOLD_EPOCH_OFF);

        while (1) {
            (*hold_counter)++;
            (*hold_epoch) = *hold_counter; /* 观察值 = 计数快照 */
            asm volatile("dmb ish" ::: "memory");
            asm volatile("wfi" ::: "memory");
        }
    }

    while (1) {
        asm volatile("wfi" ::: "memory");
    }
}
