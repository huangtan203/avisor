# Phase 24：KSM 内存去重 — 详细设计文档

## 1. 概述

### 1.1 背景

方向 2：内存虚拟化增强。
- Phase 20：完善 Stage-2 页表管理（unmap + 页表自底向上回收）
- Phase 21：Stage-2 细粒度权限管理（mprotect + HVC_MEM_PROTECT）
- Phase 22：内存超售与按需分配优化（2MB 大页 + 内存配额）
- Phase 23：页面回收与 LRU 置换（时钟算法 + Access Flag）
- **Phase 24：KSM 内存去重（内容相同页合并 + 写时复制）**

Phase 22/23 已经建立了内存超售与回收的闭环：超售允许总分配超过物理内存，回收在紧张时腾出空间。但超售比还有进一步提升的空间——多个 VM（甚至同一 VM 内部）往往映射了大量**内容完全相同**的物理页（例如相同的只读数据、零页、重复的 Guest 内核镜像）。如果能让这些映射共享同一份物理页，就能在不增加物理内存的前提下支撑更多 Guest。

Phase 24 借鉴 Linux 的 **KSM（Kernel Samepage Merging）** 思想，实现：
- 按页面内容哈希（FNV-1a）快速定位候选相同页
- 逐字节比较防止哈希碰撞
- 相同页合并为共享只读页，释放冗余物理页
- 写入时触发 Stage-2 Permission Fault → **写时复制（CoW）** 分离

### 1.2 目标

- ✅ 实现页面内容哈希（FNV-1a 32-bit）+ 哈希表索引
- ✅ 实现 Stage-2 页表扫描与相同页合并（ksm_scan_vm）
- ✅ 实现写时复制（CoW）分离（ksm_cow_break）
- ✅ 用 Stage-2 descriptor 的 IGNORED bit 55 标记共享页（MM_STAGE2_KSM_SHARED）
- ✅ 引入正确的 Stage-2 PA 提取掩码（MM_STAGE2_PA_MASK）
- ✅ 新增 2 个 HVC 接口：KSM_SCAN / KSM_STATS
- ✅ Guest 侧完整测试验证（Test 17）

### 1.3 非目标

- 不实现跨页比较的树状索引（如 KSM 的 red-black tree）—— 用哈希链表即可满足本项目规模
- 不做零页（zero page）特殊化 —— 零页也走通用哈希合并
- 不做后台周期性扫描线程 —— 由 Guest 通过 HVC 主动触发
- 不追踪共享页的换出/换入 —— 共享页仍受 Phase 23 回收约束

---

## 2. 设计总览

### 2.1 架构图

```
┌──────────────────────────────────────────────────────────────────┐
│                        Guest VM (EL1)                           │
│  ┌──────────────────────┐  ┌──────────────────────────────────┐ │
│  │ HVC_MEM_KSM_SCAN     │  │ HVC_MEM_KSM_STATS               │ │
│  │ (主动触发扫描合并)    │  │ (查询去重统计)                   │ │
│  └──────────┬───────────┘  └──────────────┬───────────────────┘ │
└─────────────┼─────────────────────────────┼─────────────────────┘
              │ HVC #35                       │ HVC #36
┌─────────────┼─────────────────────────────┼─────────────────────┐
│  EL2 (Hypervisor)                            │                  │
│  ┌──────────▼───────────┐  ┌──────────────▼───────────────────┐ │
│  │ ksm_scan_vm()        │  │ ksm_get_stats()                 │ │
│  └──────────┬───────────┘  └──────────────────────────────────┘ │
│             │                                                     │
│  ┌──────────▼───────────────────────────────────────────────┐    │
│  │  Stage-2 页表遍历 (L1 → L2 → L3)                          │    │
│  │   - 跳过 device memory (MMIO)                             │    │
│  │   - 跳过只读页（可能已是共享页）                           │    │
│  │   - 计算 FNV-1a 哈希 → 哈希表查找                        │    │
│  │   - 命中 → ksm_merge_page（合并为共享只读页）             │    │
│  │   - 未命中 → ksm_make_shared（首个页转为共享页）          │    │
│  └──────────┬───────────────────────────────────────────────┘    │
│             │                                                     │
│  ┌──────────▼───────────────────────────────────────────────┐    │
│  │  CoW 分离 (ksm_cow_break)                                 │    │
│  │   - Stage-2 permission fault + 写操作 + 非 MMIO          │    │
│  │   - refcount == 1 → 直接改回 RW                          │    │
│  │   - refcount >  1 → 分配新页 + 拷贝 + 重映射 RW          │    │
│  └───────────────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────────┘
```

### 2.2 核心数据结构

#### 2.2.1 ksm_page（include/common/ksm.h）

```c
struct ksm_page {
    struct ksm_page *next;      /* 哈希链表下一节点 */
    paddr_t pa;                 /* 物理页地址（4KB 对齐） */
    uint32_t hash;              /* 页面内容哈希值 */
    int refcount;               /* 引用计数（多少个 Stage-2 映射指向此页） */
    int status;                 /* KSM_PAGE_SHARED / KSM_PAGE_COW */
};
```

