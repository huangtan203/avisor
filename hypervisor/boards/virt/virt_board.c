// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - QEMU virt 平台板级支持
 *
 * Phase 4：设备虚拟化
 *
 * 新增内容：
 *   1. MMIO 陷获框架：translation fault / permission fault 统一分派
 *   2. 虚拟设备地址空间：
 *        0x0A000000 - 0x0A000FFF : virtio-net MMIO
 *        0x0A001000 - 0x0A001FFF : ivshmem 控制寄存器
 *        0x0B000000 - 0x0B0FFFFF : ivshmem 共享内存 (1MB)
 *   3. virtio-net 设备模型（环回模式）
 *   4. ivshmem 共享内存设备（VM 间通信）
 *
 * Phase 3 保留：
 *   世界切换保存/恢复（GICv3 虚拟接口 + 虚拟定时器）
 */

#include "boards/virt/virt_board.h"
#include "boards/virt/virt_mm.h"
#include "boards/virt/gicv3.h"
#include "boards/virt/vgic.h"
#include "boards/virt/virtio_mmio.h"
#include "boards/virt/ivshmem.h"
#include "arch/aarch64/sysregs.h"
#include "common/mm.h"
#include "common/types.h"
#include "common/debug.h"
#include "common/utils.h"

/* ---------- 虚拟设备地址空间 ---------- */
#define VIRTIO_NET_BASE   0x0A000000UL
#define VIRTIO_NET_SIZE   0x00001000UL   /* 4KB */
#define VIRTIO_IVSHMEM_REG_BASE  0x0A001000UL
#define VIRTIO_IVSHMEM_REG_SIZE  0x00001000UL   /* 4KB */
#define VIRTIO_IVSHMEM_MEM_BASE  0x0B000000UL
#define VIRTIO_IVSHMEM_MEM_SIZE  0x00100000UL   /* 1MB */

/* ivshmem 共享内存物理地址（从页池分配） */
static uint64_t ivshmem_shmem_pa = 0;

/*
 * per-VM 设备模型数据
 *
 * 每个 VM 有自己的 virtio-net 设备实例和 ivshmem 控制寄存器实例。
 * 存储在 task_struct 的 board_data 字段中。
 */
struct virt_vm_devices {
    /* virtio-net 设备 */
    struct virtio_mmio_dev virtio_net;
    struct virtio_net_dev {
        uint8_t  mac[6];
        uint16_t link_status;
        uint16_t max_vq_pairs;
        uint8_t  loopback_buf[2048];
        int      loopback_len;
        int      loopback_pending;
    } virtio_net_priv;
    struct virtqueue virtio_net_vqs[2];

    /* ivshmem 设备 */
    struct ivshmem_dev ivshmem;
};

/* ---------- 页面分配池 ---------- */
static uint8_t virt_page_memap[PAGING_PAGES] = { 0 };

static struct page_pool virt_page_pool = {
	.start_addr = LOW_MEMORY,
	.page_nr = PAGING_PAGES,
	.memap = virt_page_memap,
	.last_page_id = 0,
};

struct page_pool *get_virt_page_pool(void)
{
	return &virt_page_pool;
}

/* ---------- virtio-net 设备实现 (内联，简化依赖) ---------- */

#define NET_VQ_RX   0
#define NET_VQ_TX   1

#define VIRTIO_NET_F_MAC    (1 << 5)
#define VIRTIO_NET_F_STATUS (1 << 16)

#define VIRTIO_NET_CFG_MAC          0x00
#define VIRTIO_NET_CFG_STATUS       0x06
#define VIRTIO_NET_CFG_MAX_VQ_PAIRS 0x08

static uint32_t virtio_net_read_config(struct virtio_mmio_dev *dev,
                                        uint32_t offset)
{
    struct virtio_net_dev *net = (struct virtio_net_dev *)dev->priv;
    uint32_t val = 0;

    switch (offset) {
    case VIRTIO_NET_CFG_MAC + 0:
        val = net->mac[0] | (net->mac[1] << 8) | (net->mac[2] << 16) | (net->mac[3] << 24);
        break;
    case VIRTIO_NET_CFG_MAC + 4:
        val = net->mac[4] | (net->mac[5] << 8);
        break;
    case VIRTIO_NET_CFG_STATUS:
        val = net->link_status;
        break;
    case VIRTIO_NET_CFG_MAX_VQ_PAIRS:
        val = net->max_vq_pairs;
        break;
    default:
        break;
    }
    return val;
}

static void virtio_net_write_config(struct virtio_mmio_dev *dev,
                                     uint32_t offset, uint32_t val)
{
    (void)dev; (void)offset; (void)val;
}

