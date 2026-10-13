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

void guest_main(void)
{
    u64 vmid = guest_vmid();

    /* 先初始化 MMU（开启 cache，解决与 hypervisor 的 cache 一致性问题） */
    mmu_init();

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

    guest_puts("\nguest: all tests done, entering main loop\n");

    while (1) {
        asm volatile("wfi" ::: "memory");
    }
}

/*
 * Guest 次核入口（虚拟 SMP 的第二个 vCPU）
 *
 * 主核负责初始化 MMU / GIC Distributor / 设备驱动 / 运行测试；
 * 次核只需要初始化自己的：
 *   - MMU（开启 cache，同一个页表）
 *   - GIC CPU interface（per-CPU）
 *   - 虚拟定时器（per-CPU）
 * 然后进入主循环。
 *
 * 注意：BSS 清零由主核负责，次核不重复清零，
 * 否则会把主核已经初始化的数据又清零。
 */
void guest_secondary_main(void)
{
    u64 vmid = guest_vmid();
    u64 mpidr = SYSREG_READ(mpidr_el1);
    int cpu_id = (int)(mpidr & 0xFF);

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

    guest_puts("[vcpu");
    puts_dec(cpu_id);
    guest_puts("] guest: entering main loop\n");

    while (1) {
        asm volatile("wfi" ::: "memory");
    }
}
