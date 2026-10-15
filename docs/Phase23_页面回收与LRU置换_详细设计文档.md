# Phase 23：页面回收与 LRU 置换 — 详细设计文档

## 1. 概述

### 1.1 背景

方向 2：内存虚拟化增强。
- Phase 20：完善 Stage-2 页表管理（unmap + 页表自底向上回收）
- Phase 21：Stage-2 细粒度权限管理（mprotect + HVC_MEM_PROTECT）
- Phase 22：内存超售与按需分配优化（2MB 大页 + 内存配额）
- **Phase 23：页面回收与 LRU 置换（时钟算法 + Access Flag）**

Phase 22 引入了内存配额机制，支持内存超售。但当 VM 的内存使用达到配额上限时，新的分配会直接失败。这在实际生产环境中是不可接受的——我们需要一种机制，在内存紧张时**自动回收不常用的页面**，腾出空间给更需要的页面。

Phase 23 实现基于硬件 Access Flag 的**时钟算法（Clock Algorithm）** 页面回收：
- 利用 ARMv8 Stage-2 页表的 AF（Access Flag）位追踪页面访问
- 用时钟指针环形扫描页表，给每个页"第二次机会"
- AF=1 → 清 AF，继续前进（第二次机会）
- AF=0 → 回收该页（unmap + 释放物理页）

### 1.2 目标

- ✅ 支持 Stage-2 Access Flag Fault 处理（硬件 AF fault → 设置 AF 位）
- ✅ 实现时钟算法页面回收（reclaim_pages）
- ✅ 支持 2MB 大页的 AF 追踪与回收（自动拆分为小页）
- ✅ 集成回收到分配路径（quota_check 失败时先尝试回收）
- ✅ 支持 Instruction Abort（取指 AF fault）的处理
- ✅ 支持 S1PTW（Stage-1 页表遍历中的 Stage-2 fault）场景
- ✅ 新增 2 个 HVC 接口：RECLAIM_STATS / RECLAIM
- ✅ Guest 侧完整测试验证

### 1.3 非目标

- 不实现 LRU 链表（software LRU）—— 用硬件 AF + 时钟算法代替
- 不实现交换分区（Swapping）—— 只回收匿名页，不做磁盘换出
- 不实现工作集大小（WSS）估算 —— 留到后续优化
- 不实现 per-CPU 页缓存 —— 直接从全局页池分配/释放

---

## 2. 设计总览

### 2.1 架构图

```
┌──────────────────────────────────────────────────────────────────┐
│                        Guest VM (EL1)                           │
│  ┌──────────────────────┐  ┌──────────────────────────────────┐ │
│  │ HVC_MEM_RECLAIM      │  │ HVC_MEM_RECLAIM_STATS            │ │
│  │ (主动回收)           │  │ (查询统计)                       │ │
│  └──────────┬───────────┘  └──────────────┬───────────────────┘ │
└─────────────┼─────────────────────────────┼─────────────────────┘
              │ HVC #34                       │ HVC #29
┌─────────────┼─────────────────────────────┼─────────────────────┐
│  EL2 (Hypervisor)                            │                  │
│  ┌──────────▼───────────┐  ┌──────────────▼───────────────────┐ │
│  │ handle_hvc_reclaim   │  │ handle_hvc_reclaim_stats         │ │
│  └──────────┬───────────┘  └──────────────┬───────────────────┘ │
│             │                               │                   │
│  ┌──────────▼───────────────────────────────┐                   │
│  │           reclaim_pages()                │                   │
│  │         （时钟算法核心）                  │                   │
│  └──────────┬───────────────┬───────────────┘                   │
│             │               │                                   │
│  ┌──────────▼───┐   ┌──────▼─────────┐    ┌─────────────────┐  │
│  │  L1 扫描     │   │  L2 扫描       │    │  L3 扫描         │  │
│  │  (table AF)  │   │  (block/page)  │    │  (4KB page)      │  │
│  └──────────────┘   └──────┬─────────┘    └────────┬────────┘  │
│                            │                         │           │
│                   ┌────────▼─────────┐    ┌────────▼────────┐  │
│                   │ split_stage2_    │    │ unmap + free    │  │
│                   │ block (拆大页)    │    │ (回收物理页)     │  │
│                   └──────────────────┘    └─────────────────┘  │
│                                                                 │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │   Access Flag Fault 处理 (handle_access_flag_fault)       │  │
│  │   - 普通 Data/Instruction abort (S1PTW=0)                │  │
│  │   - Stage-1 页表遍历 fault (S1PTW=1, 用 HPFAR 取地址)     │  │
│  └───────────────────────────────────────────────────────────┘  │
│                                                                 │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │   分配路径集成                                             │  │
│  │   - handle_mem_abort (demand paging 配额不足 → 先回收)     │  │
│  │   - hyp_mem_map (显式分配配额不足 → 先回收)                │  │
│  └───────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────┘
```

