// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - virtio MMIO 通用实现 (Phase 4)
 *
 * 实现 virtio 1.0 legacy MMIO 寄存器接口 + virtqueue 操作。
 * 具体设备 (net, block 等) 通过 virtio_dev_ops 回调扩展。
 */

#include "boards/virt/virtio_mmio.h"
#include "boards/virt/vgic.h"
#include "arch/aarch64/sysregs.h"
#include "common/debug.h"
#include "common/mm.h"
#include "common/utils.h"

/*
 * SMP 自旋锁：保护 virtio 设备状态的并发访问。
 *
 * 同一 VM 的多个 vCPU 可能同时访问 virtio 设备（MMIO 读写），
 * 必须串行化所有设备寄存器和 vring 的操作，避免数据竞争。
 *
 * 使用 ldaxr/stxr 独占指令实现 ticketless 自旋锁，
 * 带 acquire/release 语义保证内存顺序。
 */
static inline void virtio_dev_lock(struct virtio_mmio_dev *dev)
{
    unsigned int ok;
    asm volatile(
        "1: ldaxr w0, [%1]\n"
        "   cbnz w0, 1b\n"
        "   mov  w0, #1\n"
        "   stxr %w0, w0, [%1]\n"
        "   cbnz %w0, 1b\n"
        : "=&r"(ok)
        : "r"(&dev->lock)
        : "x0", "memory");
}

static inline void virtio_dev_unlock(struct virtio_mmio_dev *dev)
{
    asm volatile("stlr wzr, [%0]" ::"r"(&dev->lock) : "memory");
}

/*
 * 计算 vring 各部分大小 (legacy 布局)
 *
 * 对齐规则 (virtio spec):
 *   - 描述符表: 16 * num 字节, 按 16 字节对齐
 *   - avail 环: 6 + 2*num 字节, 按 2 字节对齐, 整体按 align 对齐
 *   - used 环: 6 + 8*num 字节, 按 4 字节对齐, 整体按 align 对齐
 *
 * vring_desc 在 page 起始处
 * vring_avail 在 desc 之后, 按 align 对齐
 * vring_used 在 avail 之后, 按 align 对齐
 */

static inline uint32_t vring_desc_size(uint32_t num)
{
    return 16 * num;
}

static inline uint32_t vring_avail_size(uint32_t num)
{
    return 6 + 2 * num;
}

static inline uint32_t vring_used_size(uint32_t num)
{
    return 6 + 8 * num;
}

/* 向上对齐 */
static inline uint32_t align_up(uint32_t val, uint32_t align)
{
    return (val + align - 1) & ~(align - 1);
}

/*
 * 获取 vring 各部分的 guest IPA 基地址
 *   vq->pfn 是整个 vring 的起始页帧号 (page frame number)
 */
static uint64_t vq_desc_base(struct virtqueue *vq)
{
    return (uint64_t)vq->pfn * PAGE_SIZE;
}

static uint64_t vq_avail_base(struct virtqueue *vq)
{
    uint32_t align = vq->align ? vq->align : 4096;
    uint64_t desc_end = vq_desc_base(vq) + vring_desc_size(vq->num);
    return align_up((uint32_t)desc_end, align);
}

static uint64_t vq_used_base(struct virtqueue *vq)
{
    uint32_t align = vq->align ? vq->align : 4096;
    uint64_t avail_end = vq_avail_base(vq) + vring_avail_size(vq->num);
    return align_up((uint32_t)avail_end, align);
}

/* ---------- virtio_mmio_dev 初始化 ---------- */

