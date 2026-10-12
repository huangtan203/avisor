# aVisor Hypervisor Phase 4 详细设计文档 (设备虚拟化)

> 版本：v0.4.0 (Phase 4)
> 平台：QEMU virt (ARMv8-A, GICv3, 虚拟化扩展)
> 目标：MMIO 陷获框架、virtio 设备模型、VM 间共享内存通信

---

## 1. 概述

### 1.1 目标达成情况

| 目标 | 状态 | 实现方式 |
|------|------|---------|
| MMIO 陷获框架完善 | ✅ | translation fault + permission fault 统一分派，is_mmio_addr 判断，demand paging 与设备模拟分流 |
| 虚拟 SPI 中断注入（纯软件） | ✅ | vGIC List Register 多 LR 管理（LR0 留 vtimer HW 映射，LR1+ 纯软件注入） |
| virtio 设备模型框架 | ✅ | virtio MMIO 通用寄存器模拟 + virtqueue 操作抽象 + 设备回调 ops |
| virtio-net 设备模型 | ✅ | 环回模式，2 队列（rx/tx），MAC/STATUS 特性 |
| Guest 端 virtio-net 驱动 | ✅ | legacy MMIO 接口驱动，vring 初始化，tx/rx 环回测试 |
| VM 间通信（ivshmem 风格） | ✅ | 共享内存 + doorbell 寄存器 + 虚拟中断注入（当前 VM/非当前 VM 两种路径） |

### 1.2 验证结果

**ivshmem 功能验证通过：**
```
[vm1] ivshmem: revision=0x01000000 position=0
[vm1] ivshmem: vm1 wrote to shared memory, sending doorbell to peer 1
...
[vm2] ivshmem: revision=0x01000000 position=1
[vm2] ivshmem: vm2 received doorbell!
[vm2] ivshmem: shmem[0] = 0xdeadbeef shmem[1] = 0x12345678
[vm2] ivshmem: vm2 replying to peer 0
```

- 共享内存读写正常
- doorbell 触发虚拟中断正常（延迟注入：目标 VM 不在运行时写入保存的 LR，下次切换时自动加载）
- 两 VM peer_id 正确（VM1=0, VM2=1）

**virtio-net 功能部分验证：**
- 设备探测通过（magic/version/devid/vendor 正确）
- 特性协商通过（MAC + STATUS）
- vring 配置成功（pfn/num/align 正确设置）
- 队列 notify 可触发设备端回调
- ⚠️ 环回测试存在 bug：tx 数据包描述符读取异常，待修复

**vGIC 多 LR 验证：**
- 虚拟定时器（PPI 27）+ 虚拟 SPI 中断可并存
- 世界切换时 LR 状态正确保存恢复

### 1.3 修改的关键文件

```
avisor/
├── include/
│   ├── common/
│   │   ├── sched.h            # ★ vm_virt_state: gic_lr0 改为 gic_lr[NR_VGIC_LRS] 数组
│   │   └── board.h            # ★ 新增 is_mmio_addr 回调
│   └── boards/virt/
│       ├── vgic.h             # ★ 新增 API: vgic_inject_virq / vgic_save_state / vgic_restore_state
│       ├── virtio_mmio.h      # ★ 新增：virtio MMIO 定义、vring 结构、设备 ops、virtqueue API
│       └── ivshmem.h          # ★ 新增：ivshmem 设备结构与 API
├── hypervisor/
│   ├── common/
│   │   └── mm.c               # ★ handle_mem_abort 重构：MMIO 模拟与 demand paging 分流
│   ├── arch/aarch64/
│   │   ├── sysregs.h          # ★ 新增 ICH_LR1_EL2 定义、NR_VGIC_LRS
│   │   └── task.c             # ★ create_task 中 pid 赋值移到 board_ops->initialize 之前
│   └── boards/virt/
│       ├── virt_board.c       # ★ 完整重写：MMIO 分派、virtio-net 设备、ivshmem、世界切换
│       ├── vgic.c             # ★ 重写：多 LR 管理、纯软件注入、状态保存/恢复
│       ├── virtio_mmio.c      # ★ 新增：virtio MMIO 通用实现 + virtqueue 操作 + stage 2 内存访问
│       ├── virtio_net.c       # ★ 新增：独立 virtio-net 设备实现（环回模式）
│       └── ivshmem.c          # ★ 新增：ivshmem 共享内存设备实现
└── guests/el1_test/
    ├── guest.h                # ★ 新增 u8/u16/bool 类型定义、guest_fatal 声明
    └── main.c                 # ★ 完整重写：virtio-net 驱动 + ivshmem 驱动 + ping-pong 测试
```

