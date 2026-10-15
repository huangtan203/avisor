// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - Live Migration（热迁移）
 *
 * Phase 25: 脏页跟踪 + 状态保存/恢复 + 迭代预拷贝
 *
 * 迁移模型（pre-copy）：
 *   源 VM（如 VM2）通过 3 个 HVC 分阶段把自身迁移到目标 VM（如 VM3）：
 *     1. START    : 清除全部 Stage-2 AF 位（开启脏页跟踪）+ 全量拷贝内存
 *     2. PRECOPY  : 收集 AF=1 的脏页并拷贝（可多轮迭代，逐轮收敛）
 *     3. FINISH   : 最后一轮脏页拷贝 + 保存/转移 vCPU 状态 + 停止源 VM
 *                   并调度到目标 VM 恢复执行
 *
 *   脏页跟踪复用 Stage-2 描述符的 AF 位（bit 10）：清除 AF 后，Guest 再次
 *   访问该页会触发 Access Flag Fault 自动置位 AF（handle_access_flag_fault），
 *   迁移时收集 AF=1 的页即为"自上次清 AF 后被访问过"的脏页。
 */

#pragma once

#include "common/sched.h"
#include "common/mm.h"

struct pt_regs;

/* 迁移目标 VM 设置（main_virt.c 创建 dormant VM3 后调用） */
void migration_set_dest(struct task_struct *dst);

/* 迁移三阶段：start / precopy / finish，返回 0 成功、-1 失败 */
int migration_start(struct task_struct *src);
int migration_precopy(struct task_struct *src);
int migration_finish(struct task_struct *src);

/* 目标 VM 的 resume loader：恢复迁移过来的 pt_regs */
int migration_resume_loader(void *arg, struct pt_regs *regs);