void virtio_mmio_init(struct virtio_mmio_dev *dev,
                      struct task_struct *vm,
                      uint32_t dev_id,
                      int num_queues,
                      struct virtqueue *vqs,
                      const struct virtio_dev_ops *ops,
                      uint32_t virq)
{
    memzero(dev, sizeof(*dev));
    dev->vm = vm;
    dev->dev_id = dev_id;
    dev->num_queues = num_queues;
    dev->vqs = vqs;
    dev->ops = ops;
    dev->virq = virq;
    dev->guest_page_size = PAGE_SIZE;

    /* 初始化所有队列为空 */
    for (int i = 0; i < num_queues; i++) {
        memzero(&vqs[i], sizeof(vqs[i]));
        vqs[i].num = VIRTQUEUE_MAX_SIZE; /* 默认最大值 */
    }
}

/* ---------- MMIO 寄存器读 ---------- */

uint32_t virtio_mmio_read(struct virtio_mmio_dev *dev, uint32_t offset)
{
    uint32_t val = 0;

    virtio_dev_lock(dev);

    switch (offset) {
    case VIRTIO_MMIO_MAGIC_VALUE:
        val = VIRTIO_MMIO_MAGIC_VAL;
        break;

    case VIRTIO_MMIO_VERSION:
        val = VIRTIO_MMIO_VER_VAL;
        break;

    case VIRTIO_MMIO_DEVICE_ID:
        val = dev->dev_id;
        break;

    case VIRTIO_MMIO_VENDOR_ID:
        val = VIRTIO_MMIO_VENDOR_VAL;
        break;

    case VIRTIO_MMIO_HOST_FEATURES: {
        uint32_t sel = dev->host_features_sel;
        if (sel < 2)
            val = dev->host_features[sel];
        break;
    }

    case VIRTIO_MMIO_QUEUE_NUM_MAX:
        if (dev->queue_sel < dev->num_queues)
            val = VIRTQUEUE_MAX_SIZE;
        break;

    case VIRTIO_MMIO_QUEUE_PFN:
        if (dev->queue_sel < dev->num_queues)
            val = dev->vqs[dev->queue_sel].pfn;
        break;

    case VIRTIO_MMIO_INTERRUPT_STATUS:
        val = dev->irq_status;
        break;

    case VIRTIO_MMIO_STATUS:
        val = dev->status;
        break;

    default:
        /* 设备特定配置寄存器 */
        if (offset >= VIRTIO_MMIO_DEVICE_CONFIG && dev->ops && dev->ops->read_config)
            val = dev->ops->read_config(dev, offset - VIRTIO_MMIO_DEVICE_CONFIG);
        break;
    }

    virtio_dev_unlock(dev);
    return val;
}

/* ---------- MMIO 寄存器写 ---------- */

void virtio_mmio_write(struct virtio_mmio_dev *dev, uint32_t offset, uint32_t val)
{
    virtio_dev_lock(dev);

    switch (offset) {
    case VIRTIO_MMIO_HOST_FEATURES_SEL:
        dev->host_features_sel = val;
        break;

    case VIRTIO_MMIO_GUEST_FEATURES: {
        uint32_t sel = dev->guest_features_sel;
        if (sel < 2)
            dev->guest_features[sel] = val;
        break;
    }

    case VIRTIO_MMIO_GUEST_FEATURES_SEL:
        dev->guest_features_sel = val;
        break;

    case VIRTIO_MMIO_GUEST_PAGE_SIZE:
        dev->guest_page_size = val;
        break;

    case VIRTIO_MMIO_QUEUE_SEL:
        dev->queue_sel = val;
        break;

    case VIRTIO_MMIO_QUEUE_NUM:
        if (dev->queue_sel < dev->num_queues)
            dev->vqs[dev->queue_sel].num = val;
        break;

    case VIRTIO_MMIO_QUEUE_ALIGN:
        if (dev->queue_sel < dev->num_queues)
            dev->vqs[dev->queue_sel].align = val;
        break;

    case VIRTIO_MMIO_QUEUE_PFN:
        if (dev->queue_sel < dev->num_queues) {
            dev->vqs[dev->queue_sel].pfn = val;
            dev->vqs[dev->queue_sel].ready = (val != 0);
            dev->vqs[dev->queue_sel].last_avail_idx = 0;
        }
        break;

    case VIRTIO_MMIO_QUEUE_NOTIFY:
        if (val < dev->num_queues && dev->ops && dev->ops->queue_notify)
            dev->ops->queue_notify(dev, val);
        break;

    case VIRTIO_MMIO_INTERRUPT_ACK:
        dev->irq_status &= ~val;
        break;

    case VIRTIO_MMIO_STATUS: {
        uint8_t old_status = dev->status;
        dev->status = (uint8_t)val;
        if (dev->ops && dev->ops->status_change)
            dev->ops->status_change(dev, old_status, (uint8_t)val);
        break;
    }

    default:
        /* 设备特定配置寄存器 */
        if (offset >= VIRTIO_MMIO_DEVICE_CONFIG && dev->ops && dev->ops->write_config)
            dev->ops->write_config(dev, offset - VIRTIO_MMIO_DEVICE_CONFIG, val);
        break;
    }

    virtio_dev_unlock(dev);
}