#### 2.2.2 ksm_stats（include/common/ksm.h）

```c
struct ksm_stats {
    int pages_shared;     /* 共享页数量（物理页） */
    int pages_sharing;    /* 共享映射数（有多少个映射指向共享页） */
    int pages_merged;     /* 累计合并页数 */
    int pages_unmerged;   /* 累计分离页数（CoW 触发的） */
    int full_scans;       /* 完整扫描次数 */
};
```

#### 2.2.3 全局哈希表

```c
#define KSM_HASH_SIZE  4096                 /* bucket 数，2 的幂 */
static struct ksm_page *ksm_hashtab[KSM_HASH_SIZE];

#define KSM_MAX_PAGES  2048                 /* ksm_page 描述符池上限 */
static struct ksm_page ksm_page_pool[KSM_MAX_PAGES];
static int ksm_page_free_count = KSM_MAX_PAGES;
static struct ksm_page *ksm_page_free_list;
```

- 本项目无 `malloc`，用**静态描述符池 + 空闲链表**管理 `ksm_page` 结构体
- 哈希表索引：`idx = hash & (KSM_HASH_SIZE - 1)`（4096 是 2 的幂）

### 2.3 页面内容哈希：FNV-1a

```c
#define FNV1A_32_OFFSET  2166136261u
#define FNV1A_32_PRIME   16777619u

uint32_t ksm_page_hash(const void *page_addr)
{
    const unsigned char *data = (const unsigned char *)page_addr;
    uint32_t hash = FNV1A_32_OFFSET;
    for (int i = 0; i < PAGE_SIZE; i++) {
        hash ^= data[i];
        hash *= FNV1A_32_PRIME;
    }
    return hash;
}
```

- FNV-1a 逐字节滚动哈希，对 4KB 页面内容敏感、实现简单、分布均匀
- 哈希相同不代表内容相同，必须 `ksm_pages_equal` 逐字节确认（防止碰撞）

---

## 3. 扫描与合并

### 3.1 ksm_scan_vm

**位置**：`hypervisor/common/ksm.c`

遍历源 VM 的 Stage-2 页表（L1 → L2 → L3），对每个 L3 4KB 页：

```
1. 跳过无效 entry（(e & 0x3) != 0x3）
2. 跳过 device memory 页（(e & (0x7 << 2)) != MM_STAGE2_MEMATTR）
   —— MMIO 不能做内容去重
3. 跳过只读页（(e & S2_AP_MASK) == S2_AP_RO）
   —— 只读页可能已经是共享页，避免重复合并
4. 计算该页内容哈希
5. 哈希表查找相同内容共享页：
   - 命中 → ksm_merge_page(task, ipa, existing)
   - 未命中 → ksm_make_shared(pa, hash)（该页转为首个共享页，映射改 RO）
```

### 3.2 ksm_make_shared（首个页）

```c
static struct ksm_page *ksm_make_shared(paddr_t pa, uint32_t hash)
{
    struct ksm_page *kp = ksm_page_alloc();
    kp->pa = pa; kp->hash = hash;
    kp->refcount = 1; kp->status = KSM_PAGE_SHARED;
    ksm_hash_insert(kp);
    ksm_global_stats.pages_shared++;
    return kp;
}
```

调用方把该页的 Stage-2 映射改为 **RO + KSM 标记**（后续写入触发 CoW）。

### 3.3 ksm_merge_page（合并冗余页）

```c
static int ksm_merge_page(struct task_struct *task, vaddr_t ipa, struct ksm_page *kp)
{
    /* 定位 L3 entry，提取旧物理页 old_pa */
    /* 构造新 entry：kp->pa | 共享页属性（RO + KSM_SHARED） */
    entry = kp->pa;
    entry |= MM_TYPE_PAGE;
    entry |= MM_STAGE2_ACCESS;     /* AF=1 */
    entry |= MM_STAGE2_SH;         /* inner shareable */
    entry |= S2_AP_RO;             /* 只读，触发 CoW */
    entry |= MM_STAGE2_MEMATTR;    /* Normal memory */
    entry |= MM_STAGE2_KSM_SHARED; /* KSM 共享页标记 */

    *l3_entry_p = entry;

    deallocate_page(TO_VADDR(old_pa));   /* 释放冗余物理页 */
    task->mm.user_pages_count--;

    kp->refcount++;
    ksm_global_stats.pages_sharing++;
    ksm_global_stats.pages_merged++;

    /* TLB 失效 */
    return 0;
}
```

---

## 4. 写时复制（CoW）

### 4.1 触发条件

`hypervisor/common/mm.c` 的 `handle_mem_abort` 中，在正常 permission fault 处理之前：

```c
if ((dfsc >> 2) == 0x3 && !is_mmio && ((esr >> 6) & 1)) {
    if (ksm_cow_break(current, ipa) == 0)
        return 0;   /* CoW 分离成功，返回重试 */
}
```

- `dfsc >> 2 == 0x3`：permission fault（写入只读页）
- `WnR == 1`（`(esr >> 6) & 1`）：写操作
- `!is_mmio`：不是 MMIO 地址

### 4.2 ksm_cow_break

