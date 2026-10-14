# Phase 20：Stage-2 页表管理完善 详细设计文档

## 1. 概述

**方向**：方向 2 — 内存虚拟化增强

**阶段目标**：完善 Stage-2 页表管理，提取通用的 `unmap_stage2_page()` 函数，支持页表自底向上回收，区分普通内存页和 MMIO 页的取消映射，并将 `hyp_mem_unmap` 和 `shm_detach` 统一迁移到通用函数。

**平台**：QEMU virt (ARMv8-A, GICv3 + ITS + vGIC, 2 核 SMP)

---

## 2. 背景与动机

### 2.1 现状问题

Phase 10 引入了 `hyp_mem_map` / `hyp_mem_unmap` 和共享内存 `shm_attach` / `shm_detach`，但存在以下问题：

1. **代码重复**：`hyp_mem_unmap` 和 `shm_detach` 内部都有独立的页表遍历 + 清零逻辑，重复代码。
2. **无页表回收**：unmap 后只清零 L3 entry，不回收中间的 L2/L3 页表页，导致 kernel_pages_count 只增不减，页表页泄漏。
3. **无 MMIO/普通页区分**：MMIO 映射（如 ivshmem）的 unmap 不应该释放物理页，普通内存页的 unmap 应该释放。之前代码中硬编码了不同的逻辑。
4. **`free_page` 无边界检查**：传入非法物理地址会导致 `memap` 数组越界写入。
5. **缺少内存统计接口**：Guest 无法获知当前 VM 的页分配计数（user / kernel）。

### 2.2 设计目标

- 提取通用 `unmap_stage2_page(task, va, do_free)` 函数，统一所有取消映射逻辑
- 支持页表**自底向上回收**（L3 全空回收 L3，L2 全空回收 L2）
- 通过 `do_free` 参数区分普通内存页（释放物理页）和 MMIO/共享内存页（只清映射）
- `hyp_mem_unmap` 和 `shm_detach` 改为调用通用函数
- 新增 `HVC_MEM_STATS` hypercall，返回 `user_pages_count` 和 `kernel_pages_count`
- 为 `free_page` 增加边界检查，防止越界写入

---

## 3. 核心设计

### 3.1 `unmap_stage2_page()` 通用函数

```c
int unmap_stage2_page(struct task_struct *task, vaddr_t va, int do_free);
```

**参数**：
- `task`：目标 VM 任务
- `va`：目标 IPA/VA（4KB 页对齐）
- `do_free`：1 = 释放物理页（普通内存），0 = 只清映射（MMIO / 共享内存）

**返回值**：0 成功，-1 未映射

#### 3.1.1 页表遍历

三级页表遍历（与 `map_stage2_page` 对称）：

```
L1 (1GB per entry) → L2 (2MB per entry) → L3 (4KB per entry)
```

遍历路径：
1. 从 `task->mm.first_table` 获取 L1 表 PA
2. 根据 `va[38:30]` 索引 L1 entry，得到 L2 表 PA
3. 根据 `va[29:21]` 索引 L2 entry，得到 L3 表 PA
4. 根据 `va[20:12]` 索引 L3 entry，得到目标页 PA

每一级都检查 descriptor type（`entry & 0x3 == 3` 表示 table descriptor），不是则返回 -1。

#### 3.1.2 取消映射流程

```
1. 遍历页表，找到目标 L3 entry
2. 清零 L3 entry（先清映射，再释放物理页）
3. user_pages_count--
4. [可选] 释放物理页（do_free == 1 时）
5. 自底向上回收空的页表页
6. TLB flush（tlbi vmalle1 + dsb ish）
```

**注意**：先清零 entry 再释放物理页，避免释放后页表仍指向该页的窗口。

### 3.2 页表自底向上回收

#### 3.2.1 回收策略

```
unmap L3 entry
    ↓
L3 表是否全空？ ──否──→ 结束
    │是
    ↓
回收 L3 表页（free_page）
kernel_pages_count--
清零 L2 中指向该 L3 的 entry
    ↓
L2 表是否全空？ ──否──→ 结束
    │是
    ↓
回收 L2 表页（free_page）
kernel_pages_count--
清零 L1 中指向该 L2 的 entry
    ↓
结束（L1 永远不回收）
```

**L1 表不回收**：它是 VM 页表的根（`first_table`），生命周期与 task 一致。

#### 3.2.2 `page_table_empty()` 辅助函数

```c
static int page_table_empty(paddr_t table_pa)
{
    uint64_t *table = (uint64_t *)TO_VADDR(table_pa);
    for (int i = 0; i < PTRS_PER_TABLE; i++) {
        if (table[i] != 0)
            return 0;
    }
    return 1;
}
```

