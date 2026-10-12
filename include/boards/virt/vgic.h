// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - GICv3 虚拟化接口 (vGIC)
 *
 * 学习要点：
 *   GICv3 的虚拟化由硬件直接支持：
 *   - Guest 用标准 ICC_*_EL1 系统寄存器访问虚拟 CPU 接口（不 trap）
 *   - Hypervisor 用 ICH_LR<n>_EL2（List Register）注入虚拟中断
 *   - Guest 的 EOI 由硬件自动处理 LR 失效
 */

#pragma once

#include "common/types.h"

void vgic_init(void);
void vgic_inject_virq_hw(uint32_t vintid, uint32_t pintid);
void vgic_vtimer_defer(void);   /* HV 上下文收到 vtimer 中断：延迟注入 */
void vgic_vtimer_flush(void);   /* 回到 Guest 前补注入缓存的 vtimer 中断 */
uint64_t vgic_get_lr0(void);