```
1. 遍历页表定位 L3 entry，检查是否 KSM 共享页（哈希表按内容哈希查找）
2. refcount == 1：
   - 只有本映射指向共享页 → 直接改回 RW，清除 KSM 标记
   - 从哈希表移除，释放 ksm_page 描述符
   - 无需分配新页（零拷贝优化）
3. refcount > 1：
   - 分配新物理页，拷贝共享内容
   - 重映射为 RW（普通页）
   - 共享页 refcount--，pages_unmerged++
```

---

## 5. Stage-2 descriptor 扩展

### 5.1 KSM 共享页标记（bit 55）

ARMv8 Stage-2 page/block descriptor 的 bits [55:50] 是 **IGNORED**，软件可自由使用。

```c
#define MM_STAGE2_KSM_SHARED (1ULL << 55)
```

用于 permission fault 时快速判断是否需要 CoW（结合哈希表确认）。

### 5.2 Stage-2 PA 提取掩码（MM_STAGE2_PA_MASK）

**关键修复**：之前代码用 `PAGE_MASK`（`0xfffffffffffff000`）提取 descriptor 物理地址。引入 bit 55 作为 KSM 标记后，`PAGE_MASK` 会把 bit 55 一并保留，导致提取出的 PA 带着高位垃圾，读非法地址。

```c
/*
 * 48-bit IPA 下，Stage-2 descriptor 的 Output Address 是 bits[47:12]。
 * 不能用 PAGE_MASK，因为 bit[63:48] 是 IGNORED / 软件可用位（如 KSM 的 bit 55），
 * 必须一并清掉。
 */
#define MM_STAGE2_PA_MASK (0x0000FFFFFFFFF000ULL)
```

---

## 6. HVC 接口

### 6.1 HVC_MEM_KSM_SCAN (#35)

主动触发 KSM 扫描合并。

**参数**：`x0` = 最大扫描页数（0 = 全部扫描）
**返回**：`x0` = 实际合并的页数

### 6.2 HVC_MEM_KSM_STATS (#36)

查询 KSM 去重统计。

**参数**：无
**返回**：
- `x0` = 共享页数量
- `x1` = 共享映射数
- `x2` = 累计合并页数
- `x3` = 累计分离页数（CoW）

Guest 侧新增 `hvc_call_ret4` 一次取回 4 个返回值。

---

## 7. Guest 测试用例

### 7.1 测试流程（Test 17）

```
1. 分配多页内容相同的页面（写入相同模式）
2. 查询初始 KSM 统计（shared/sharing/merged/unmerged）
3. 触发 HVC_MEM_KSM_SCAN（全量扫描）
4. 验证：相同页被合并，pages_merged > 0，共享映射数增加
5. 写入共享页中的一页（触发 CoW）
6. 验证：写入成功，pages_unmerged 增加（CoW 分离）
7. 校验：分离后的页面内容正确，其他共享页不受影响
```

### 7.2 测试结果

- **VM1**：PASSED ✓
- **VM2**：PASSED ✓
- 前置测试不受影响

---

## 8. 文件变更清单

| 文件 | 类型 | 说明 |
|------|------|------|
| `include/common/ksm.h` | 新增 | ksm_page / ksm_stats 结构体 + 接口声明 |
| `hypervisor/common/ksm.c` | 新增 | 哈希表 + FNV-1a + 扫描合并 + CoW 分离 + 统计 |
| `include/arch/aarch64/mmu.h` | 修改 | 新增 MM_STAGE2_KSM_SHARED / MM_STAGE2_PA_MASK |
| `include/common/hvc.h` | 修改 | 新增 HVC_MEM_KSM_SCAN #35 / HVC_MEM_KSM_STATS #36 |
| `hypervisor/common/mm.c` | 修改 | permission fault 前置 KSM CoW 分离判断 |
| `hypervisor/arch/aarch64/sync_exc.c` | 修改 | HVC handler 新增 KSM_SCAN / KSM_STATS |
| `hypervisor/boards/virt/main_virt.c` | 修改 | 启动流程调用 ksm_init() |
| `guests/el1_test/guest.h` | 修改 | Guest 侧 KSM HVC 常量 + hvc_call_ret4 |
| `guests/el1_test/main.c` | 修改 | Phase 24 测试用例（Test 17） |

---

## 9. 总结

Phase 24 实现了基于内容哈希的 KSM 内存去重：

1. **零 malloc 依赖**：静态描述符池 + 空闲链表管理 ksm_page
2. **内容敏感哈希**：FNV-1a 32-bit + 逐字节比较，防止碰撞误合并
3. **写时复制**：合并后置只读，写入经 Stage-2 permission fault 触发 CoW 分离
4. **refcount==1 快速路径**：单映射共享页直接改回 RW，零拷贝
5. **硬件位复用**：利用 Stage-2 IGNORED bit 55 标记共享页
6. **正确的 PA 掩码**：引入 MM_STAGE2_PA_MASK，兼容软件占用高位

配合 Phase 22 内存配额与 Phase 23 页面回收，内存超售比进一步提升：相同内容页合并后，物理内存的实际利用率显著提高。