遍历整个 512-entry 的页表页，判断是否全 0。

时间复杂度 O(512) = O(1)，因为页表页大小固定（4KB = 512 × 8B）。

### 3.3 调用方迁移

#### 3.3.1 `hyp_mem_unmap`

```c
int hyp_mem_unmap(paddr_t ipa)
{
    int ret = unmap_stage2_page(current, ipa, 1);  // do_free=1：释放物理页
    if (ret < 0) {
        WARN("hyp_mem_unmap: ipa 0x%lx not mapped", ipa);
        return -1;
    }
    INFO("hyp_mem_unmap: vm=%lu ipa=0x%lx", current->vmid, ipa);
    return 0;
}
```

#### 3.3.2 `shm_detach`

```c
int shm_detach(shm_id_t id)
{
    ...
    unmap_stage2_page(current, ipa, 0);  // do_free=0：只清映射，不释放物理页
    ...
}
```

共享内存是跨 VM 的物理页，detach 只是取消当前 VM 的映射，不能释放物理页（否则另一个 VM 就访问不到了）。

### 3.4 `free_page` 边界检查

```c
void free_page(struct page_pool *pool, paddr_t p)
{
    unsigned long idx = (p - pool->start_addr) / PAGE_SIZE;
    if (p < pool->start_addr || idx >= pool->page_nr) {
        PANIC("free_page: invalid pa 0x%lx (start=0x%lx nr=%lu idx=%lu)",
              p, pool->start_addr, pool->page_nr, idx);
    }
    pool->memap[idx] = 0;
}
```

Phase 19 及之前的 `free_page` 没有边界检查。如果传入非法地址，会导致 `memap` 数组越界写入，可能破坏相邻的全局变量。

### 3.5 `HVC_MEM_STATS` hypercall

```
HVC 编号：22
输入：x0 = 0（无参数）
输出：x0 = user_pages_count, x1 = kernel_pages_count
```

Guest 可以通过此 hypercall 获知当前 VM 的内存使用情况，用于测试验证和调试。

Guest 侧辅助函数：

```c
static inline u64 hvc_call_ret2(u64 nr, u64 arg0, u64 *ret1)
{
    register u64 x0 __asm__("x0") = arg0;
    register u64 x1 __asm__("x1");
    register u64 x8 __asm__("x8") = nr;
    asm volatile("hvc #0"
                 : "+r"(x0), "=r"(x1)
                 : "r"(x8)
                 : "memory");
    if (ret1)
        *ret1 = x1;
    return x0;
}
```

---

## 4. TPIDR_EL2 改进（附带）

### 4.1 问题

之前 `smp_processor_id()` 通过读 `MPIDR_EL1` 获取 CPU ID。但在 Hypervisor (EL2) 中，当 `HCR_EL2.VM=1` 时，读 `MPIDR_EL1` 可能返回 `VMPIDR_EL2` 中的虚拟值（而非物理 MPIDR），导致 per-CPU 的 `current` 指针、任务表等数据结构全部错乱。

### 4.2 方案

改用 `TPIDR_EL2`（EL2 专属的软件线程 ID 寄存器）存储物理 CPU ID：

- 主核启动时：在 `boot_virt.S` 的 `el2_entry` 开头设置 `tpidr_el2 = 0`
- 次核启动时：在 `secondary_el2` 中计算完 cpu_id 后设置 `tpidr_el2 = x0`
- `smp_processor_id()` 改为读 `tpidr_el2`

TPIDR_EL2 完全由软件控制，不受任何虚拟化影响。

---

## 5. 文件变更清单

| 文件 | 类型 | 说明 |
|------|------|------|
| `include/common/mm.h` | 修改 | 新增 `unmap_stage2_page()` 声明 |
| `hypervisor/common/mm.c` | 修改 | 新增 `page_table_empty()` + `unmap_stage2_page()`；`hyp_mem_unmap` 改为调用通用函数；`free_page` 增加边界检查 |
| `include/common/hvc.h` | 修改 | 新增 `HVC_MEM_STATS = 22` |
| `hypervisor/arch/aarch64/sync_exc.c` | 修改 | 新增 `HVC_MEM_STATS` case |
| `hypervisor/boards/virt/shmem.c` | 修改 | `shm_detach` 改为调用 `unmap_stage2_page` |
| `include/boards/virt/shmem.h` | 修改 | （如有必要） |
| `guests/el1_test/guest.h` | 修改 | 新增 `HVC_MEM_STATS` 宏 + `hvc_call_ret2()` 函数 |
| `guests/el1_test/main.c` | 修改 | 新增 `phase20_unmap_pt_reclaim_test()` |
| `hypervisor/arch/aarch64/task.c` | 修改 | 清理调试打印 |
| `hypervisor/boards/virt/guest_loader.c` | 修改 | 清理调试打印 |
| `hypervisor/boards/virt/main_virt.c` | 修改 | loader args 改为全局静态变量 |
| `hypervisor/boards/virt/boot_virt.S` | 修改 | 设置 `TPIDR_EL2` |
| `include/common/sched.h` | 修改 | `smp_processor_id()` 改为读 TPIDR_EL2 |