/* ---------- 中断注入 ---------- */

void virtio_mmio_inject_irq(struct virtio_mmio_dev *dev, uint32_t irq_bits)
{
    dev->irq_status |= irq_bits;
    /* 注入虚拟 SPI 中断 */
    vgic_inject_virq(dev->virq);
}

/* ---------- virtqueue 操作 ---------- */

int vq_has_avail(struct virtio_mmio_dev *dev, struct virtqueue *vq)
{
    if (!vq->ready)
        return 0;

    uint64_t avail_base = vq_avail_base(vq);
    uint16_t idx;

    /* 读 avail->idx (offset 2) */
    if (vq_copy_from_guest(dev, avail_base + 2, &idx, sizeof(idx)) != sizeof(idx))
        return 0;

    return idx != vq->last_avail_idx;
}

int vq_pop_avail(struct virtio_mmio_dev *dev, struct virtqueue *vq)
{
    if (!vq_has_avail(dev, vq))
        return -1;

    uint64_t avail_base = vq_avail_base(vq);

    /* avail->ring[last_avail_idx % num] */
    uint16_t ring_idx = vq->last_avail_idx % vq->num;
    uint16_t desc_idx;

    if (vq_copy_from_guest(dev, avail_base + 4 + ring_idx * 2,
                           &desc_idx, sizeof(desc_idx)) != sizeof(desc_idx))
        return -1;

    vq->last_avail_idx++;
    return desc_idx;
}

int vq_read_desc(struct virtio_mmio_dev *dev, struct virtqueue *vq,
                 uint16_t desc_idx,
                 uint64_t *addr_out, uint32_t *len_out,
                 uint16_t *flags_out, uint16_t *next_out)
{
    if (!vq->ready || desc_idx >= vq->num)
        return -1;

    uint64_t desc_base = vq_desc_base(vq);
    uint64_t desc_addr = desc_base + desc_idx * 16;

    /*
     * 一次性读取整个 16 字节描述符。
     * 不要分开多次读（addr / len / flags / next 各读一次），
     * 因为多次小粒度读取在某些 cache 配置下会出现第一次读到零的问题。
     */
    struct vring_desc {
        uint64_t addr;
        uint32_t len;
        uint16_t flags;
        uint16_t next;
    } desc;

    if (vq_copy_from_guest(dev, desc_addr, &desc, sizeof(desc)) != sizeof(desc))
        return -1;

    if (addr_out)  *addr_out  = desc.addr;
    if (len_out)   *len_out   = desc.len;
    if (flags_out) *flags_out = desc.flags;
    if (next_out)  *next_out  = desc.next;

    return 0;
}

static void *ipa_to_hv_va(struct task_struct *vm, uint64_t ipa, int *writable);
static void dcache_invalidate_range(void *start, size_t len);
static void dcache_clean_range(void *start, size_t len);

