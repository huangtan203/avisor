# Phase 54：多物理区间页池与跨区间分配

## 1. 阶段目标

将 page pool 从单一连续物理区间扩展为最多 8 个独立物理区间，同时保持调用方接口不变：

- 普通页分配支持多个区间；
- 普通页释放按物理地址定位所属区间；
- 大页分配只允许在同一个区间内形成连续块；
- 大页释放拒绝跨越区间边界；
- 每个区间拥有独立 cursor 和 bitmap；
- QEMU 默认配置不满足安全接入条件时，自动保留旧连续池。

## 2. 数据结构

新增：

```c
#define PAGE_POOL_MAX_RANGES 8

struct page_pool_range {
    paddr_t start_addr;
    uint64_t page_nr;
    uint8_t *memap;
    uint64_t last_page_id;
};
```

`struct page_pool` 保留原有字段，并增加：

```c
struct page_pool_range ranges[PAGE_POOL_MAX_RANGES];
unsigned int range_count;
```

这样旧平台仍可使用原有 `start_addr/page_nr/memap/last_page_id` 路径，QEMU 只有在显式初始化成功后才使用区间路径。

## 3. 区间初始化

`virt_page_pool_init()` 在 `platform_early_probe()` 后调用：

1. 检查 `page_pool_plan_safe`；
2. 将每个规范化 free range 裁剪到 `[LOW_MEMORY, HIGH_MEMORY)`；
3. 检查有效区间数量不超过 8，且总页数不超过静态 bitmap 容量；
4. 在原有静态 bitmap 中为每个区间规划独立片段；
5. 初始化区间 cursor 和轮转起点；
6. 完整规划成功后一次性发布；任一步骤失败则保持 `range_count=0`，回退原固定连续池。

区间初始化从规范化的 `free[]` 与固定可映射页池 `[LOW_MEMORY, HIGH_MEMORY)` 求交集，过滤 Hypervisor image、启动栈、DTB 与固件保留区。初始化先在局部数组完整验证区间数量和 bitmap 容量，最后一次性发布；任一检查失败都保持 `range_count=0`，回退旧连续池。

标准 QEMU DTB 的单个最大候选区不覆盖旧固定池，但规范化 free map 在固定池中包含两个安全区间。运行时实际启用结果：

```text
page-pool=planned ranges=2 pages=257792
```

普通页分配采用区间轮转，日志可见物理页在两个区间地址间交替，且不落入 DTB/保留区或两段之间的空洞。

## 4. 普通页分配与释放

区间模式下，`get_free_page()` 依次扫描每个区间，从该区间 cursor 开始寻找空闲页。分配地址为：

```text
range.start_addr + index * PAGE_SIZE
```

`free_page()` 逐区间检查物理地址是否位于范围内且页对齐，匹配后只清除所属区间 bitmap。非法地址或非页对齐地址触发 PANIC，避免释放到错误 bitmap。

## 5. 大页分配与释放

大页分配在单个区间内执行：

- 起始物理地址必须满足 `npages * PAGE_SIZE` 对齐；
- 连续页不能跨越区间边界；
- 每个区间独立检查 bitmap；
- 成功后只更新该区间 cursor。

因此不连续空闲区不会被错误拼接成物理连续大页。

大页释放先定位所属区间，再检查 `start_idx + npages` 不超过该区间末尾。跨区间块或非法地址会被拒绝。

## 6. 并发安全

现有 `page_pool_lock` 继续覆盖：

- 普通页分配/释放；
- 任务页分配；
- 大页分配/释放。

每个区间的 bitmap 和 cursor 都在同一把锁下访问，不引入新的锁顺序或跨 CPU 竞态。

## 7. KSM 回归修复

多区间轮转使迁移测试更稳定地暴露了一个既有 KSM 元数据查找问题：共享页元数据按合并时的内容哈希挂入桶，但页面内容可能在后续流程中变化。释放已知物理页时若按当前内容重算哈希，只搜索一个桶，可能找不到原元数据并令 `ksm_drop_vm()` 返回 `-1`。

`ksm_cow_break()` 与 `ksm_drop_vm()` 现按物理地址遍历全部 KSM 桶查找共享页。删除时仍使用元数据保存的原始 `kp->hash`，保证链表摘除位置正确。

## 8. 验证结果

### 构建

- `make -f Makefile.virt -B -j$(nproc)`：通过；
- `git diff --check`：通过；
- Raspberry Pi C 源码已通过本阶段涉及的 page pool 类型与 UART per-CPU task 表编译；完整构建继续被既有 `boot.S` 展开出的 `UL` 整数字面量汇编错误阻塞，与本阶段页池改动无关。

### 标准 QEMU GICv3 DTB

```text
[platform] page-pool=planned ranges=2 pages=257792
[vm1] PASSED: 大页 + 内存配额正确
[vm1] PASSED: 页面回收功能正确
[vm1] PASSED: KSM 内存去重 + CoW 功能正确
[vm2] PASSED: 大页 + 内存配额正确
[vm2] PASSED: 页面回收功能正确
[vm2] PASSED: KSM 内存去重 + CoW 功能正确
migration: source VM reclaimed (pages=28 shm=0 ksm=227 done=1)
```

Phase 18–27 回归通过，包括 Stage-2、KSM/CoW、页面回收、大页分配、vSGI、GICR、冻结协议、双 vCPU 迁移、源 VM 回收、zombie 回收和 task 槽位复用。日志中没有 `FAILED` 或 `PANIC`。

## 9. 当前边界

- 单次连续分配不会跨越物理区间；即使相邻区间在地址上连续，也按独立区间处理。
- 区间数量固定上限为 8，总容量受静态 `PAGING_PAGES` bitmap 限制。
- allocator 只采用固定已映射窗口 `[LOW_MEMORY, HIGH_MEMORY)` 内的规范化 free range；窗口外内存不参与分配。
- `range_count=0` 继续表示旧连续池模式，保留非 virt 平台路径。