---

## 2. 总体架构

### 2.1 Phase 4 系统视图

```
                    ┌──────────────────────────────────────────────────┐
                    │           aVisor Hypervisor (EL2)                │
                    │                                                  │
                    │  ┌────────────────────────────────────────────┐  │
                    │  │         MMIO Trap Framework               │  │
                    │  │                                            │  │
                    │  │  handle_mem_abort()                        │  │
                    │  │    ├─ permission fault → 一定是 MMIO       │  │
                    │  │    └─ translation fault                    │  │
                    │  │         ├─ is_mmio_addr()? → 设备模拟     │  │
                    │  │         └─ 否 → demand paging             │  │
                    │  └────────────────────────────────────────────┘  │
                    │                       │                          │
                    │                       ▼                          │
                    │  ┌────────────────────────────────────────────┐  │
                    │  │         Device Emulation Layer             │  │
                    │  │                                            │  │
                    │  │  virt_mmio_read / virt_mmio_write          │  │
                    │  │    ├─ virtio-net MMIO (0x0A000000)        │  │
                    │  │    └─ ivshmem regs   (0x0A001000)        │  │
                    │  └────────────────────────────────────────────┘  │
                    │                       │                          │
                    │           ┌───────────┴───────────┐              │
                    │           ▼                       ▼              │
                    │  ┌──────────────────┐   ┌──────────────────┐    │
                    │  │  virtio-net dev  │   │   ivshmem dev    │    │
                    │  │  (per-VM)        │   │  (per-VM, shmem  │    │
                    │  │  2 queues (rx/tx)│   │   shared)         │    │
                    │  │  loopback mode   │   │  doorbell → virq  │    │
                    │  └──────────────────┘   └──────────────────┘    │
                    │           │                       │              │
                    │           └───────────┬───────────┘              │
                    │                       ▼                          │
                    │  ┌────────────────────────────────────────────┐  │
                    │  │           vGIC (Virtual GIC)               │  │
                    │  │                                            │  │
                    │  │  LR0: vtimer (HW 映射)                     │  │
                    │  │  LR1: virtio-net SPI 48 (纯软件注入)       │  │
                    │  │  LR1: ivshmem SPI 49 (纯软件注入)          │  │
                    │  │  (LR 按可用性动态分配)                     │  │
                    │  └────────────────────────────────────────────┘  │
                    └──────────────────────────────────────────────────┘
```

### 2.2 虚拟设备地址空间

| 起始地址 | 大小 | 设备 | 说明 |
|---------|------|------|------|
| 0x0A000000 | 4KB | virtio-net MMIO | virtio 1.0 legacy MMIO 接口 |
| 0x0A001000 | 4KB | ivshmem 控制寄存器 | Revision / Position / Doorbell |
| 0x0B000000 | 1MB | ivshmem 共享内存 | 所有 VM 共享同一块物理内存 |

### 2.3 虚拟中断分配

| 中断号 | 类型 | 所属设备 | 注入方式 |
|-------|------|---------|---------|
| 27 | PPI | 虚拟定时器 | HW 映射 (LR0) |
| 48 | SPI | virtio-net | 纯软件注入 (LR1+) |
| 49 | SPI | ivshmem doorbell | 纯软件注入 (LR1+) |

---

## 3. MMIO 陷获框架