### 2.2 核心原理：时钟算法（Clock Algorithm）

时钟算法是一种近似 LRU 的页面置换算法，用环形指针 + 使用位（Access Flag）模拟 LRU 行为：

```
        ┌───── clock_hand ────┐
        ▼                      │
    ┌───┬───┬───┬───┬───┬───┐  │
    │ P0│ P1│ P2│ P3│ P4│ P5│  │
    │AF=1│AF=0│AF=1│AF=1│AF=0│AF=1│
    └───┴───┴───┴───┴───┴───┘  │
      ▲                    │   │
      └──── next victim ◀──┘   │
                               │
        ◀──── 环形扫描 ────────┘
```

**算法步骤**：
1. 从 `clock_hand` 指向的页面开始扫描
2. 如果页面 AF=1（被访问过）：
   - 清除 AF 位（给第二次机会）
   - 时钟指针前进
3. 如果页面 AF=0（最近没被访问过）：
   - 回收该页面（unmap + free）
   - 时钟指针前进
   - 已回收数 +1
4. 达到目标回收数或扫描完一圈时停止

**优点**：
- O(1) 空间开销（不需要维护链表）
- 利用硬件 AF 位，不需要软件追踪
- 实现简单，性能好

### 2.3 新增数据结构

#### 2.3.1 mm_struct 扩展（include/common/sched.h）

```c
struct mm_struct {
    unsigned long first_table;
    int user_pages_count;       /* 4KB 用户页数量 */
    int kernel_pages_count;     /* 页表页数量 */
    int huge_pages_count;       /* 2MB 大页数量 */
    unsigned long max_memory;   /* 内存配额（字节） */
    int reclaim_count;          /* Phase 23: 累计回收页面数（4KB 粒度） */
    int reclaim_fail_count;     /* Phase 23: 回收失败次数 */
    unsigned long clock_hand;   /* Phase 23: 时钟算法指针（IPA，页对齐） */
    unsigned long cntvoff;
};
```

**字段说明**：
- `reclaim_count`：累计成功回收的 4KB 页面数（大页拆分后按 512 页计）
- `reclaim_fail_count`：主动回收请求但未达到目标的次数
- `clock_hand`：时钟指针，指向下一次扫描起始的 IPA（4KB 对齐，32-bit 环形回绕）

#### 2.3.2 新增 HVC 常量（include/common/hvc.h）

```c
#define HVC_MEM_RECLAIM_STATS 29  /* 查询回收统计：x0=回收页数 x1=回收失败次数 */
#define HVC_MEM_RECLAIM       34  /* 主动回收：x0=目标页数，返回实际回收页数 */
```

---

## 3. Access Flag Fault 处理

### 3.1 硬件机制

ARMv8 Stage-2 页表的每个 descriptor 都有一个 AF（Access Flag）位（bit 10）：
- 当 AF=0 时，首次访问该页面会触发 **Access Flag Fault**
- 软件设置 AF=1 后，后续访问不再触发 fault
- 硬件可以自动设置 AF 位（如果开启了 Hardware Access Flag Update）

对于页面回收：
- 回收算法主动清除 AF 位（标记为"未访问"）
- 之后如果页面被访问，触发 AF fault → 软件重新设置 AF=1
- 下次扫描时，AF=1 的页面有"第二次机会"

### 3.2 handle_access_flag_fault 实现

**位置**：`hypervisor/common/mm.c`

**支持的 entry 类型**：
1. **L2 block entry**（2MB 大页）
2. **L3 page entry**（4KB 小页）

**处理流程**：
```
1. 遍历 L1 → L2 → L3 页表，找到对应的 entry
2. 检查 entry 是否有效且 AF=0
3. 设置 AF 位（entry |= MM_STAGE2_ACCESS）
4. 刷新 TLB（tlbi vmalle1 + dsb + isb）
5. 返回 0（成功，Guest 重试访问）
```

### 3.3 Instruction Abort 支持

如果回收算法清除了代码页的 AF 位，Guest 取指时会触发 **Instruction Abort**（ESR_EL2.EC = 32），而不是 Data Abort。

