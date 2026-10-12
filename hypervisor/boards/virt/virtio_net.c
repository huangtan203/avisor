// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - virtio-net 设备模型 (Phase 4)
 *
 * 实现 virtio 1.0 网络设备（legacy MMIO 接口）。
 * 支持环回模式：发送队列的包被回送到接收队列。
 *
 * virtio-net 有两个 virtqueue:
 *   - q0: receive queue (rx) - 设备 -> 驱动
 *   - q1: transmit queue (tx) - 驱动 -> 设备
 *
 * 设备配置空间:
 *   struct virtio_net_config {
 *       u8 mac[6];
 *       u16 status;  // link status (VIRTIO_NET_S_LINK_UP = 1)
 *       u16 max_virtqueue_pairs;
 *   };
 */

#include "boards/virt/virtio_mmio.h"
#include "boards/virt/vgic.h"
#include "common/debug.h"
#include "common/utils.h"

/* virtio-net 特性位 (常用) */
#define VIRTIO_NET_F_MAC        (1 << 5)   /* 设备有 MAC 地址 */
#define VIRTIO_NET_F_STATUS     (1 << 16)  /* 设备有状态字段 */

/* virtio-net 配置空间偏移 */
#define VIRTIO_NET_CFG_MAC      0x00  /* 6 bytes */
#define VIRTIO_NET_CFG_STATUS   0x06  /* 2 bytes */
#define VIRTIO_NET_CFG_MAX_VQ_PAIRS 0x08 /* 2 bytes */

/* 队列索引 */
#define NET_VQ_RX   0  /* receive queue */
#define NET_VQ_TX   1  /* transmit queue */

/* 设备私有数据 */
struct virtio_net_dev {
    uint8_t  mac[6];
    uint16_t link_status;
    uint16_t max_vq_pairs;

    /* 环回缓冲区 (简化实现，最多缓冲一个包) */
    uint8_t  loopback_buf[2048];
    int      loopback_len;
    int      loopback_pending;
};

/*
 * 读取设备配置寄存器
 */
static uint32_t virtio_net_read_config(struct virtio_mmio_dev *dev, uint32_t offset)
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

/*
 * 写入设备配置寄存器 (virtio-net 配置只读)
 */
static void virtio_net_write_config(struct virtio_mmio_dev *dev,
                                    uint32_t offset, uint32_t val)
{
    (void)dev; (void)offset; (void)val;
    /* 配置空间只读 */
}

/*
 * 从 tx 队列读出包 (描述符链)
 * 返回读取的字节数
 */
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

/*
 * 把包写入 rx 队列的第一个可用缓冲区
 * 返回 0 成功, -1 失败
 */
static int write_rx_packet(struct virtio_mmio_dev *dev,
                           const uint8_t *buf, int len)
{
    struct virtqueue *rxq = &dev->vqs[NET_VQ_RX];

    /* 找一个 rx 描述符链 (guest 预先放好的空缓冲区) */
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

    /* 标记 used */
    vq_add_used(dev, rxq, (uint16_t)desc_head, written);

    /* 注入接收中断 */
    virtio_mmio_inject_irq(dev, VIRTIO_IRQ_USED_BUFFER);

    return 0;
}

/*
 * 队列通知：guest 往 tx 队列放了包
 *
 * 环回模式：把 tx 队列的包回送到 rx 队列
 */
static void virtio_net_queue_notify(struct virtio_mmio_dev *dev, uint32_t qidx)
{
    if (qidx != NET_VQ_TX)
        return;

    struct virtqueue *txq = &dev->vqs[NET_VQ_TX];
    struct virtio_net_dev *net = (struct virtio_net_dev *)dev->priv;

    /* 处理所有 tx 描述符 */
    while (vq_has_avail(dev, txq)) {
        int desc_head = vq_pop_avail(dev, txq);
        if (desc_head < 0)
            break;

        /* 读出发送包 */
        int pkt_len = read_tx_packet(dev, (uint16_t)desc_head,
                                     net->loopback_buf,
                                     sizeof(net->loopback_buf));

        /* 标记 tx 描述符为 used (发送完成) */
        vq_add_used(dev, txq, (uint16_t)desc_head, 0);

        if (pkt_len > 0) {
            /* 环回：把包写入 rx 队列 */
            write_rx_packet(dev, net->loopback_buf, pkt_len);
        }
    }

    /* 注入 tx 完成中断 (used buffer) */
    virtio_mmio_inject_irq(dev, VIRTIO_IRQ_USED_BUFFER);
}

/*
 * 状态变更回调
 */
static void virtio_net_status_change(struct virtio_mmio_dev *dev,
                                     uint8_t old_status, uint8_t new_status)
{
    (void)old_status;

    if (new_status & VIRTIO_STATUS_DRIVER_OK) {
        INFO("virtio-net: driver ready (mac=%02x:%02x:%02x:%02x:%02x:%02x)",
             ((uint8_t *)dev->priv)[0], ((uint8_t *)dev->priv)[1],
             ((uint8_t *)dev->priv)[2], ((uint8_t *)dev->priv)[3],
             ((uint8_t *)dev->priv)[4], ((uint8_t *)dev->priv)[5]);
    }
}

/* 设备操作表 */
static const struct virtio_dev_ops virtio_net_ops = {
    .read_config = virtio_net_read_config,
    .write_config = virtio_net_write_config,
    .queue_notify = virtio_net_queue_notify,
    .status_change = virtio_net_status_change,
};

/*
 * 初始化 virtio-net 设备
 *   dev: 预分配的 virtio_mmio_dev
 *   net_priv: 预分配的私有数据
 *   vqs: 预分配的 virtqueue 数组 (至少 2 个)
 *   vm: 所属 VM
 *   mac: MAC 地址 (6 字节)
 */
void virtio_net_init(struct virtio_mmio_dev *dev,
                     struct virtio_net_dev *net_priv,
                     struct virtqueue *vqs,
                     struct task_struct *vm,
                     const uint8_t *mac)
{
    /* 初始化私有数据 */
    memzero(net_priv, sizeof(*net_priv));
    if (mac)
        memcpy(net_priv->mac, mac, 6);
    else {
        /* 默认 MAC: 52:54:00:12:34:56 (QEMU 风格) */
        net_priv->mac[0] = 0x52;
        net_priv->mac[1] = 0x54;
        net_priv->mac[2] = 0x00;
        net_priv->mac[3] = 0x12;
        net_priv->mac[4] = 0x34;
        net_priv->mac[5] = 0x56;
    }
    net_priv->link_status = 1; /* link up */
    net_priv->max_vq_pairs = 1;

    /* 初始化通用 virtio 设备 */
    virtio_mmio_init(dev, vm, VIRTIO_ID_NET, 2, vqs, &virtio_net_ops,
                     VIRQ_VIRTIO_NET);

    dev->priv = net_priv;

    /* 设置 host 特性 */
    dev->host_features[0] = VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS;
    dev->host_features[1] = 0;
}