static int read_tx_packet(struct virtio_mmio_dev *dev, uint16_t desc_head,
                          uint8_t *buf, int buf_len)
{
    struct virtqueue *txq = &dev->vqs[NET_VQ_TX];
    uint16_t idx = desc_head;
    int total = 0;

    while (1) {
        uint64_t addr;
        uint32_t len;
        uint16_t flags, next;

        if (vq_read_desc(dev, txq, idx, &addr, &len, &flags, &next) < 0)
            break;

        int copy_len = len < (buf_len - total) ? len : (buf_len - total);
        if (copy_len > 0) {
            int copied = vq_copy_from_guest(dev, addr, buf + total, copy_len);
            total += copied;
            if (copied != copy_len)
                break;
        }

        if (!(flags & VRING_DESC_F_NEXT))
            break;
        idx = next;
    }
    return total;
}

static int write_rx_packet(struct virtio_mmio_dev *dev,
                           const uint8_t *buf, int len)
{
    struct virtqueue *rxq = &dev->vqs[NET_VQ_RX];

    INFO("virtio-net: rx pfn=0x%x num=%d", rxq->pfn, rxq->num);

    if (!vq_has_avail(dev, rxq))
        return -1;

    int desc_head = vq_pop_avail(dev, rxq);
    if (desc_head < 0)
        return -1;

    uint16_t idx = (uint16_t)desc_head;
    int remaining = len;
    int written = 0;

    while (1) {
        uint64_t addr;
        uint32_t dlen;
        uint16_t flags, next;

        if (vq_read_desc(dev, rxq, idx, &addr, &dlen, &flags, &next) < 0)
            break;

        int copy_len = remaining < dlen ? remaining : dlen;
        if (copy_len > 0) {
            int copied = vq_copy_to_guest(dev, addr, buf + written, copy_len);
            written += copied;
            remaining -= copied;
            if (copied != copy_len)
                break;
        }

        if (!(flags & VRING_DESC_F_NEXT))
            break;
        idx = next;
    }

    vq_add_used(dev, rxq, (uint16_t)desc_head, written);

    virtio_mmio_inject_irq(dev, VIRTIO_IRQ_USED_BUFFER);
    return 0;
}

static void virtio_net_queue_notify(struct virtio_mmio_dev *dev, uint32_t qidx)
{
    if (qidx != NET_VQ_TX)
        return;

    struct virtqueue *txq = &dev->vqs[NET_VQ_TX];
    struct virtio_net_dev *net = (struct virtio_net_dev *)dev->priv;

    while (vq_has_avail(dev, txq)) {
        int desc_head = vq_pop_avail(dev, txq);
        if (desc_head < 0)
            break;

        int pkt_len = read_tx_packet(dev, (uint16_t)desc_head,
                                     net->loopback_buf,
                                     sizeof(net->loopback_buf));

        vq_add_used(dev, txq, (uint16_t)desc_head, 0);

        if (pkt_len > 0) {
            write_rx_packet(dev, net->loopback_buf, pkt_len);
        }
    }

    virtio_mmio_inject_irq(dev, VIRTIO_IRQ_USED_BUFFER);
}

static void virtio_net_status_change(struct virtio_mmio_dev *dev,
                                     uint8_t old_status, uint8_t new_status)
{
    (void)old_status;
    if (new_status & VIRTIO_STATUS_DRIVER_OK) {
        struct virtio_net_dev *net = (struct virtio_net_dev *)dev->priv;
        INFO("virtio-net: driver ready (mac=%02x:%02x:%02x:%02x:%02x:%02x)",
             net->mac[0], net->mac[1], net->mac[2],
             net->mac[3], net->mac[4], net->mac[5]);
    }
}

static const struct virtio_dev_ops virtio_net_ops = {
    .read_config = virtio_net_read_config,
    .write_config = virtio_net_write_config,
    .queue_notify = virtio_net_queue_notify,
    .status_change = virtio_net_status_change,
};

static void init_virtio_net_device(struct virt_vm_devices *devs,
                                   struct task_struct *vm, int vmid)
{
    struct virtio_net_dev *net = &devs->virtio_net_priv;

    memzero(net, sizeof(*net));
    /* MAC: 52:54:00:12:34:5x, 最后一位随 VM ID 变化 */
    net->mac[0] = 0x52;
    net->mac[1] = 0x54;
    net->mac[2] = 0x00;
    net->mac[3] = 0x12;
    net->mac[4] = 0x34;
    net->mac[5] = 0x50 + (uint8_t)(vmid & 0xf);
    net->link_status = 1;
    net->max_vq_pairs = 1;