**处理方式**：在 `sync_exc.c` 的 `ESR_EL2_EC_IABT_LOW` case 中复用 `handle_mem_abort` 处理逻辑。

### 3.4 S1PTW 场景的特殊处理

**问题**：当 Stage-2 fault 发生在 Stage-1 页表遍历过程中（S1PTW=1）：
- FAR_EL2 保存的是触发翻译的虚拟地址（指令/数据地址），**不是** Stage-2 fault 的真实地址
- 真实的 fault IPA 在 **HPFAR_EL2** 中

**HPFAR_EL2 格式**：
- Bit 0: F (Fault) — 0 = 有效，1 = 无效
- Bits [3:1]: RES0
- Bits [39:4]: IPA[39:12]（页帧号，右对齐到 bit 4）

**提取公式**：
```c
ipa = (hpfar & 0xFFFFFFFFF0ULL) << 8;
```

**修复方案**：在 `handle_mem_abort` 入口处判断 S1PTW 位：
```c
if (esr & (1 << 7)) {  // S1PTW
    uint64_t hpfar;
    asm volatile("mrs %0, hpfar_el2" : "=r"(hpfar));
    ipa = (hpfar & 0xFFFFFFFFF0ULL) << 8;
} else {
    ipa = get_ipa(addr);
}
```

### 3.5 QEMU 兼容性：AF fault 误报

**现象**：在 QEMU 模拟器中，某些场景下（特别是 S1PTW=1 时），Stage-2 Access Flag Fault 会被误报为 Translation Fault（FSC = 0b000101 而不是 0b001001）。

**容错处理**：对于 translation fault，先尝试按 AF fault 处理——如果 entry 存在但 AF=0，就设置 AF 位并刷新 TLB。

```c
// 正常 AF fault 路径
if ((dfsc >> 2) == 0x2) {
    if (handle_access_flag_fault(current, ipa) == 0)
        return 0;
}

// 容错：QEMU 误报为 translation fault 的 AF fault
if ((dfsc >> 2) == 0x1) {
    if (handle_access_flag_fault(current, ipa) == 0)
        return 0;
    // 确实是 translation fault，继续 demand paging
}
```

---

## 4. 大页拆分（split_stage2_block）

### 4.1 为什么需要拆分？

回收算法是按 4KB 粒度工作的。对于 2MB 大页：
- 如果整页都不活跃 → 可以整个回收（2MB）
- 但时钟算法是逐页扫描的，需要能单独处理 2MB block

**设计选择**：
- 回收时遇到 AF=0 的 L2 block → 直接回收整个 2MB（高效）
- 不需要预先拆分
- 但如果 block 的 AF=1，需要清 AF 位，下次扫描时判断是否回收

### 4.2 split_stage2_block 函数

虽然回收算法可以直接处理大页，但我们仍然实现了 `split_stage2_block` 函数，以备后续需要（如透明大页拆分、细粒度 mprotect 等）。

**拆分步骤**：
```
1. 找到 L2 block entry，确认是 block descriptor
2. 分配一个 L3 页表页
3. 清零 L3 页表
4. 填充 512 个 L3 page entry（继承 block 的物理地址和属性）
5. 将 L2 entry 改为 table descriptor，指向新 L3 表
6. 刷新 TLB
7. 更新计数：huge_pages_count--, user_pages_count += 512, kernel_pages_count++
```

---

## 5. 页面回收算法（reclaim_pages）

### 5.1 函数签名

```c
int reclaim_pages(struct task_struct *task, int nr_pages);
```

- `task`：目标 VM 的 task_struct
- `nr_pages`：目标回收页数（4KB 粒度）
- 返回值：实际回收的页面数

### 5.2 扫描范围

扫描整个 32-bit IPA 地址空间（0 ~ 4GB），按 4KB 页粒度，环形扫描。

`clock_hand` 是一个 32-bit IPA 指针，每次扫描完一页前进 4KB，溢出后自动回绕到 0。

### 5.3 扫描逻辑

```
for (scanned < max_scan && reclaimed < nr_pages) {
    根据 clock_hand 计算 L1/L2/L3 索引
    检查 L1 entry:
        无效 → 跳过整个 L1 entry 范围（512 * 512 * 4KB = 1GB）
        table 且 AF=0 → 设置 AF（修复旧 table entry）
    
    检查 L2 entry:
        无效 → 跳过整个 L2 entry 范围（512 * 4KB = 2MB）
        table 且 AF=0 → 设置 AF（修复旧 table entry）
        block:
            AF=1 → 清 AF（第二次机会）
            AF=0 → 回收整个 block（512 页）
    
    检查 L3 entry:
        AF=1 → 清 AF（第二次机会）
        AF=0 → 回收该页
    
    clock_hand += PAGE_SIZE
    scanned++
}
```