void vq_add_used(struct virtio_mmio_dev *dev, struct virtqueue *vq,
                 uint16_t desc_idx, uint32_t len)
{
	if (!vq->ready)
		return;

	uint64_t used_base = vq_used_base(vq);
	uint64_t page_ipa = used_base & PAGE_MASK;
	uint64_t page_off = used_base & (PAGE_SIZE - 1);
	void *page_va;

	/* 确保页面已映射（used 环 guest 不会先访问，需要 hypervisor 主动分配） */
	page_va = ipa_to_hv_va(dev->vm, page_ipa, NULL);
	if (!page_va) {
		page_va = allocate_task_page(dev->vm, page_ipa);
		if (!page_va)
			return;
	}

	uint16_t idx = *(volatile uint16_t *)((char *)page_va + page_off + 2);
	uint16_t ring_idx = idx % vq->num;
	uint64_t ring_off = page_off + 4 + ring_idx * 8;

	/* 写 ring entry */
	*(volatile uint32_t *)((char *)page_va + ring_off) = desc_idx;
	*(volatile uint32_t *)((char *)page_va + ring_off + 4) = len;

	/* 更新 idx */
	idx++;
	*(volatile uint16_t *)((char *)page_va + page_off + 2) = idx;

	/* 确保数据写回内存（guest 以 non-cacheable 方式读） */
	dcache_clean_range(page_va, PAGE_SIZE);
}

/* ---------- Guest 内存访问 (阶段 2 翻译) ---------- */

/*
 * Cache 维护辅助函数：按 VA 做 cache 操作
 * ARMv8 data cache line size 通常是 64 字节（从 CTR_EL0 读取）
 */
static inline uint64_t get_dcache_line_size(void)
{
    uint64_t ctr;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    /* DminLine: bits [19:16], log2(number of words) */
    return 4UL << ((ctr >> 16) & 0xf);
}

static void dcache_invalidate_range(void *start, size_t len)
{
    uint64_t line_size = get_dcache_line_size();
    uintptr_t p = (uintptr_t)start & ~(line_size - 1);
    uintptr_t end = (uintptr_t)start + len;

    while (p < end) {
        /*
         * 用 ivac (Invalidate to PoC) 而不是 civac。
         *
         * 注意：只有在确定内存中的数据比 cache 中的更新时才用 ivac。
         * 当 Stage 2 映射为 Non-cacheable 时，guest 写直接到内存，
         * 而 hypervisor cache 中可能有旧数据（如 demand paging 时
         * memzero 的零）。此时 ivac 丢弃 hypervisor cache 中的旧数据，
         * 下次读时从内存加载最新数据。
         *
         * 不能用 civac：因为 civac 会先 clean（写回内存），
         * 这会把 hypervisor cache 中的旧数据（零）写回内存，
         * 覆盖 guest 刚写的新数据！
         */
        asm volatile("dc ivac, %0" ::"r"(p) : "memory");
        p += line_size;
    }
    asm volatile("dsb sy" ::: "memory");
}

static void dcache_clean_range(void *start, size_t len)
{
    uint64_t line_size = get_dcache_line_size();
    uintptr_t p = (uintptr_t)start & ~(line_size - 1);
    uintptr_t end = (uintptr_t)start + len;

    while (p < end) {
        asm volatile("dc cvac, %0" ::"r"(p) : "memory");
        p += line_size;
    }
    asm volatile("dsb sy" ::: "memory");
}

/*
 * 通过阶段 2 页表将 guest IPA 转换为 hypervisor 虚拟地址
 * 由于 hypervisor 使用等值映射 (VA = PA)，我们只需做 stage 2 walk 找到 PA，
 * 然后用 TO_VADDR() 转换即可。
 */
