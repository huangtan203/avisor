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

    /* 内存区域 (用于描述符/avail/used 环) */
    u8 *rx_mem;
    u8 *tx_mem;
};

static struct virtio_net_drv vnet;

/* 简单的内存分配器 (从栈顶往下分配) */
#define GUEST_HEAP_TOP  0x40400000UL
#define GUEST_HEAP_BASE 0x40300000UL
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

    /* 向 rx 队列填入空缓冲区 (用于接收) */
    for (int i = 0; i < VRING_SIZE; i++) {
        u8 *buf = guest_alloc(1500, 64);
        if (!buf) break;
        vnet.rx_desc[i].addr = (u64)buf;
        vnet.rx_desc[i].len = 1500;
        vnet.rx_desc[i].flags = VRING_DESC_F_WRITE;
        vnet.rx_desc[i].next = 0;
        vnet.rx_avail->ring[i] = i;
    }
    vnet.rx_avail->idx = VRING_SIZE;

    /* 7. 初始化 tx 队列 (q1) */
    if (vring_init(base, 1, VRING_SIZE,
                   &vnet.tx_desc, &vnet.tx_avail, &vnet.tx_used) < 0) {
        guest_puts("virtio-net: failed to init tx vring\n");
        return -1;
    }
    vnet.tx_last_used = 0;

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

/* 发送一个包 (简化: 单描述符) */
static int virtio_net_tx(const u8 *data, int len)
{
    if (!vnet.ready)
        return -1;

    /* 找一个可用 tx 描述符 */
    static u16 tx_desc_idx = 0;
    u16 idx = tx_desc_idx % VRING_SIZE;

    vnet.tx_desc[idx].addr = (u64)data;
    vnet.tx_desc[idx].len = len;
    vnet.tx_desc[idx].flags = 0;
    vnet.tx_desc[idx].next = 0;

    /* 提交到 avail 环 */
    u16 avail_idx = vnet.tx_avail->idx;
    vnet.tx_avail->ring[avail_idx % VRING_SIZE] = idx;
    vnet.tx_avail->idx = avail_idx + 1;

    /* 通知设备 */
    mmio_write32(vnet.base + VIRTIO_MMIO_QUEUE_NOTIFY, 1);

    tx_desc_idx++;
    return 0;
}

/* 检查 rx 是否有包 */
static int virtio_net_rx_poll(u8 *buf, int buf_len)
{
    if (!vnet.ready)
        return 0;

    if (vnet.rx_used->idx == vnet.rx_last_used)
        return 0;

    /* 取一个已用描述符 */
    struct vring_used_elem *elem =
        &vnet.rx_used->ring[vnet.rx_last_used % VRING_SIZE];
    u16 desc_idx = (u16)elem->id;
    int pkt_len = elem->len;

    if (pkt_len > buf_len)
        pkt_len = buf_len;

    /* 拷贝数据 */
    u8 *src = (u8 *)(u64)vnet.rx_desc[desc_idx].addr;
    for (int i = 0; i < pkt_len; i++)
        buf[i] = src[i];

    vnet.rx_last_used++;

    /* 归还描述符到 avail 环 */
    u16 avail_idx = vnet.rx_avail->idx;
    vnet.rx_avail->ring[avail_idx % VRING_SIZE] = desc_idx;
    vnet.rx_avail->idx = avail_idx + 1;

    return pkt_len;
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

    /* 构造测试包 */
    u8 tx_pkt[64];
    for (int i = 0; i < 64; i++)
        tx_pkt[i] = (u8)(i * 2 + 1);

    /* 发送 */
    if (virtio_net_tx(tx_pkt, 64) < 0) {
        guest_puts("virtio-net: tx failed\n");
        return;
    }

    /* 轮询接收 (因为环回, 应该马上收到) */
    u8 rx_pkt[64];
    int rx_len = 0;
    for (int i = 0; i < 1000; i++) {
        rx_len = virtio_net_rx_poll(rx_pkt, 64);
        if (rx_len > 0)
            break;
    }

    if (rx_len == 0) {
        guest_puts("virtio-net: loopback test FAILED (no rx)\n");
        return;
    }

    /* 验证数据 */
    int match = 1;
    for (int i = 0; i < 64 && i < rx_len; i++) {
        if (rx_pkt[i] != tx_pkt[i]) {
            match = 0;
            break;
        }
    }

    if (match && rx_len == 64) {
        guest_puts("virtio-net: loopback test PASSED (64 bytes)\n");
    } else {
        guest_puts("virtio-net: loopback test FAILED (len=");
        puts_dec(rx_len);
        guest_puts(", data mismatch)\n");
    }
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

        /* 等 VM2 回复 */
        int timeout = 10000000;
        while (!ivshmem_irq_received && timeout-- > 0) {
            asm volatile("nop");
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
        /* VM2 等 doorbell 从 VM1 */
        int timeout = 10000000;
        while (!ivshmem_irq_received && timeout-- > 0) {
            asm volatile("nop");
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
    }

    icc_eoir1_write(iar);
}

/* ---------- 主函数 ---------- */

void guest_main(void)
{
    u64 vmid = guest_vmid();

    guest_puts("\nguest: hello from EL1!\n");
    guest_puts("guest: I am VM ");
    puts_dec(vmid);
    guest_puts(", running on aVisor hypervisor (Phase 4)\n");

    /* 初始化虚拟 GIC */
    gic_vcpu_init();
    guest_puts("guest: virtual GIC CPU interface initialized\n");

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

    guest_puts("\nguest: all tests done, entering main loop\n");

    while (1) {
        asm volatile("wfi" ::: "memory");
    }
}
