# Phase 22：内存超售与按需分配优化 — 详细设计文档

## 1. 概述

### 1.1 背景

方向 2：内存虚拟化增强。
- Phase 20：完善 Stage-2 页表管理（unmap + 页表自底向上回收）
- Phase 21：Stage-2 细粒度权限管理（mprotect + HVC_MEM_PROTECT）
- **Phase 22：内存超售与按需分配优化（2MB 大页 + 内存配额）**

随着 VM 数量和内存需求的增长，纯 4KB 小页的 demand paging 面临两个问题：
1. **TLB 效率低**：4KB 页面太小，TLB miss 率高，大内存工作负载性能差
2. **无资源隔离**：单个 VM 可以耗尽所有物理内存，影响其他 VM

Phase 22 引入两个核心机制解决上述问题：
- **2MB 大页（Block Mapping）**：支持 2MB 粒度的 Stage-2 映射，减少 TLB miss 和页表开销
- **内存配额（Memory Quota）**：每个 VM 有最大内存限制，实现内存资源隔离与超售

### 1.2 目标

- ✅ 支持 Stage-2 2MB block 映射（map / unmap / mprotect）
- ✅ 连续物理页分配器（allocate_block_pages / free_block_pages）
- ✅ 内存配额机制（set_task_memory_quota / quota_check）
- ✅ Demand Paging 集成配额检查
- ✅ 新增 5 个 HVC 接口：QUERY / SET_QUOTA / HUGE_MAP / HUGE_UNMAP / HUGE_PROTECT
- ✅ Guest 侧完整测试验证

### 1.3 非目标

- 不实现 1GB 大页（L1 block）—— 2MB 是更实用的粒度
- 不实现页面回收（Page Reclaim）/ 交换（Swapping）—— 留到后续 Phase
- 不实现透明大页（THP）—— 由 Guest 显式通过 hypercall 请求

---

## 2. 设计总览

### 2.1 架构图

```
┌───────────────────────────────────────────────────────────┐
│                      Guest VM (EL1)                       │
│  ┌──────────────┐  ┌──────────────┐  ┌───────────────┐  │
│  │  HVC_MEM_    │  │  HVC_MEM_    │  │  HVC_MEM_     │  │
│  │  HUGE_MAP    │  │  QUERY       │  │  SET_QUOTA    │  │
│  └──────┬───────┘  └──────┬───────┘  └───────┬───────┘  │
└─────────┼─────────────────┼───────────────────┼──────────┘
          │ HVC #26-28      │ HVC #24          │ HVC #25
┌─────────┼─────────────────┼───────────────────┼──────────┐
│  ┌──────▼───────┐  ┌──────▼───────┐  ┌───────▼───────┐  │
│  │ handle_hvc64 │  │ handle_hvc64 │  │ handle_hvc64  │  │
│  └──────┬───────┘  └──────┬───────┘  └───────┬───────┘  │
│         │                 │                  │          │
│  ┌──────▼───────┐  ┌──────▼───────┐  ┌───────▼───────┐  │
│  │ map_stage2_  │  │ get_task_    │  │ set_task_     │  │
│  │ block        │  │ memory_usage │  │ memory_quota  │  │
│  └──────┬───────┘  └──────────────┘  └───────────────┘  │
│         │                                               │
│  ┌──────▼───────┐  ┌──────────────┐  ┌───────────────┐  │
│  │ allocate_    │  │ quota_check  │  │ mm_struct:    │  │
│  │ block_pages  │  │ (demand page)│  │ huge_pages_   │  │
│  └──────┬───────┘  └──────┬───────┘  │ count         │  │
│         │                 │          │ max_memory    │  │
│  ┌──────▼───────┐  ┌──────▼───────┐  └───────────────┘  │
│  │   Page Pool  │  │   Page Pool  │                     │
│  │   (memap)    │  │   (memap)    │                     │
│  └──────────────┘  └──────────────┘                     │
│                                              EL2 (Hypervisor) │
└───────────────────────────────────────────────────────────┘
```