    virtio_mmio_init(&devs->virtio_net, vm, VIRTIO_ID_NET, 2,
                     devs->virtio_net_vqs, &virtio_net_ops,
                     VIRQ_VIRTIO_NET);
    devs->virtio_net.priv = net;
    devs->virtio_net.host_features[0] = VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS;
    devs->virtio_net.host_features[1] = 0;
}

/* ---------- MMIO 地址检查 ---------- */

static int virt_is_mmio_addr(struct task_struct *tsk, unsigned long ipa)
{
    (void)tsk;

    /* virtio-net MMIO 区域 */
    if (ipa >= VIRTIO_NET_BASE && ipa < VIRTIO_NET_BASE + VIRTIO_NET_SIZE)
        return 1;

    /* ivshmem 控制寄存器区域 */
    if (ipa >= VIRTIO_IVSHMEM_REG_BASE && ipa < VIRTIO_IVSHMEM_REG_BASE + VIRTIO_IVSHMEM_REG_SIZE)
        return 1;

    /* 注意：ivshmem 共享内存区域是真实内存映射，不是 MMIO 模拟 */

    return 0;
}

/* ---------- MMIO 读/写分派 ---------- */

static struct virt_vm_devices *get_vm_devices(struct task_struct *tsk)
{
    return (struct virt_vm_devices *)tsk->board_data;
}

static unsigned long virt_mmio_read(struct task_struct *tsk,
                                    unsigned long ipa)
{
    struct virt_vm_devices *devs = get_vm_devices(tsk);
    if (!devs)
        return 0;

    /* virtio-net */
    if (ipa >= VIRTIO_NET_BASE && ipa < VIRTIO_NET_BASE + VIRTIO_NET_SIZE) {
        uint32_t offset = (uint32_t)(ipa - VIRTIO_NET_BASE);
        return virtio_mmio_read(&devs->virtio_net, offset);
    }

    /* ivshmem 控制寄存器 */
    if (ipa >= VIRTIO_IVSHMEM_REG_BASE && ipa < VIRTIO_IVSHMEM_REG_BASE + VIRTIO_IVSHMEM_REG_SIZE) {
        uint32_t offset = (uint32_t)(ipa - VIRTIO_IVSHMEM_REG_BASE);
        return ivshmem_mmio_read(&devs->ivshmem, offset);
    }

    return 0;
}

static void virt_mmio_write(struct task_struct *tsk,
                            unsigned long ipa, unsigned long val)
{
    struct virt_vm_devices *devs = get_vm_devices(tsk);
    if (!devs)
        return;

    /* virtio-net */
    if (ipa >= VIRTIO_NET_BASE && ipa < VIRTIO_NET_BASE + VIRTIO_NET_SIZE) {
        uint32_t offset = (uint32_t)(ipa - VIRTIO_NET_BASE);
        virtio_mmio_write(&devs->virtio_net, offset, (uint32_t)val);
        return;
    }

    /* ivshmem 控制寄存器 */
    if (ipa >= VIRTIO_IVSHMEM_REG_BASE && ipa < VIRTIO_IVSHMEM_REG_BASE + VIRTIO_IVSHMEM_REG_SIZE) {
        uint32_t offset = (uint32_t)(ipa - VIRTIO_IVSHMEM_REG_BASE);
        ivshmem_mmio_write(&devs->ivshmem, offset, (uint32_t)val);
        return;
    }
}

/* ---------- ivshmem 共享内存初始化 ---------- */

static uint64_t alloc_shared_memory(size_t size)
{
    /*
     * 分配共享内存的物理页。
     * 注意：这些页可能物理不连续，但在 guest 的 IPA 空间中
     * 我们会映射为连续的（通过 stage 2 页表）。
     * 返回第一页的物理地址（用于引用跟踪）。
     */
    int num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    paddr_t first_page = 0;

    for (int i = 0; i < num_pages; i++) {
        void *page = allocate_page();
        if (!page) {
            PANIC("ivshmem: failed to allocate shared memory page %d", i);
            return 0;
        }
        memzero(page, PAGE_SIZE);
        if (i == 0)
            first_page = TO_PADDR((uint64_t)page);
    }

    return first_page;
}

/* ---------- VM 初始化 ---------- */