### 5.4 渐进式回收

- 每次最多扫描一圈（max_scan = 总映射页数）
- 达到目标回收数就停止
- 不会长时间持有锁（实际上是关中断的，但扫描很快）

### 5.5 Table Descriptor 的 AF 位

**重要发现**：Stage-2 的 table descriptor（L1/L2 指向次一级页表的 entry）也有 AF 位。

如果 table descriptor 的 AF=0：
- Stage-2 页表遍历过程中会触发 table level AF fault
- QEMU 会将其误报为 Level 1/2 Translation Fault
- 导致死循环（translation fault → demand paging → 无效修复 → 死循环）

**修复措施**：
1. **新建 table entry 时设置 AF**：`map_stage2_table` 中新建 L1/L2 table entry 时加上 `MM_STAGE2_ACCESS`
2. **回收扫描中修复旧 entry**：`reclaim_pages` 扫描 L1/L2 table entry 时，如果 AF=0 就设置 AF

### 5.6 TLB 刷新策略

- 回收过程中不清 TLB（批量操作，减少开销）
- 扫描结束后统一刷新一次 TLB
- 刷新条件：`scanned_mapped > 0`（只要修改了任何 entry 的 AF 位，都需要刷新）

---

## 6. 回收到分配路径的集成

### 6.1 Demand Paging 路径

在 `handle_mem_abort` 的 demand paging 路径中：
- 先做 quota_check
- 如果配额不足，调用 `reclaim_pages` 尝试回收
- 回收成功后再次尝试分配

```c
if (!quota_check(current, PAGE_SIZE)) {
    int reclaimed = reclaim_pages(current, 1);
    if (reclaimed <= 0) {
        current->mm.reclaim_fail_count++;
        return -1;
    }
    /* 回收成功，继续分配 */
}
```

### 6.2 显式分配路径

在 `hyp_mem_map` 中：
- 先做 quota_check
- 如果配额不足，调用 `reclaim_pages` 尝试回收
- 回收成功后再次尝试分配

---

## 7. HVC 接口

### 7.1 HVC_MEM_RECLAIM_STATS (#29)

查询回收统计信息。

**参数**：无
**返回**：
- `x0` = 累计回收页数（4KB 粒度）
- `x1` = 回收失败次数

### 7.2 HVC_MEM_RECLAIM (#34)

主动触发页面回收。

**参数**：
- `x0` = 目标回收页数（4KB 粒度）
**返回**：
- `x0` = 实际回收的页面数

---

## 8. Guest 测试用例

### 8.1 测试地址

- `TEST_START = 0x50000000`（512 页 = 2MB 测试区域）
- 远离代码段/数据段/堆/栈，避免干扰

### 8.2 测试流程

```
1. 预分配 512 页（hyp_mem_map），写入不同的数据模式
2. 查询初始回收计数 = 0 ✓
3. 第一次回收（请求 100 页）
   - 预期：回收 0 页（所有页刚被访问过，AF=1，第一次扫描只清 AF）
4. 访问其中一部分页（写入新数据）
   - 这些页的 AF 会被重新设置为 1
5. 第二次回收（请求 400 页）
   - 预期：回收约 400 页（未被访问的页 AF=0，被回收）
6. 验证：
   - 被回收的页面：数据为 0（demand paging 分配新页，清零）
   - 被访问的页面：数据正确（未被回收）
7. 统计：回收页数、失败次数等正确 ✓
```

### 8.3 测试结果

- **VM1**：PASSED ✓
- **VM2**：PASSED ✓
- 所有前置测试不受影响

---

## 9. 关键 Bug 与调试记录

### 9.1 Bug 1：Instruction Abort 未处理

**现象**：清除代码页 AF 后，Guest 死循环，QEMU 报告 Level 1 Translation Fault。

**根因**：取指触发的是 Instruction Abort（EC=32），不是 Data Abort（EC=36），我们没有处理 IABT。

**修复**：新增 `ESR_EL2_EC_IABT_LOW` case，复用 `handle_mem_abort`。

### 9.2 Bug 2：页表页未清零

**现象**：回收算法扫描到无效 entry，行为异常。

**根因**：`map_stage2_table` 分配中间页表后没有清零，导致垃圾数据被当成有效 entry。

**修复**：分配页表后立即 `memset(0, PAGE_SIZE)`。

### 9.3 Bug 3：get_ipa AT 翻译失败