### 3.1 设计思路

Guest 访问设备寄存器需要陷入 Hypervisor 进行模拟。有两种触发方式：

1. **Permission Fault（权限异常）**：Stage 2 映射了页面但设置为不可访问（MMIO 区域），Guest 访问触发权限异常。
2. **Translation Fault（翻译异常）**：Stage 2 根本没有映射该地址，Guest 访问触发翻译异常。

Phase 4 采用 **Translation Fault + is_mmio_addr 判断** 的方案：
- MMIO 区域不预先映射到 Stage 2
- Guest 访问时触发 translation fault
- `handle_mem_abort` 调用 `board_ops->is_mmio_addr()` 判断是否为 MMIO 区域
  - 是 → 调用 `board_ops->mmio_read/mmio_write` 模拟
  - 否 → demand paging 分配普通内存页

### 3.2 为什么选择 Translation Fault 方案

| 方案 | 优点 | 缺点 |
|------|------|------|
| Permission Fault | 区分明确，不需要软件判断 | 需要预先映射所有 MMIO 页面（只读无效页表），浪费页表内存 |
| Translation Fault | 无需预映射，节省页表内存 | 需要软件判断是否为 MMIO（每次 translation fault 都要走判断逻辑） |

对于少量虚拟设备（2-3 个），translation fault 方案更简洁，且节省页表内存。

### 3.3 核心代码流程

```
handle_mem_abort(addr, esr)
  │
  ├─ dfsc = esr & 0x3F
  ├─ ipa = get_ipa(addr)
  │
  ├─ is_mmio = 0
  ├─ if dfsc[3:2] == 0x3 (permission fault)
  │    is_mmio = 1
  ├─ else if dfsc[3:2] == 0x1 (translation fault)
  │    if board_ops.is_mmio_addr(current, ipa)
  │      is_mmio = 1
  │
  ├─ if is_mmio
  │    ├─ wnr = esr[6] (写标志)
  │    ├─ srt = esr[20:16] (目标寄存器号)
  │    ├─ if 读: regs[srt] = board_ops.mmio_read(current, ipa)
  │    └─ if 写: board_ops.mmio_write(current, ipa, regs[srt])
  │    └─ increment_current_pc(4) (跳过 MMIO 指令)
  │
  └─ else
       └─ demand paging: 分配页面 → map_stage2_page
```

### 3.4 新增 board_ops 回调

```c
struct board_ops {
    // ... 已有回调 ...
    int (*is_mmio_addr)(struct task_struct *tsk, uint64_t ipa);
};
```

`is_mmio_addr` 判断给定 IPA 是否在虚拟设备 MMIO 区域内。virt 平台实现为 `virt_is_mmio_addr`，检查地址是否落在 virtio-net / ivshmem 寄存器区间。

---

## 4. vGIC 扩展：虚拟 SPI 中断注入

### 4.1 背景与挑战

Phase 3 中 vGIC 只支持虚拟定时器中断（PPI 27），通过 HW 映射模式（ICH_LR_HW）注入，一个 List Register (LR0) 就够了。

Phase 4 需要支持多个虚拟 SPI 中断（virtio-net=48, ivshmem=49），这些是纯软件注入的中断，需要：
1. 多个 LR 支持
2. 动态分配/释放 LR
3. 当前 VM 不在运行时的延迟注入
4. 世界切换时 LR 状态的保存/恢复

### 4.2 List Register 管理策略

- **LR0**：预留给虚拟定时器（HW 映射模式），不参与动态分配
- **LR1 ~ LRn**：用于纯软件注入的虚拟中断，按可用性动态分配
- **NR_VGIC_LRS = 2**：当前配置 2 个 LR（足够支持 vtimer + 2 个 SPI）

### 4.3 注入逻辑

