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
    switch (offset) {
    case VIRTIO_MMIO_MAGIC_VALUE:
        return VIRTIO_MMIO_MAGIC_VAL;

    case VIRTIO_MMIO_VERSION:
        return VIRTIO_MMIO_VER_VAL;

    case VIRTIO_MMIO_DEVICE_ID:
        return dev->dev_id;

    case VIRTIO_MMIO_VENDOR_ID:
        return VIRTIO_MMIO_VENDOR_VAL;

    case VIRTIO_MMIO_HOST_FEATURES: {
        uint32_t sel = dev->host_features_sel;
        if (sel < 2)
            return dev->host_features[sel];
        return 0;
    }

    case VIRTIO_MMIO_QUEUE_NUM_MAX:
        if (dev->queue_sel < dev->num_queues)
            return VIRTQUEUE_MAX_SIZE;
        return 0;

    case VIRTIO_MMIO_QUEUE_PFN:
        if (dev->queue_sel < dev->num_queues)
            return dev->vqs[dev->queue_sel].pfn;
        return 0;

    case VIRTIO_MMIO_INTERRUPT_STATUS:
        return dev->irq_status;

    case VIRTIO_MMIO_STATUS:
        return dev->status;

    default:
        /* 设备特定配置寄存器 */
        if (offset >= VIRTIO_MMIO_DEVICE_CONFIG && dev->ops && dev->ops->read_config)
            return dev->ops->read_config(dev, offset - VIRTIO_MMIO_DEVICE_CONFIG);
        return 0;
    }
}

/* ---------- MMIO 寄存器写 ---------- */

void virtio_mmio_write(struct virtio_mmio_dev *dev, uint32_t offset, uint32_t val)
{
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
    uint64_t addr;
    uint32_t len;
    uint16_t flags, next;

    if (vq_copy_from_guest(dev, desc_addr, &addr, 8) != 8)
        return -1;
    if (vq_copy_from_guest(dev, desc_addr + 8, &len, 4) != 4)
        return -1;
    if (vq_copy_from_guest(dev, desc_addr + 12, &flags, 2) != 2)
        return -1;
    if (vq_copy_from_guest(dev, desc_addr + 14, &next, 2) != 2)
        return -1;

    if (addr_out)  *addr_out  = addr;
    if (len_out)   *len_out   = len;
    if (flags_out) *flags_out = flags;
    if (next_out)  *next_out  = next;

    return 0;
}

void vq_add_used(struct virtio_mmio_dev *dev, struct virtqueue *vq,
                 uint16_t desc_idx, uint32_t len)
{
    if (!vq->ready)
        return;

    uint64_t used_base = vq_used_base(vq);
    uint16_t idx;

    /* 读 used->idx */
    if (vq_copy_from_guest(dev, used_base + 2, &idx, sizeof(idx)) != sizeof(idx))
        return;

    /* 写 used->ring[idx % num] */
    uint16_t ring_idx = idx % vq->num;
    uint64_t ring_entry = used_base + 4 + ring_idx * 8;
    uint32_t id = desc_idx;

    vq_copy_to_guest(dev, ring_entry, &id, 4);
    vq_copy_to_guest(dev, ring_entry + 4, &len, 4);

    /* 增加 used->idx */
    idx++;
    vq_copy_to_guest(dev, used_base + 2, &idx, sizeof(idx));
}

/* ---------- Guest 内存访问 (阶段 2 翻译) ---------- */

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
        if (!src)
            break;

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
        if (!dst)
            break;

        memcpy(dst, src, chunk);
        copied += chunk;
        guest_ipa += chunk;
        src = (const char *)src + chunk;
        len -= chunk;
    }

    return copied;
}