static void *ipa_to_hv_va(struct task_struct *vm, uint64_t ipa, int *writable)
{
    paddr_t table = vm->mm.first_table;

    if (!table)
        return NULL;

    /* Level 1 */
    uint64_t lv1_idx = (ipa >> LV1_SHIFT) & (PTRS_PER_TABLE - 1);
    uint64_t *lv1 = (uint64_t *)TO_VADDR(table);

    if (!(lv1[lv1_idx] & 1))
        return NULL;  /* 未映射 */

    /* Level 2 */
    paddr_t lv2_table = lv1[lv1_idx] & PAGE_MASK;
    uint64_t lv2_idx = (ipa >> LV2_SHIFT) & (PTRS_PER_TABLE - 1);
    uint64_t *lv2 = (uint64_t *)TO_VADDR(lv2_table);

    if (!(lv2[lv2_idx] & 1))
        return NULL;

    /* Level 3 */
    paddr_t lv3_table = lv2[lv2_idx] & PAGE_MASK;
    uint64_t lv3_idx = (ipa >> PAGE_SHIFT) & (PTRS_PER_TABLE - 1);
    uint64_t *lv3 = (uint64_t *)TO_VADDR(lv3_table);

    if (!(lv3[lv3_idx] & 1))
        return NULL;

    paddr_t pa = lv3[lv3_idx] & PAGE_MASK;
    pa |= (ipa & (PAGE_SIZE - 1));

    if (writable)
        *writable = (lv3[lv3_idx] & 0x2) ? 1 : 0;

    return (void *)TO_VADDR(pa);
}

int vq_copy_from_guest(struct virtio_mmio_dev *dev,
                       uint64_t guest_ipa, void *dst, int len)
{
    int copied = 0;

    while (len > 0) {
        int page_off = guest_ipa & (PAGE_SIZE - 1);
        int chunk = len < (PAGE_SIZE - page_off) ? len : (PAGE_SIZE - page_off);
        void *src;
        int w;

        src = ipa_to_hv_va(dev->vm, guest_ipa, &w);
        if (!src) {
            /*
             * 页面尚未映射：主动做 demand paging。
             * 读未映射的页面会返回零（新分配的页面是 zeroed 的），
             * 这与 guest 未写入的语义一致。
             */
            src = allocate_task_page(dev->vm, guest_ipa & PAGE_MASK);
            if (!src)
                break;
        }

        /*
         * Invalidate cache before reading guest memory.
         * Guest 写入后数据在内存中，但 hypervisor cache 可能有旧数据，
         * 需要 invalidate cache 强制从内存重新加载。
         */
        dcache_invalidate_range(src, chunk);

        memcpy(dst, src, chunk);
        copied += chunk;
        guest_ipa += chunk;
        dst = (char *)dst + chunk;
        len -= chunk;
    }

    return copied;
}

int vq_copy_to_guest(struct virtio_mmio_dev *dev,
                     uint64_t guest_ipa, const void *src, int len)
{
    int copied = 0;

    while (len > 0) {
        int page_off = guest_ipa & (PAGE_SIZE - 1);
        int chunk = len < (PAGE_SIZE - page_off) ? len : (PAGE_SIZE - page_off);
        void *dst;
        int w;

        dst = ipa_to_hv_va(dev->vm, guest_ipa, &w);
        if (!dst) {
            /*
             * 页面尚未映射：主动做 demand paging。
             * 例如 used 环的页面 guest 不会先访问，
             * hypervisor 写的时候需要先分配。
             */
            dst = allocate_task_page(dev->vm, guest_ipa & PAGE_MASK);
            if (!dst)
                break;
        }

        memcpy(dst, src, chunk);

        /*
         * Clean cache after writing to guest memory.
         * 把 cache 中的数据写回到内存，
         * 确保 Guest 以 non-cacheable 方式读的时候能看到最新数据。
         */
        dcache_clean_range(dst, chunk);

        copied += chunk;
        guest_ipa += chunk;
        src = (const char *)src + chunk;
        len -= chunk;
    }

    return copied;
}