```c
void vgic_inject_virq(uint32_t vintid)
{
    // 找一个空闲的 LR（从 1 开始，LR0 留 vtimer）
    for (i = 1; i < NR_VGIC_LRS; i++) {
        if (LR[i] 状态 == invalid) {
            LR[i] = vintid | PENDING | GROUP1;
            return;
        }
    }
    // 无空闲 LR → 丢弃（或挂起等待）
}
```

**Guest EOI 处理**：
- 纯软件模式（HW=0）下，Guest 写 ICC_EOIR1_EL1 后，对应 LR 自动变为 invalid 状态
- Hypervisor 不需要主动清理

### 4.4 延迟注入（目标 VM 不在运行）

当向非当前 VM 注入中断时，不能直接写硬件 LR，而是修改该 VM 保存的虚拟化状态中的 `gic_lr[]` 数组：

```c
// ivshmem doorbell 注入逻辑
if (target->vm == current) {
    vgic_inject_virq(VIRQ_IVSHMEM);  // 直接写硬件 LR
} else {
    // 修改目标 VM 保存的 LR 数组
    for (i = 1; i < NR_VGIC_LRS; i++) {
        if ((virt->gic_lr[i] & ICH_LR_STATE_MASK) == 0) {
            virt->gic_lr[i] = VIRQ_IVSHMEM | ICH_LR_GROUP1 | ICH_LR_PENDING;
            break;
        }
    }
}
```

下次切换到目标 VM 时，`vgic_restore_state` 会把 `gic_lr[]` 数组加载到硬件 LR 中，Guest 立即看到中断。

### 4.5 状态保存与恢复

世界切换时，保存/恢复所有 LR + VMCR：

```c
void vgic_save_state(struct vm_virt_state *state)
{
    state->gic_vmcr = READ_SYSREG64(ICH_VMCR_EL2);
    state->gic_lr[0] = READ_SYSREG64(ICH_LR0_EL2);
    state->gic_lr[1] = READ_SYSREG64(ICH_LR1_EL2);
    // ... 更多 LR ...
}

void vgic_restore_state(struct vm_virt_state *state)
{
    WRITE_SYSREG64(state->gic_vmcr, ICH_VMCR_EL2);
    WRITE_SYSREG64(state->gic_lr[0], ICH_LR0_EL2);
    WRITE_SYSREG64(state->gic_lr[1], ICH_LR1_EL2);
    // ... 更多 LR ...
}
```

---

## 5. virtio 设备模型框架

### 5.1 virtio MMIO 寄存器布局（Legacy 接口）

| 偏移 | 寄存器 | 宽度 | 读写 | 说明 |
|------|--------|------|------|------|
| 0x000 | MagicValue | 32 | R | 0x74726976 ("virt" 小端) |
| 0x004 | Version | 32 | R | 1 (legacy) |
| 0x008 | DeviceID | 32 | R | 设备类型 (1=net, 2=block...) |
| 0x00C | VendorID | 32 | R | 0x554D4551 |
| 0x010 | HostFeatures | 32 | R | Host 支持的特性（按 HostFeaturesSel 分页） |
| 0x014 | HostFeaturesSel | 32 | W | Host 特性页选择 |
| 0x020 | GuestFeatures | 32 | W | Guest 接受的特性 |
| 0x024 | GuestFeaturesSel | 32 | W | Guest 特性页选择 |
| 0x028 | GuestPageSize | 32 | W | Guest 页大小（legacy 模式） |
| 0x030 | QueueSel | 32 | W | 当前操作的队列号 |
| 0x034 | QueueNumMax | 32 | R | 队列最大描述符数 |
| 0x038 | QueueNum | 32 | W | Guest 设置的队列大小 |
| 0x040 | QueuePFN | 32 | RW | 队列页帧号（GuestPageSize 单位） |
| 0x044 | QueueAlign | 32 | W | vring 对齐字节数 |
| 0x050 | QueueNotify | 32 | W | 通知设备：队列有新的 avail 描述符 |
| 0x060 | InterruptStatus | 32 | R | 中断状态 (bit0=used buffer, bit1=config change) |
| 0x064 | InterruptAck | 32 | W | 中断应答（清除对应位） |
| 0x070 | Status | 32 | RW | 设备状态寄存器 |
| 0x100+ | DeviceConfig | 32 | RW | 设备特定配置空间 |