static void virt_initialize(struct task_struct *tsk)
{
	tsk->mm.cntvoff = READ_SYSREG64(CNTPCT_EL0);
	tsk->virt.gic_vmcr = (0xF8UL << ICH_VMCR_VPMR_SHIFT) | ICH_VMCR_VENG1;

    /*
     * 分配 per-VM 设备模型数据结构
     * 注意：这里用 allocate_page 分配，假设 PAGE_SIZE 足够存下
     * struct virt_vm_devices（目前约 2KB，远小于 4KB）
     */
    struct virt_vm_devices *devs =
        (struct virt_vm_devices *)allocate_page();
    if (!devs) {
        PANIC("virt_initialize: failed to allocate device data");
        return;
    }
    memzero(devs, sizeof(*devs));
    tsk->board_data = devs;

    int vmid = (int)tsk->pid;

    /* 初始化 virtio-net 设备 */
    init_virtio_net_device(devs, tsk, vmid);

    /* 分配全局共享内存（只在第一个 VM 初始化时做一次） */
    if (ivshmem_shmem_pa == 0) {
        ivshmem_shmem_pa = alloc_shared_memory(VIRTIO_IVSHMEM_MEM_SIZE);
        INFO("ivshmem: shared memory allocated at PA 0x%lx (%d KB)",
             ivshmem_shmem_pa, (int)(VIRTIO_IVSHMEM_MEM_SIZE / 1024));
    }

    /* 初始化 ivshmem 设备 (peer_id = vmid - 1, 因为 vmid 从 1 开始) */
    int peer_id = vmid - 1;
    if (peer_id < 0 || peer_id >= IVSHMEM_MAX_PEERS)
        peer_id = 0;

    ivshmem_init(&devs->ivshmem, tsk, peer_id,
                 ivshmem_shmem_pa, VIRTIO_IVSHMEM_MEM_SIZE,
                 VIRTIO_IVSHMEM_REG_BASE, VIRTIO_IVSHMEM_MEM_BASE);
}

/* ---------- 世界切换 ---------- */

static void virt_leaving_vm(struct task_struct *tsk)
{
	uint64_t lr0, ctl;

	/* 1. 保存虚拟 GIC 接口全部状态（VMCR + 所有 LR） */
	vgic_save_state(&tsk->virt);
	lr0 = tsk->virt.gic_lr[0];

	/*
	 * 2. 若 LR0 是 vtimer 的 HW 映射且非 invalid，
	 *    强制 deactivate 物理 PPI 27。
	 */
	if ((lr0 & ICH_LR_HW) && (lr0 & ICH_LR_STATE_MASK)) {
		uint32_t pintid = (uint32_t)(lr0 >> 32) & 0x3ff;

		if (pintid == PPI_VIRT_TIMER) {
			WRITE_SYSREG64(PPI_VIRT_TIMER, ICC_DIR_EL1);
			asm volatile("isb");
		}
	}

	/* 3. 保存虚拟定时器状态 */
	tsk->virt.cntv_cval = READ_SYSREG64(CNTV_CVAL_EL0);
	ctl = READ_SYSREG64(CNTV_CTL_EL0);
	tsk->virt.cntv_ctl = ctl;

	if (ctl & CNTV_CTL_ISTATUS)
		tsk->virt.vtimer_pending = 1;

	/* 4. 禁用物理 CNTV */
	WRITE_SYSREG64(0, CNTV_CTL_EL0);
	asm volatile("isb");
}

static void virt_entering_vm(struct task_struct *tsk)
{
	/* 1. 恢复虚拟时间偏移 */
	WRITE_SYSREG64(tsk->mm.cntvoff, CNTVOFF_EL2);

	/* 2. 恢复虚拟 GIC 接口全部状态（VMCR + 所有 LR） */
	vgic_restore_state(&tsk->virt);

	/* 3. 恢复虚拟定时器 */
	WRITE_SYSREG64(tsk->virt.cntv_cval, CNTV_CVAL_EL0);
	WRITE_SYSREG64(tsk->virt.cntv_ctl, CNTV_CTL_EL0);
	asm volatile("isb");

	/* 4. 补注入离开前缓存的 vtimer 虚拟中断 */
	if (tsk->virt.vtimer_pending) {
		tsk->virt.vtimer_pending = 0;
		vgic_inject_virq_hw(PPI_VIRT_TIMER, PPI_VIRT_TIMER);
	}
}

/* ---------- board ops ---------- */

const struct board_ops virt_board_ops = {
	.initialize = virt_initialize,
	.mmio_read = virt_mmio_read,
	.mmio_write = virt_mmio_write,
	.is_mmio_addr = virt_is_mmio_addr,
	.entering_vm = virt_entering_vm,
	.leaving_vm = virt_leaving_vm,
	.is_irq_asserted = NULL,
	.is_fiq_asserted = NULL,
	.debug = NULL,
};
