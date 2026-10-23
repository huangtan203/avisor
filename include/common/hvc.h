// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - 半虚拟化调用（Hypercall）接口
 *
 * 学习要点：
 *   Hypervisor 和 Guest 之间的"系统调用"叫 hypercall：
 *   Guest 执行 HVC 指令（Hypervisor Call），陷入 EL2。
 *   这是半虚拟化（paravirtualization）的核心机制，
 *   Xen/KVM 都用类似方式提供 console、IPI、内存管理等接口。
 *
 *   调用约定（本项目自定义，类似 Linux KVM 的 smccc 风格简化版）：
 *     x8 - hypercall 功能号
 *     x0~x7 - 参数 / 返回值
 *
 *   注意：entry.S 的 el01_sync 会把 x8 作为 hvc_nr 传入
 *   handle_sync_exception（项目约定，不走 ESR.ISS 的 HVC 立即数）。
 */

#pragma once

/* hypercall 功能号 */
#define HVC_CONSOLE_PUTC 1 /* x0 = 字符，输出到 Hypervisor 控制台 */
#define HVC_GET_TIMER_FREQ 2 /* 返回值 x0 = 定时器频率（Hz） */
#define HVC_GET_VMID 3 /* 返回值 x0 = 当前 VM 的 ID（pid，Phase 3） */
#define HVC_PCI_DEBUG 4 /* PCI 直通中断链路诊断 dump（Phase 7 调试用） */

/* Phase 10: 新增 hypercall */
#define HVC_SCHED_YIELD     10  /* 主动让出 CPU，返回 0 */
#define HVC_SCHED_GET_PRIO  11  /* 获取当前任务优先级，返回 prio */
#define HVC_SCHED_SET_PRIO  12  /* x0 = prio，设置优先级，返回 0/-1 */
#define HVC_MEM_MAP         20  /* 映射内存页：x0=ipa x1=flags，返回 0/-1 */
#define HVC_MEM_UNMAP       21  /* 取消映射：x0=ipa，返回 0/-1 */
#define HVC_MEM_STATS       22  /* 内存统计：返回 x0=user_pages x1=kernel_pages */
#define HVC_MEM_PROTECT     23  /* 修改页权限：x0=ipa x1=prot(0=RW 1=RO 2=NONE)，返回 0/-1 */

/* Phase 22: 内存超售与大页 hypercall */
#define HVC_MEM_QUERY        24  /* 查询内存使用：返回 x0=当前使用(字节) x1=配额(字节) x2=大页数 */
#define HVC_MEM_SET_QUOTA    25  /* 设置配额：x0=max_bytes，返回 0/-1 */
#define HVC_MEM_HUGE_MAP     26  /* 映射 2MB 大页：x0=ipa x1=prot，返回 0/-1 */
#define HVC_MEM_HUGE_UNMAP   27  /* 取消 2MB 大页映射：x0=ipa，返回 0/-1 */
#define HVC_MEM_HUGE_PROTECT 28  /* 修改 2MB 大页权限：x0=ipa x1=prot，返回 0/-1 */

/* Phase 23: 页面回收 hypercall */
#define HVC_MEM_RECLAIM_STATS 29  /* 查询回收统计：返回 x0=回收页数 x1=回收失败次数 */
#define HVC_MEM_RECLAIM       34  /* 主动回收：x0=目标页数，返回实际回收页数 */

/* Phase 24: KSM 内存去重 hypercall */
#define HVC_MEM_KSM_SCAN      35  /* 触发 KSM 扫描合并：x0=最大扫描页数，返回合并页数 */
#define HVC_MEM_KSM_STATS     36  /* 查询 KSM 统计：返回 x0=共享页 x1=共享映射数 x2=累计合并 x3=累计分离 */
#define HVC_SHM_CREATE      30  /* 创建共享内存区：x0=key x1=size，返回 shmid/-1 */
#define HVC_SHM_ATTACH      31  /* 附加共享内存：x0=shmid x1=ipa，返回 0/-1 */
#define HVC_SHM_DETACH      32  /* 分离共享内存：x0=shmid，返回 0/-1 */
#define HVC_SHM_NOTIFY      33  /* 通知对端：x0=shmid，返回 0/-1 */

/* Phase 12: 虚拟 SGI（vSGI）hypercall */
#define HVC_VSGI_SEND       40  /* 发送虚拟 SGI：x0=target_vcpu_id x1=sgi_id，返回 0/-1 */
#define HVC_VSGI_GET_CPUID  41  /* 获取当前 vCPU ID，返回 vcpu_id */

/* Phase 15: 虚拟 GICD 测试 hypercall */
#define HVC_GICD_INJECT     42  /* x0=vintid，直接经 vgic_inject_virq 注入，返回 0/-1 */

/* Phase 16: 虚拟 GICR 测试 hypercall */
#define HVC_GICR_INJECT     43  /* x0=vPPI(16-31)，直接经 vgic_inject_virq 注入，返回 0/-1 */

/* Phase 25: Live Migration 热迁移 hypercall */
#define HVC_MIGRATE_START    50  /* 开始迁移：清 AF + 全量拷贝，返回 0/-1 */
#define HVC_MIGRATE_PRECOPY  51  /* 迭代预拷贝：收集并拷贝脏页，返回脏页数 */
#define HVC_MIGRATE_FINISH   52  /* 完成迁移：stop-and-copy + 状态转移，不返回源 VM */
#define HVC_MIGRATE_STATUS   53  /* 查询全局迁移状态 */
#define HVC_SMP_FREEZE_BEGIN    54  /* x0=目标物理核，等待冻结 ACK */
#define HVC_SMP_FREEZE_RELEASE  55  /* 释放已冻结的目标物理核 */
#define HVC_SMP_FREEZE_ACK      56  /* x0=核号，返回该核 ACK generation */
#define HVC_SMP_FREEZE_PROGRESS 57  /* x0=核号，返回冻结处理次数 */
#define HVC_SMP_FREEZE_SNAPSHOT_STATUS 58 /* x0=核号，返回 x0=有效 x1=generation */
#define HVC_SMP_TXN_FREEZE_BEGIN    59 /* x0=目标物理核，事务冻结并等待 ACK */
#define HVC_SMP_TXN_FREEZE_COMMIT   60 /* 提交事务：目标核按保存现场恢复执行 */
#define HVC_SMP_TXN_FREEZE_ROLLBACK 61 /* 回滚事务：目标核从冻结点重新执行 */
#define HVC_SMP_TXN_FREEZE_STATUS   62 /* 查询事务状态：0=无 1=冻结 2=已提交 -1=已回滚 */