### 5.2 核心数据结构

```c
// virtio 设备回调（具体设备实现）
struct virtio_dev_ops {
    uint32_t (*read_config)(struct virtio_mmio_dev *dev, uint32_t offset);
    void (*write_config)(struct virtio_mmio_dev *dev, uint32_t offset, uint32_t val);
    void (*queue_notify)(struct virtio_mmio_dev *dev, uint32_t qidx);
    void (*status_change)(struct virtio_mmio_dev *dev, uint8_t old_status, uint8_t new_status);
};

// virtqueue
struct virtqueue {
    uint32_t pfn;       // 队列页帧号
    uint32_t num;       // 描述符数量
    uint32_t align;     // vring 对齐
    int      ready;     // 队列是否就绪
    uint16_t last_avail_idx;  // 上次处理的 avail index
};

// virtio MMIO 设备通用结构
struct virtio_mmio_dev {
    struct task_struct *vm;
    uint32_t dev_id;
    uint32_t virq;
    int      num_queues;
    struct virtqueue *vqs;
    const struct virtio_dev_ops *ops;
    void *priv;         // 设备私有数据
    // ... 寄存器状态 ...
};
```

### 5.3 virtqueue 内存布局（Legacy）

```
  Page N                    Page N+1                    Page N+2
┌───────────────┐  ┌───────────────────────┐  ┌───────────────────────┐
│  Desc Table   │  │    Avail Ring         │  │    Used Ring          │
│  (16*num B)   │  │  flags (2B)           │  │  flags (2B)           │
│               │  │  idx (2B)             │  │  idx (2B)             │
│ desc[0]       │  │  ring[0] (2B)         │  │  ring[0].id (4B)     │
│   .addr (8B)  │  │  ring[1] (2B)         │  │  ring[0].len (4B)    │
│   .len  (4B)  │  │  ...                  │  │  ring[1].id (4B)     │
│   .flags(2B)  │  │  used_event (2B)      │  │  ring[1].len (4B)    │
│   .next (2B)  │  │                       │  │  ...                  │
│ desc[1]       │  │                       │  │  avail_event (4B)    │
│ ...           │  │                       │  │                       │
└───────────────┘  └───────────────────────┘  └───────────────────────┘
   align 16          align = QueueAlign           align = QueueAlign
```

- **Desc Table**：描述符表，每个描述符 16 字节（addr + len + flags + next）
- **Avail Ring**：可用环，Guest 提交描述符链（设备读，Guest 写）
- **Used Ring**：已用环，设备返回处理完的描述符（设备写，Guest 读）

### 5.4 Guest 内存访问（Stage 2 翻译）

Hypervisor 访问 Guest 的 vring 内存需要通过 Stage 2 页表翻译。`ipa_to_hv_va` 函数遍历 Guest 的 Stage 2 页表，将 IPA 转换为 Hypervisor 的虚拟地址（等值映射）：

```c
static void *ipa_to_hv_va(struct task_struct *vm, uint64_t ipa, int *writable)
{
    // Level 1
    lv1_idx = (ipa >> LV1_SHIFT) & (PTRS_PER_TABLE - 1);
    if (!(lv1[lv1_idx] & 1)) return NULL;

    // Level 2
    lv2_idx = (ipa >> LV2_SHIFT) & (PTRS_PER_TABLE - 1);
    if (!(lv2[lv2_idx] & 1)) return NULL;

    // Level 3
    lv3_idx = (ipa >> PAGE_SHIFT) & (PTRS_PER_TABLE - 1);
    if (!(lv3[lv3_idx] & 1)) return NULL;

    pa = (lv3[lv3_idx] & PAGE_MASK) | (ipa & (PAGE_SIZE - 1));
    return (void *)TO_VADDR(pa);
}
```