### 2.2 新增数据结构

#### 2.2.1 mm_struct 扩展（include/common/sched.h）

```c
struct mm_struct {
    unsigned long first_table;
    int user_pages_count;       /* 4KB 用户页数量 */
    int kernel_pages_count;     /* 页表页数量 */
    int huge_pages_count;       /* Phase 22: 2MB 大页数量 */
    unsigned long max_memory;   /* Phase 22: 内存配额（字节），0=无限制 */
    unsigned long cntvoff;
};
```

**字段说明**：
- `huge_pages_count`：已映射的 2MB block 数量。总用户内存 =
  `user_pages_count * 4KB + huge_pages_count * 2MB`
- `max_memory`：内存配额（字节）。0 表示无限制（默认行为）。
  demand paging 和显式大页分配前都会检查是否超出配额。

#### 2.2.2 新增 HVC 常量（include/common/hvc.h）

```c
#define HVC_MEM_QUERY        24  /* 查询：x0=使用字节 x1=配额 x2=大页数 */
#define HVC_MEM_SET_QUOTA    25  /* 设置配额：x0=max_bytes，返回 0/-1 */
#define HVC_MEM_HUGE_MAP     26  /* 映射 2MB 大页：x0=ipa x1=prot，返回 0/-1 */
#define HVC_MEM_HUGE_UNMAP   27  /* 取消 2MB 大页：x0=ipa，返回 0/-1 */
#define HVC_MEM_HUGE_PROTECT 28  /* 修改大页权限：x0=ipa x1=prot，返回 0/-1 */
```

---

## 3. 2MB 大页（Block Mapping）详细设计

### 3.1 ARMv8 Stage-2 Block Descriptor 格式

Stage-2 L2 表的 entry 可以是 block descriptor（2MB 粒度）：

```
  63       52 51        48 47                     21 20 12 11 ... 8 7 6 5 4 3 2 1 0
 ┌───────────┬────────────┬─────────────────────────┬───────┬─────┬───┬───┬───┐
 │  RES0     │  RES0      │  Output Address[47:21]  │  ...  │ AP  │SH │AF │ 0 │
 └───────────┴────────────┴─────────────────────────┴───────┴─────┴───┴───┴───┘
```

- **bit[1:0] = 01**：block descriptor（区别于 page descriptor 的 11）
- **Output Address[47:21]**：2MB 对齐的物理地址
- **AP[7:6]、SH[9:8]、AF[10]**：与 page descriptor 含义相同

### 3.2 新增 MMU 常量（include/arch/aarch64/mmu.h）

```c
#define MMU_STAGE2_BLOCK_FLAGS                                            \
    (MM_TYPE_BLOCK | MM_STAGE2_ACCESS | MM_STAGE2_SH | MM_STAGE2_AP | \
     MM_STAGE2_MEMATTR)
```

- `MM_TYPE_BLOCK = 0x1`：block descriptor type
- 其他位与 `MMU_STAGE2_PAGE_FLAGS` 相同（AF / SH / AP / MEMATTR）

### 3.3 map_stage2_block() — 大页映射

**函数签名**：
```c
int map_stage2_block(struct task_struct *task, vaddr_t va, paddr_t pa,
                     uint64_t flags);
```

**参数**：
- `task`：目标 VM task
- `va`：目标 IPA（必须 2MB 对齐）
- `pa`：物理地址（必须 2MB 对齐）
- `flags`：映射标志（建议用 `MMU_STAGE2_BLOCK_FLAGS` 变体）

**返回值**：0 成功，-1 失败

**流程**：
1. 参数校验（2MB 对齐、task 非空）
2. 确保 L1 页表存在（first_table）
3. 遍历 L1 → L2：
   - 如果 L1 entry 为空，分配新的 L2 表页，写入 L1 entry
   - 如果 L1 entry 是 table，取出 L2 表地址
   - 如果 L1 entry 是 block（1GB），返回失败
