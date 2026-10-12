// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - virtio MMIO 通用框架 (Phase 4)
 *
 * 遵循 virtio 1.0 MMIO 规范 (legacy 模式, 与 QEMU virtio-mmio 兼容):
 *   - MagicValue   = 0x74726976 ("virt" 小端)
 *   - Version      = 1 (legacy)
 *   - DeviceID     = 设备类型 (1=net, 2=block, ...)
 *   - VendorID     = 0x554d4551 ("QEMU")
 *   - HostFeatures / GuestFeatures : 特性协商
 *   - QueuePFN / QueueNum / QueueSel : virtqueue 配置
 *   - QueueNotify  : 通知设备有新的描述符
 *   - InterruptStatus / InterruptACK : 设备中断状态
 *   - Status       : 设备状态寄存器
 *
 * 学习要点：
 *   virtio 设备模型的核心抽象：
 *     1. MMIO 寄存器 (控制面) - guest 通过 MMIO 读写配置设备
 *     2. virtqueue (数据面) - 共享内存中的环形缓冲区, 用于批量数据传输
 *     3. 中断注入 - 设备有事件时注入虚拟 SPI 中断通知 guest
 */

#pragma once

#include "common/types.h"
#include "common/sched.h"

/* ---------- virtio MMIO 寄存器偏移 (legacy, 32-bit 访问) ---------- */
#define VIRTIO_MMIO_MAGIC_VALUE        0x000  /* MagicValue (RO) */
#define VIRTIO_MMIO_VERSION            0x004  /* Version (RO) */
#define VIRTIO_MMIO_DEVICE_ID          0x008  /* Device ID (RO) */
#define VIRTIO_MMIO_VENDOR_ID          0x00C  /* Vendor ID (RO) */
#define VIRTIO_MMIO_HOST_FEATURES      0x010  /* HostFeatures (RO) */
#define VIRTIO_MMIO_HOST_FEATURES_SEL  0x014  /* HostFeaturesSel (WO) */
#define VIRTIO_MMIO_GUEST_FEATURES     0x020  /* GuestFeatures (WO) */
#define VIRTIO_MMIO_GUEST_FEATURES_SEL 0x024  /* GuestFeaturesSel (WO) */
#define VIRTIO_MMIO_GUEST_PAGE_SIZE    0x028  /* GuestPageSize (WO, legacy) */
#define VIRTIO_MMIO_QUEUE_SEL          0x030  /* QueueSel (WO) */
#define VIRTIO_MMIO_QUEUE_NUM_MAX      0x034  /* QueueNumMax (RO) */
#define VIRTIO_MMIO_QUEUE_NUM          0x038  /* QueueNum (WO) */
#define VIRTIO_MMIO_QUEUE_ALIGN        0x03C  /* QueueAlign (WO, legacy) */
#define VIRTIO_MMIO_QUEUE_PFN          0x040  /* QueuePFN (RW, legacy) */
#define VIRTIO_MMIO_QUEUE_NOTIFY       0x050  /* QueueNotify (WO) */
#define VIRTIO_MMIO_INTERRUPT_STATUS   0x060  /* InterruptStatus (RO) */
#define VIRTIO_MMIO_INTERRUPT_ACK      0x064  /* InterruptACK (WO) */
#define VIRTIO_MMIO_STATUS             0x070  /* Status (RW) */
#define VIRTIO_MMIO_DEVICE_CONFIG      0x100  /* Device-specific config */

/* virtio MMIO 规范值 */
#define VIRTIO_MMIO_MAGIC_VAL   0x74726976UL  /* "virt" */
#define VIRTIO_MMIO_VER_VAL     1             /* legacy MMIO */
#define VIRTIO_MMIO_VENDOR_VAL  0x554d4551UL  /* "QEMU" */

/* 设备 ID */
#define VIRTIO_ID_NET           1   /* Network device */
#define VIRTIO_ID_BLOCK         2   /* Block device */

/* 设备状态位 */
#define VIRTIO_STATUS_ACK       (1 << 0)
#define VIRTIO_STATUS_DRIVER    (1 << 1)
#define VIRTIO_STATUS_DRIVER_OK (1 << 2)
#define VIRTIO_STATUS_FEATURES_OK (1 << 3)
#define VIRTIO_STATUS_FAILED    (1 << 7)

/* 中断状态位 */
#define VIRTIO_IRQ_USED_BUFFER  (1 << 0)  /* used buffer notification */
#define VIRTIO_IRQ_CONFIG_CHANGE (1 << 1) /* config change */

/* ---------- virtqueue 描述符 (legacy 布局) ---------- */
/*
 * 描述符表: 每 16 字节一个描述符
 *   struct vring_desc {
 *       u64 addr;    // guest physical address
 *       u32 len;     // length
 *       u16 flags;   // VRING_DESC_F_*
 *       u16 next;    // next descriptor index (if chain)
 *   };
 * 可用环 (avail ring):
 *   u16 flags, idx;
 *   u16 ring[num];
 *   u16 used_event;  (if VIRTIO_RING_F_EVENT_IDX)
 * 已用环 (used ring):
 *   u16 flags, idx;
 *   struct { u32 id; u32 len; } ring[num];
 *   u16 avail_event;
 */
#define VRING_DESC_F_NEXT       1
#define VRING_DESC_F_WRITE      2
#define VRING_DESC_F_INDIRECT   4

#define VIRTQUEUE_MAX_SIZE      256  /* 单个队列最大描述符数 */