基于此封装了 `vq_copy_from_guest` / `vq_copy_to_guest` 函数，支持跨页拷贝。

### 5.5 virtqueue 操作函数

| 函数 | 作用 |
|------|------|
| `vq_has_avail(dev, vq)` | 检查队列是否有新的可用描述符（avail->idx != last_avail_idx） |
| `vq_pop_avail(dev, vq)` | 弹出一个可用描述符链的头索引 |
| `vq_read_desc(dev, vq, idx, ...)` | 读取指定索引的描述符 |
| `vq_add_used(dev, vq, idx, len)` | 将处理完的描述符加入 used 环 |
| `vq_copy_from_guest(dev, ipa, dst, len)` | 从 Guest 内存拷贝数据到 Hypervisor |
| `vq_copy_to_guest(dev, ipa, src, len)` | 从 Hypervisor 拷贝数据到 Guest 内存 |

---

## 6. virtio-net 设备实现

### 6.1 设备特性

- **Device ID**: 1 (VIRTIO_ID_NET)
- **支持的特性**:
  - VIRTIO_NET_F_MAC (bit 5)：提供 MAC 地址
  - VIRTIO_NET_F_STATUS (bit 16)：提供链路状态
- **队列数**: 2（rx = 队列 0，tx = 队列 1）
- **工作模式**: 环回（Loopback）—— tx 队列的数据包自动回送到 rx 队列

### 6.2 设备配置空间

| 偏移 | 字段 | 大小 | 说明 |
|------|------|------|------|
| 0x00 | mac[0:5] | 6B | MAC 地址 |
| 0x06 | status | 2B | 链路状态 (1 = link up) |
| 0x08 | max_virtqueue_pairs | 2B | 最大队列对数 |

### 6.3 环回处理流程

```
Guest 写 tx 描述符 → 写 avail 环 → 写 QueueNotify
          │
          ▼
Hypervisor 收到 MMIO 写（QueueNotify）
          │
          ▼
virtio_net_queue_notify()
  ├─ 遍历 tx 队列的 avail 描述符
  ├─ read_tx_packet(): 从 tx 描述符链读取数据包内容
  ├─ vq_add_used(txq, desc_idx, 0): 标记 tx 描述符为已用
  └─ write_rx_packet(): 将数据包写入 rx 队列的空缓冲区
       ├─ 从 rx 队列弹出一个 avail 描述符
       ├─ 将数据包拷贝到 Guest 的 rx 缓冲区
       └─ vq_add_used(rxq, desc_idx, pkt_len)
          └─ 注入 virtio-net 虚拟中断 (SPI 48)
```

### 6.4 中断注入

数据包处理完成后，调用 `virtio_mmio_inject_irq(dev, VIRTIO_IRQ_USED_BUFFER)`：
1. 设置 `dev->irq_status |= VIRTIO_IRQ_USED_BUFFER`
2. 调用 `vgic_inject_virq(dev->virq)` 注入虚拟 SPI 中断

Guest 读取 InterruptStatus 寄存器可获得中断原因，写 InterruptAck 清除对应位。

---

## 7. ivshmem 共享内存设备

### 7.1 设计思路

ivshmem 是 QEMU 中的一种共享内存设备，核心思想：
- 多个 VM 共享同一块物理内存
- 通过 doorbell 寄存器触发对端中断
- 实现 VM 间的低延迟通信

Phase 4 实现了一个简化版 ivshmem：
- 2 个 peer（对应 2 个 VM）
- 1MB 共享内存（从页池分配连续物理页）
- 每个 VM 有自己的控制寄存器视图（IVPOSITION 不同）
- 写 doorbell 寄存器 → 向目标 peer 注入虚拟中断

### 7.2 控制寄存器布局

| 偏移 | 寄存器 | 读写 | 说明 |
|------|--------|------|------|
| 0x00 | RevisionID | R | 0x01000000 (v1.0, legacy 格式) |
| 0x04 | IVPosition | R | 当前 VM 的 peer ID (0 或 1) |
| 0x0C | Doorbell | W | 低 16 位 = 目标 peer ID |

