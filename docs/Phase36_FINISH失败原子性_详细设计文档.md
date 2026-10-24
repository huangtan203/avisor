# Phase 36：FINISH 失败原子性详细设计

## 1. 背景与目标

Phase 33/34 已完成双 vCPU 迁移提交和源 VM 回收。Phase 36 处理 FINISH 在最后一轮脏页复制或目标 vCPU 准备阶段失败时的原子回滚，避免源 vCPU 身份、目标页表和迁移状态出现半提交。

目标：

- 失败返回 `-1`；
- 源 VM vCPU1 的身份、内存空间、设备数据和名称恢复；
- 目标 VM 本轮新增页全部回滚；
- 源 VM AF 脏页跟踪恢复；
- 事务冻结协议释放；
- 迁移回到 `MIGRATION_IDLE`，允许重新 START/PRECOPY/FINISH；
- 成功 FINISH 的提交语义不变。

## 2. 失败窗口

FINISH 的准备阶段顺序为：

1. VM 级事务冻结并确认 vCPU1；
2. 执行最后一轮 `MIG_OP_COLLECT_DIRTY`；
3. 保存 vCPU1 原始身份；
4. 暂时切换 vCPU1 到目标 VM；
5. 注册目标 vCPU；
6. 任何准备错误触发 abort；
7. 通过全部检查后才执行正式提交和源 VM 回收。

目标页表的临时新增页使用 Phase 35 的 migration-new 标记，因此 abort 可精确回收本轮资源，不影响目标 VM 基线映射。

## 3. 回滚算法

失败路径执行：

```text
restore vCPU1 mm/vmid/board_data/name
unregister destination vCPU
register source vCPU
rollback target migration-new pages
restore source Stage-2 AF
commit transaction rollback/release
clear migration source
state = MIGRATION_IDLE
abort_count++
return -1
```

恢复操作必须在迁移状态仍受事务冻结保护时完成，防止源或目标 vCPU 在中间状态重新运行。

## 4. 接口与测试

新增：

- `HVC_MIGRATE_FAIL_FINISH = 69`：注入一次准备阶段失败；
- `HVC_MIGRATE_ABORT_STATS = 70`：查询 abort 次数；
- `migration_fail_finish_once()`；
- `migration_finish_abort_stats()`。

Test 24 验证注入失败、目标页回滚、状态复位、abort 计数增加，以及同一源/目标可以重试并成功迁移。

## 5. 不变量

- abort 不释放迁移开始前已有的目标设备、共享内存和基线页；
- abort 不增加或减少源 VM 用户页、内核页统计；
- 成功路径不执行恢复操作；
- 失败注入为一次性，重试不会继承旧故障。