**现象**：ipa 值错误，demand paging 映射到错误地址。

**根因**：Guest MMU 开启后，AT 指令翻译可能因各种原因失败（如 table level AF fault），此时 PAR_EL1.F=1，地址无效。但我们没有检查 F 位，直接使用了错误的地址。

**修复**：get_ipa 中检查 PAR_EL1.F 位，翻译失败时 fallback 为 VA（等值映射 Guest 下 VA == IPA）。

### 9.4 Bug 4：Table Descriptor 缺少 AF 位（关键根因）

**现象**：清除代码页 AF 后，Guest 触发大量 Instruction Abort，QEMU 报告 Level 1 Translation Fault，系统死循环。

**根因**：Stage-2 table descriptor（L1/L2 entry）也有 AF 位，但 `map_stage2_table` 只设置了 `MM_TYPE_PAGE_TABLE (0x3)`，没有设置 AF 位。当 page entry 的 AF 被清除后，table entry 本身 AF=0 导致 table level AF fault，QEMU 将其误报为 Level 1 Translation Fault，handler 按 translation fault 处理 → 死循环。

**修复**：
1. `map_stage2_table` 新建 table entry 时加上 AF 位
2. `reclaim_pages` 扫描时修复旧 table entry 的 AF 位

### 9.5 Bug 5：S1PTW=1 时用错 fault 地址（最终根因）

**现象**：系统仍有巨量 demand paging，同一个地址反复 fault。

**根因**：S1PTW=1（Stage-1 translation table walk fault）时，FAR_EL2 是指令 VA，不是 Stage-2 fault 的真实 IPA。真实地址在 HPFAR_EL2 中。我们一直在错误的地址上设置 AF 位。

**诊断过程**：
1. 观察到反复 fault 的地址 entry 有效且 AF=1，很困惑
2. 读取 HPFAR_EL2，发现 hpfar = 0x4000d0
3. 在 demand paging 中对比 hpfar 和已知正确的 ipa，得出格式：`ipa = hpfar << 8`
4. 检查 ESR.S1PTW = 1，确认是 Stage-1 页表遍历中的 Stage-2 fault
5. 真正的 fault 发生在 Guest 页表页面（0x4000d000）上，其 AF 被回收算法清除了

**修复**：在 `handle_mem_abort` 入口判断 S1PTW 位，S1PTW=1 时从 HPFAR 提取真实 IPA。

---

## 10. 文件变更清单

| 文件 | 类型 | 说明 |
|------|------|------|
| `include/common/sched.h` | 修改 | mm_struct 新增 reclaim_count / reclaim_fail_count / clock_hand |
| `include/common/hvc.h` | 修改 | 新增 HVC_MEM_RECLAIM_STATS #29 / HVC_MEM_RECLAIM #34 |
| `include/common/mm.h` | 修改 | 新增 reclaim_pages / split_stage2_block / handle_access_flag_fault 声明 |
| `include/arch/aarch64/mmu.h` | 修改 | 新增 MM_STAGE2_ACCESS 常量定义 |
| `hypervisor/common/mm.c` | 修改 | AF fault 处理 + 大页拆分 + 时钟算法回收 + S1PTW 支持 + 容错处理 |
| `hypervisor/arch/aarch64/sync_exc.c` | 修改 | 新增 Instruction Abort (IABT) 处理分支 |
| `hypervisor/boards/virt/main_virt.c` | 修改 | HVC handler 新增 RECLAIM_STATS / RECLAIM |
| `guests/el1_test/guest.h` | 修改 | Guest 侧 HVC 号、测试辅助函数 |
| `guests/el1_test/main.c` | 修改 | Phase 23 测试用例（Test 16） |

---

## 11. 总结

Phase 23 实现了完整的 Stage-2 页面回收机制：

1. **硬件辅助的 LRU 近似**：利用 ARMv8 Access Flag + 时钟算法，实现简单高效的页面置换
2. **全面的 fault 处理**：支持 Data Abort、Instruction Abort、S1PTW 三种场景
3. **大页友好**：支持 2MB block 的 AF 追踪与整体回收
4. **无缝集成**：配额不足时自动触发回收，对 Guest 透明
5. **调试发现的关键机制**：S1PTW + HPFAR、table descriptor AF 位、QEMU AF fault 误报

配合 Phase 22 的内存配额，实现了完整的内存超售闭环：
- **超售**：总分配可以超过物理内存
- **回收**：内存紧张时自动回收不活跃页
- **下一步**：KSM 内存去重（进一步提升超售比）