### 7.3 共享内存映射

```
物理内存：
  ┌──────────────────────────────┐
  │   Shared Memory (1MB)        │  ← 从页池分配的连续物理页
  │   (PA: 0x41002000)           │
  └──────────────────────────────┘
            │          │
     Stage 2映射    Stage 2映射
            │          │
            ▼          ▼
  VM1 IPA: 0x0B000000  VM2 IPA: 0x0B000000
```

两个 VM 看到的共享内存 IPA 地址相同（0x0B000000），但都映射到同一块物理内存。一个 VM 写入，另一个 VM 立即可见。

### 7.4 Doorbell 中断注入

Guest 写 Doorbell 寄存器，指定目标 peer ID：

```c
void ivshmem_mmio_write(struct ivshmem_dev *dev, uint32_t offset, uint32_t val)
{
    if (offset != IVSHMEM_DOORBELL) return;

    int target_peer = val & 0xFFFF;
    struct ivshmem_dev *target = ivshmem_peers[target_peer];

    if (target->vm == current) {
        // 目标就是当前 VM → 直接注入硬件 LR
        vgic_inject_virq(VIRQ_IVSHMEM);
    } else {
        // 目标不是当前 VM → 修改其保存的 LR（延迟注入）
        struct vm_virt_state *virt = &target->vm->virt;
        for (int i = 1; i < NR_VGIC_LRS; i++) {
            if ((virt->gic_lr[i] & ICH_LR_STATE_MASK) == 0) {
                virt->gic_lr[i] = VIRQ_IVSHMEM | ICH_LR_GROUP1 | ICH_LR_PENDING;
                break;
            }
        }
    }
}
```

**延迟注入原理**：目标 VM 不在运行时，其硬件 LR 状态保存在 `task_struct.virt.gic_lr[]` 数组中。直接修改这个数组，下次世界切换（vgic_restore_state）时会加载到硬件 LR，Guest 进入后立即看到中断。

---

## 8. Guest 端驱动

### 8.1 virtio-net 驱动

实现了 minimal 的 virtio legacy MMIO 驱动，用于验证设备模型：

1. **设备探测**：读 MagicValue / Version / DeviceID / VendorID
2. **特性协商**：读 HostFeatures，写 GuestFeatures（MAC + STATUS）
3. **vring 初始化**：
   - 分配 vring 内存（从 guest heap 分配）
   - 设置 QueueNum / QueueAlign / QueuePFN
   - 向 rx 队列预填充空缓冲区
4. **发送数据包**：
   - 获取空闲 tx 描述符
   - 填充描述符（addr = 数据包地址, len = 长度）
   - 添加到 avail 环
   - 写 QueueNotify 通知设备
5. **接收数据包**：
   - 轮询 used 环（或中断驱动）
   - 从 used 环取出已填充的 rx 描述符
   - 拷贝数据
   - 归还描述符到 avail 环

### 8.2 ivshmem 驱动

1. **设备探测**：读 RevisionID / IVPOSITION
2. **共享内存访问**：直接读写 0x0B000000 起始的内存区域
3. **发送 doorbell**：写 Doorbell 寄存器，指定目标 peer
4. **中断处理**：收到 ivshmem 中断后，读取共享内存数据

### 8.3 Ping-Pong 测试流程

```
VM1 (peer 0)                    VM2 (peer 1)
    │                              │
    │  shmem[0] = 0xdeadbeef      │
    │  shmem[1] = 0x12345678      │
    │  doorbell → peer 1          │
    ├─────────────────────────────▶│
    │                              │  收到 SPI 49 中断
    │                              │  读 shmem[0] = 0xdeadbeef
    │                              │  shmem[2] = 0xcafebabe
    │  收到 SPI 49 中断            │  doorbell → peer 0
    │◀─────────────────────────────┤
    │  读 shmem[2] = 0xcafebabe    │
    │  验证成功                    │
```