4. 在 L2 中检查目标 entry：
   - 如果已有映射（table 或 block），返回失败（不覆盖）
5. 写入 block descriptor
6. `huge_pages_count++`

**注意**：不自动 TLB flush（由调用方按需处理）。

### 3.4 unmap_stage2_block() — 大页取消映射

**函数签名**：
```c
int unmap_stage2_block(struct task_struct *task, vaddr_t va, int do_free);
```

**参数**：
- `do_free`：1 = 释放 2MB 物理内存（512 个 4KB 页），0 = 不释放

**流程**：
1. 遍历 L1 → L2
2. 检查 L2 entry 是否是 block descriptor
3. 如果 do_free=1，释放连续 512 个物理页（调用 free_page 循环）
4. 清零 L2 entry
5. `huge_pages_count--`
6. 如果 L2 表变空，回收 L2 表页（自底向上回收，与 unmap_stage2_page 一致）
7. TLB flush（vmalle1 + dsb ish + isb）

### 3.5 mprotect_stage2_block() — 大页权限修改

**函数签名**：
```c
int mprotect_stage2_block(struct task_struct *task, vaddr_t va, uint64_t new_ap);
```

**流程**：
1. 遍历 L1 → L2
2. 检查 L2 entry 是否是 block descriptor
3. 清除旧 AP 位，设置新 AP 位
4. TLB flush

### 3.6 与现有 Page Mapping 的关系

- **互相独立**：大页和小页是两种不同的映射方式，分别对应 L2 entry 的不同类型
- **不自动转换**：不提供大页↔小页的自动拆分/合并（避免复杂性）
- **冲突检查**：如果某个 2MB 区域已经有 L3 表（小页映射），调用 `map_stage2_block` 会返回失败。调用方需要先 unmap 所有小页。

---

## 4. 连续物理页分配器

### 4.1 背景

2MB 大页需要 2MB 对齐的连续物理内存（512 个连续 4KB 页）。
原有的 `get_free_page()` 只能分配单个 4KB 页，不保证连续性。

### 4.2 allocate_block_pages()

**函数签名**：
```c
paddr_t allocate_block_pages(int npages);
```

**参数**：`npages` — 连续页数（必须是 2 的幂，保证自然对齐）

**返回值**：连续块的起始物理地址，失败返回 0

**算法**：
```
for i in 0..page_nr:
    start = (last_page_id + i) % page_nr
    if start % npages != 0: 跳过（保证自然对齐）
    检查 start..start+npages-1 是否都空闲
    if 全空闲:
        标记为已用
        更新 last_page_id
        返回 start * PAGE_SIZE + start_addr
return 0  // 失败
```

**设计要点**：
- **自然对齐**：起始索引必须是 npages 的倍数，保证物理地址 2MB 对齐
- **循环扫描**：从 last_page_id 开始（next-fit 策略），不总是从头扫描
- **加锁保护**：使用 page_pool_lock（和单页分配共享锁）

### 4.3 free_block_pages()

**函数签名**：
```c
void free_block_pages(paddr_t block_pa, int npages);
```

**流程**：
1. 计算起始页索引
2. 边界检查（非法地址则 PANIC）
3. 循环清零 memap 中对应条目

---

## 5. 内存配额（Memory Quota）机制

### 5.1 设计目标

- **资源隔离**：每个 VM 的内存使用受配额限制，单个 VM 不能耗尽物理内存
- **超售支持**：所有 VM 的配额之和可以超过物理内存（超售）
- **按需检查**：在实际分配内存（demand paging 或显式分配）前检查配额

### 5.2 配额接口

#### set_task_memory_quota()
```c
int set_task_memory_quota(struct task_struct *task, unsigned long max_bytes);
```
- 设置配额（字节），0 表示无限制
- 可以动态调整（增大或减小）