/* ---------- 通用 virtio 设备结构 ---------- */

struct virtio_mmio_dev;

/* 设备操作函数表 */
struct virtio_dev_ops {
    /* 读取设备特定配置寄存器 (offset 相对于 DEVICE_CONFIG) */
    uint32_t (*read_config)(struct virtio_mmio_dev *dev, uint32_t offset);
    /* 写入设备特定配置寄存器 */
    void (*write_config)(struct virtio_mmio_dev *dev, uint32_t offset, uint32_t val);
    /* Guest 通知队列有新描述符 */
    void (*queue_notify)(struct virtio_mmio_dev *dev, uint32_t qidx);
    /* 设备状态变更 (DRIVER_OK 等) */
    void (*status_change)(struct virtio_mmio_dev *dev, uint8_t old_status, uint8_t new_status);
};

/* 单个 virtqueue */
struct virtqueue {
    uint32_t num;            /* 队列大小 (描述符数) */
    uint32_t pfn;            /* 队列页帧号 (guest IPA / PAGE_SIZE) */
    uint32_t align;          /* 对齐 (legacy) */
    int      ready;          /* 队列是否就绪 (pfn != 0) */
    uint16_t last_avail_idx; /* 上次处理到的 avail 索引 */
};

/* virtio MMIO 设备基类 */
struct virtio_mmio_dev {
    uint32_t dev_id;             /* 设备类型 ID */
    uint32_t host_features[2];   /* host 特性 (0=低32位, 1=高32位) */
    uint32_t guest_features[2];  /* guest 协商后的特性 */
    uint32_t host_features_sel;  /* 当前选中的 host features 字 */
    uint32_t guest_features_sel; /* 当前选中的 guest features 字 */
    uint32_t guest_page_size;    /* guest 页大小 (legacy) */
    uint8_t  status;             /* 设备状态 */
    uint32_t irq_status;         /* 中断状态位 */
    uint32_t virq;               /* 注入的虚拟中断号 */

    /* virtqueues */
    int num_queues;
    int queue_sel;              /* 当前选中的队列索引 */
    struct virtqueue *vqs;

    /* 设备私有数据 */
    void *priv;

    /* 设备操作 */
    const struct virtio_dev_ops *ops;

    /* 所属 VM */
    struct task_struct *vm;
};

/* ---------- 通用 virtio MMIO 接口 ---------- */

/*
 * 初始化一个 virtio MMIO 设备
 *   dev: 设备结构体 (调用者分配)
 *   vm:  所属 VM
 *   dev_id: 设备类型 ID
 *   num_queues: virtqueue 数量
 *   vqs: 预分配的 virtqueue 数组
 *   ops: 设备操作函数表
 *   virq: 虚拟中断号
 */
void virtio_mmio_init(struct virtio_mmio_dev *dev,
                      struct task_struct *vm,
                      uint32_t dev_id,
                      int num_queues,
                      struct virtqueue *vqs,
                      const struct virtio_dev_ops *ops,
                      uint32_t virq);

/*
 * MMIO 读操作分派 (由 board 的 mmio_read 调用)
 *   返回读取的值
 */
uint32_t virtio_mmio_read(struct virtio_mmio_dev *dev, uint32_t offset);

/*
 * MMIO 写操作分派 (由 board 的 mmio_write 调用)
 */
void virtio_mmio_write(struct virtio_mmio_dev *dev, uint32_t offset, uint32_t val);

/*
 * 注入虚拟中断: 设置 irq_status 位 + 注入 vIRQ
 */
void virtio_mmio_inject_irq(struct virtio_mmio_dev *dev, uint32_t irq_bits);

/* ---------- virtqueue 操作 ---------- */

/*
 * 获取当前 avail 环中是否有新的描述符链
 * 返回 1 有, 0 无
 */
int vq_has_avail(struct virtio_mmio_dev *dev, struct virtqueue *vq);

/*
 * 从 avail 环取下一个描述符链头索引
 * 返回描述符索引, -1 为空
 */
int vq_pop_avail(struct virtio_mmio_dev *dev, struct virtqueue *vq);

/*
 * 读取描述符 (支持链)
 *   desc_idx: 描述符索引
 *   addr_out: 输出 guest IPA 地址
 *   len_out:  输出长度
 *   flags_out: 输出 flags
 *   next_out:  输出下一个描述符索引 (若有)
 * 返回 0 成功
 */
int vq_read_desc(struct virtio_mmio_dev *dev, struct virtqueue *vq,
                 uint16_t desc_idx,
                 uint64_t *addr_out, uint32_t *len_out,
                 uint16_t *flags_out, uint16_t *next_out);

/*
 * 向 used 环添加一个已处理的描述符
 *   desc_idx: 描述符链头索引
 *   len: 已写入长度 (对 device-readable 描述符, 可以是 0)
 */
void vq_add_used(struct virtio_mmio_dev *dev, struct virtqueue *vq,
                 uint16_t desc_idx, uint32_t len);

/*
 * 从 guest IPA 读取数据 (阶段 2 翻译后读物理内存)
 * 返回读取的字节数
 */
int vq_copy_from_guest(struct virtio_mmio_dev *dev,
                       uint64_t guest_ipa, void *dst, int len);

/*
 * 向 guest IPA 写入数据
 * 返回写入的字节数
 */
int vq_copy_to_guest(struct virtio_mmio_dev *dev,
                     uint64_t guest_ipa, const void *src, int len);