---

## 6. Guest 侧测试设计

### 6.1 测试函数：`phase20_unmap_pt_reclaim_test`

**测试地址**：`0x49000000`（远离 Guest 代码和其他测试区域）

**测试步骤**：

1. **初始计数基准**：调用 `HVC_MEM_STATS` 获取初始 user_pages / kernel_pages
2. **map 4 页**：连续映射 4 个普通内存页
3. **验证 map 计数**：
   - user_pages 应增加 4
   - kernel_pages 应增加（至少分配了 1 个 L3 页表页）
4. **逐页 unmap + 验证**：
   - 每 unmap 1 页后检查 user_pages 递减 1
   - 验证计数单调递减的正确性
5. **最终验证**：
   - user_pages 回到初始值
   - kernel_pages 回到初始值（验证 L3 页表页被回收）

### 6.2 预期输出

```
--- Test 13: Phase 20 Stage-2 页表管理完善 ---
  初始: user_pages=N, kernel_pages=M
  map 4页后: user_pages=N+4, kernel_pages=M+1
  unmap 4页后: user_pages=N, kernel_pages=M
  PASSED: Stage-2 unmap + 计数正确
```

---

## 7. 测试与验证

### 7.1 构建

```bash
make -f Makefile.virt all
make -C guests/el1_test clean    # 修改 .h 后手动清理 Guest
make -f Makefile.virt all
```

### 7.2 运行

```bash
timeout 60 make -f Makefile.virt run
```

### 7.3 验证标志

- `[vm1] Phase 19: ALL tests done`
- `[vm2] Phase 19: ALL tests done`
- `[vm1] --- Test 13: Phase 20 Stage-2 页表管理完善 ---`
- `[vm1] PASSED: Stage-2 unmap + 计数正确`
- `[vm2] PASSED: Stage-2 unmap + 计数正确`
- `[vm1] guest: all tests done, entering main loop`
- `[vm2] guest: all tests done, entering main loop`

### 7.4 全量回归结果

Phase 1 ~ Phase 19 全部通过，Phase 20 新测试通过。

---

## 8. 已知问题与后续工作

### 8.1 布局敏感的间歇性 bug（未解决）

在调查过程中发现一个**布局敏感的间歇性越界写入 bug**：

- **现象**：当 Guest 镜像大小达到 ~35KB 时，在特定代码布局下会出现 PC=0 导致的 Instruction Abort
- **触发条件**：依赖于 Hypervisor 和 Guest 的具体代码/数据布局
- **当前状态**：当前代码布局下不触发，全量测试通过
- **怀疑方向**：某个数组越界或野指针写入，当恰好覆盖到 `task_struct` 的 `pt_regs.pc` 字段时就会导致 PC=0
- **已加固**：`free_page` 增加了边界检查；`smp_processor_id()` 改用 TPIDR_EL2
- **后续工作**：需要更系统的动态检测（如 canary、内存保护）来定位越界写入的源头

### 8.2 后续 Phase 规划

- Phase 21：Stage-2 页表细粒度权限管理（只读、可执行等）
- Phase 22：内存超售与按需分配优化
- Phase 23：Ballooning / 内存热插拔

---

## 9. 总结

Phase 20 完成了 Stage-2 页表管理的完善：

1. **通用化**：提取了 `unmap_stage2_page()` 统一所有取消映射逻辑
2. **页表回收**：支持 L3→L2 自底向上回收，解决了页表页泄漏问题
3. **类型区分**：通过 `do_free` 参数区分普通内存页和 MMIO/共享内存页
4. **接口新增**：`HVC_MEM_STATS` 提供内存统计能力
5. **健壮性**：`free_page` 增加边界检查，`smp_processor_id()` 改用 TPIDR_EL2
6. **全量通过**：Phase 1-20 所有测试在 virt 平台 2 核 SMP 模式下全部通过