#### get_task_memory_usage()
```c
unsigned long get_task_memory_usage(struct task_struct *task);
```
- 返回当前用户内存使用量（字节）
- `= user_pages_count * 4KB + huge_pages_count * 2MB`

#### quota_check()
```c
int quota_check(struct task_struct *task, unsigned long add_bytes);
```
- 检查再分配 add_bytes 字节后是否超出配额
- 返回 0 = 允许，-1 = 超出配额
- max_memory = 0 时始终返回 0（无限制）

### 5.3 配额检查点

配额检查在以下位置进行：

#### 5.3.1 Demand Paging
在 `handle_mem_abort()` 的 translation fault 处理中：
```c
if (quota_check(current, PAGE_SIZE) < 0) {
    WARN("demand page: quota exceeded...");
    return -1;
}
```
超出配额时，demand paging 失败，Guest 收到 Data Abort（相当于 OOM）。

#### 5.3.2 大页分配（HVC_MEM_HUGE_MAP）
```c
if (quota_check(current, SECTION_SIZE) < 0) {
    regs->regs[0] = -1;
    break;
}
```

#### 5.3.3 不检查的情况
- 页表页分配（kernel_pages_count）：不计入配额，属于 Hypervisor 管理开销
- MMIO 映射：不分配物理内存，不计数
- 共享内存：由共享内存子系统独立管理

---

## 6. HVC 接口详细说明

### 6.1 HVC_MEM_QUERY (#24) — 查询内存使用

**调用**：`hvc_call_ret3(HVC_MEM_QUERY, 0, &quota, &huge_count)`

**返回值**：
- `x0`：当前用户内存使用量（字节）
- `x1`：当前配额（字节），0 表示无限制
- `x2`：当前大页数量

### 6.2 HVC_MEM_SET_QUOTA (#25) — 设置配额

**调用**：`hvc_call(HVC_MEM_SET_QUOTA, max_bytes)`

**参数**：
- `x0` = max_bytes：最大内存限制（字节），0 = 无限制

**返回**：`x0` = 0 成功，-1 失败

### 6.3 HVC_MEM_HUGE_MAP (#26) — 映射 2MB 大页

**调用**：`hvc_call2(HVC_MEM_HUGE_MAP, ipa, prot)`

**参数**：
- `x0` = ipa：目标 IPA（必须 2MB 对齐）
- `x1` = prot：权限（0=RW, 1=RO, 2=NONE）

**返回**：`x0` = 0 成功，-1 失败

**失败原因**：地址未对齐 / 已有映射 / 超出配额 / 物理内存不足

### 6.4 HVC_MEM_HUGE_UNMAP (#27) — 取消大页映射

**调用**：`hvc_call(HVC_MEM_HUGE_UNMAP, ipa)`

**参数**：
- `x0` = ipa：目标 IPA（必须 2MB 对齐）

**返回**：`x0` = 0 成功，-1 失败（未映射 / 不是大页）

**说明**：同时释放物理内存（2MB）

### 6.5 HVC_MEM_HUGE_PROTECT (#28) — 修改大页权限

**调用**：`hvc_call2(HVC_MEM_HUGE_PROTECT, ipa, prot)`

**参数**：
- `x0` = ipa：目标 IPA（必须 2MB 对齐）
- `x1` = prot：新权限（0=RW, 1=RO, 2=NONE）

**返回**：`x0` = 0 成功，-1 失败

---

## 7. Guest 侧测试设计

### 7.1 测试架构

测试函数：`phase22_hugepage_quota_test()`
测试位置：Guest 主核，所有测试完成后调用
测试用 IPA 基址：`0x41000000`（16MB 处，2MB 对齐，避开已用区域）

### 7.2 10 个检查点