---

## 9. 构建与运行

### 9.1 构建

```bash
cd avisor
make -f Makefile.virt
```

构建产物：
- `bin_virt/kernel8.img` — Hypervisor 镜像（内嵌 Guest）
- `guests/el1_test/el1_test.bin` — Guest 镜像

### 9.2 运行

```bash
qemu-system-aarch64 \
    -M virt,virtualization=on,gic-version=3 \
    -cpu max \
    -smp 1 \
    -m 1G \
    -kernel bin_virt/kernel8.img \
    -nographic -serial mon:stdio
```

退出 QEMU: `Ctrl+A` 然后 `X`

### 9.3 预期输出

```
========================================
  aVisor Hypervisor (QEMU virt port)
========================================
...
INFO[1]: ivshmem: peer 0 initialized
INFO[2]: ivshmem: peer 1 initialized
...
[vm1] --- Test 1: virtio-net ---
[vm1] virtio-net: magic=0x74726976 version=1 devid=1 vendor=0x554d4551
[vm1] virtio-net: MAC=52:54:00:12:34:51
...
[vm1] --- Test 2: ivshmem ---
[vm1] ivshmem: position=0
...
[vm2] ivshmem: position=1
[vm2] ivshmem: vm2 received doorbell!
...
```

---

## 10. 已知问题与后续计划

### 10.1 已知问题

1. **virtio-net 环回测试 bug**：tx 数据包描述符读取异常（设备端读到全 0），导致环回测试失败。推测是 vring 内存的 Stage 2 映射或地址计算问题，需进一步调试。
2. **GIC Distributor 未模拟**：Guest 访问 GICD 寄存器会触发 translation fault → demand paging → 分配空页。目前绕过方法是 Guest 不操作 GICD，虚拟 SPI 中断由 vGIC LR 直接注入。
3. **ivshmem 测试时序问题**：VM1 启动后立即开始 ping-pong 测试，但 VM2 尚未启动，导致首次测试超时。两 VM 都运行后，doorbell 中断功能正常。
4. **LR 数量有限**：当前只有 2 个 LR，只能同时挂起 1 个虚拟 SPI 中断。中断频繁时可能丢失。

### 10.2 后续计划

- [ ] 修复 virtio-net 环回 bug
- [ ] 模拟 GIC Distributor 关键寄存器（ISENABLER / ICPENDR 等）
- [ ] 增加 LR 数量（支持更多虚拟中断）
- [ ] 实现 virtio-blk 设备
- [ ] ivshmem 支持更多 peer（动态注册/注销）
- [ ] 中断上下文切换优化（EOI 拆分模式下 active 状态管理）

---

## 附录 A：关键数据结构关系图

```
struct task_struct
├── pid, name, state, priority, counter, ...
├── cpu_context (x19-x28, sp, pc)
├── mm (first_table, user_pages_count, ...)
├── board_ops → &virt_board_ops
├── board_data → struct virt_vm_devices
│   ├── virtio_net (struct virtio_mmio_dev)
│   │   ├── vm → task_struct (回指)
│   │   ├── dev_id = VIRTIO_ID_NET
│   │   ├── virq = 48
│   │   ├── num_queues = 2
│   │   ├── vqs[0] = rx virtqueue
│   │   ├── vqs[1] = tx virtqueue
│   │   ├── ops → &virtio_net_ops
│   │   └── priv → struct virtio_net_dev (mac, link_status, ...)
│   ├── virtio_net_priv
│   └── ivshmem (struct ivshmem_dev)
│       ├── vm → task_struct (回指)
│       ├── peer_id = 0/1
│       ├── shmem_pa
│       └── shmem_size
├── virt (struct vm_virt_state)
│   ├── gic_vmcr
│   ├── gic_lr[NR_VGIC_LRS]  ← 保存所有 LR 状态
│   ├── cntv_cval
│   ├── cntv_ctl
│   └── vtimer_pending
└── ...
```