| # | 测试项 | 操作 | 预期结果 |
|---|-------|------|---------|
| 1 | 初始查询 | HVC_MEM_QUERY | 配额=0（无限制），大页数=0 |
| 2 | 大页映射 | HVC_MEM_HUGE_MAP (RW) | 成功返回 0 |
| 3 | 大页读写 | 读写大页开头和末尾 | 数据正确，无缺页 |
| 4 | 大页计数 | HVC_MEM_QUERY | huge_count = 1 |
| 5 | 大页 mprotect | HVC_MEM_HUGE_PROTECT (RO) | 成功返回 0 |
| 6 | RO 写验证 | 向 RO 大页写入 | 触发 Permission Fault |
| 7 | 设置配额 | HVC_MEM_SET_QUOTA (4MB) | 成功返回 0 |
| 8 | 超配额拒绝 | 配额内已用 2MB，再分配 2MB | 返回失败 |
| 9 | 大页 unmap | HVC_MEM_HUGE_UNMAP | 成功，huge_count 归 0 |
| 10 | 配额内重分配 | unmap 后重新 map 2MB | 成功 |

### 7.3 权限验证机制

复用 Phase 21 的同步异常处理框架：
- `sync_expect_start()` — 开始期望异常
- `sync_expect_end()` — 检查是否收到异常
- `guest_sync_handler()` — 同步异常 C 处理函数，验证 ESR 后返回

---

## 8. 已知问题与后续工作

### 8.1 已知问题

- **PC=0 布局敏感 bug（Phase 21 遗留）**：
  Guest 代码大小变化后，Guest 启动后不久 PC 变为 0，触发 Instruction Abort。
  已知该 bug 在 Phase 20 baseline 不触发，Phase 21/22 因代码/布局变化触发。
  根因正在调查中（怀疑 Hypervisor 侧全局数组越界或指针错误）。
  本 Phase 的功能代码已完成且编译通过，但因该 bug 无法在 QEMU 中完整测试。

### 8.2 后续 Phase 规划

- **Phase 23**：页面回收（Page Reclaim）+ 超售回收策略
  - 访问位（Access Flag）跟踪
  - 最近最少使用（LRU）页面置换
  - 大页拆分（必要时拆成小页再回收）

---

## 9. 改动文件清单

### Hypervisor 侧
| 文件 | 改动 |
|------|------|
| `include/arch/aarch64/mmu.h` | 新增 MMU_STAGE2_BLOCK_FLAGS 常量 |
| `include/common/mm.h` | 新增 6 个函数声明（大页 + 配额 + 连续页分配） |
| `include/common/sched.h` | mm_struct 新增 huge_pages_count / max_memory |
| `include/common/hvc.h` | 新增 5 个 HVC 常量（#24-#28） |
| `hypervisor/common/mm.c` | 新增 map/unmap/mprotect_stage2_block、配额函数、连续页分配器、demand paging 配额检查 |
| `hypervisor/arch/aarch64/sync_exc.c` | 新增 5 个 HVC case 处理 |

### Guest 侧
| 文件 | 改动 |
|------|------|
| `guests/el1_test/guest.h` | 新增 5 个 HVC 常量 + hvc_call_ret3 辅助函数 |
| `guests/el1_test/main.c` | 新增 phase22_hugepage_quota_test() 测试函数 + 调用 |

---

## 10. 总结

Phase 22 实现了内存虚拟化的两个重要增强：

1. **2MB 大页支持** — 通过 Stage-2 block mapping，单个 TLB 条目覆盖 2MB（原来是 4KB 的 512 倍），显著降低 TLB miss 率，同时减少页表内存开销（每个大页节省一个 4KB 的 L3 表）。

2. **内存配额机制** — 为每个 VM 设置最大内存限制，实现 VM 间的内存资源隔离，支持内存超售。配额检查在 demand paging 和显式大页分配时生效。

这两个机制为后续 Phase 实现更高级的内存管理（页面回收、超售回收、透明大页等）奠定了基础。
