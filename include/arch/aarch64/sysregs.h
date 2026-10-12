// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor
 *
 * A Tiny Hypervisor for IoT Development
 *
 * Copyright (c) 2022 Deng Jie (mr.dengjie@gmail.com).
 */

#pragma once

// ***************************************
// SCTLR_EL2, System Control Register (EL2)
// ***************************************

#define SCTLR_EE	       (0 << 25)
#define SCTLR_I_CACHE_DISABLED (0 << 12)
#define SCTLR_D_CACHE_DISABLED (0 << 2)
#define SCTLR_MMU_DISABLED     (0 << 0)
#define SCTLR_MMU_ENABLED      (1 << 0)

#define SCTLR_VALUE_MMU_DISABLED                                      \
	(SCTLR_EE | SCTLR_I_CACHE_DISABLED | SCTLR_D_CACHE_DISABLED | \
	 SCTLR_MMU_DISABLED)

// ***************************************
// HCR_EL2, Hypervisor Configuration Register (EL2)
// ***************************************

// trap related
#define HCR_TACR (1UL << 21)
#define HCR_TID3 (1UL << 18)
#define HCR_TID2 (1UL << 17)
#define HCR_TID1 (1UL << 16)
#define HCR_TWE	 (1UL << 14)
#define HCR_TWI	 (1UL << 13)
#define HCR_HCE	 (1UL << 8) // HVC instruction enable at EL1
// others
#define HCR_E2H	 (0UL << 34)
#define HCR_RW	 (1UL << 31)
#define HCR_TGE	 (0UL << 27)
#define HCR_AMO	 (1UL << 5) // routing to EL2
#define HCR_IMO	 (1UL << 4) // routing to EL2
#define HCR_FMO	 (1UL << 3) // routing to EL2
#define HCR_SWIO (1UL << 1)
#define HCR_VM	 (1UL << 0) // stage 2 translation enable

/*
 * 注意：所有位定义必须用 1UL（无符号）。
 * 如果用 (1 << 31)（有符号 int 负数），组合后的 HCR_VALUE 在
 * WRITE_SYSREG64 的 uint64_t 转换中发生符号扩展，高位全变 1，
 * 意外置位 E2H(VHE)/高位 trap 位——曾导致虚拟中断完全失灵。
 */
#define HCR_VALUE                                                              \
	(HCR_TACR | HCR_TID3 | HCR_TID2 | HCR_TID1 | HCR_TWE | HCR_TWI |       \
	 HCR_HCE | HCR_E2H | HCR_RW | HCR_TGE | HCR_AMO | HCR_IMO | HCR_FMO | \
	 HCR_SWIO | HCR_VM)

// SCR_EL3, Secure Configuration Register (EL3)
// ***************************************

#define SCR_RESERVED (3 << 4)
#define SCR_RW	     (1 << 10)
#define SCR_HCE	     (1 << 8)
#define SCR_NS	     (1 << 0)

#define SCR_VALUE (SCR_RESERVED | SCR_RW | SCR_HCE | SCR_NS)

// ***************************************
// SPSR_EL3, Saved Program Status Register (EL3)
// ***************************************

#define SPSR_MASK_ALL (7 << 6)
#define SPSR_EL2h     (9 << 0)

#define SPSR_VALUE (SPSR_MASK_ALL | SPSR_EL2h)

// ***************************************
// VTCR_EL2,  Virtualization Translation Control Register (EL2)
// ***************************************

#define VTCR_NSA   (1 << 30)
#define VTCR_NSW   (1 << 29)
#define VTCR_VS	   (0 << 19)
#define VTCR_PS	   (2 << 16)
#define VTCR_TG0   (0 << 14) // 4KB
#define VTCR_SH0   (3 << 12)
#define VTCR_ORGN0 (0 << 10)
#define VTCR_IRGN0 (0 << 8)
#define VTCR_SL0   (1 << 6)
#define VTCR_T0SZ  (64 - 38)

#define VTCR_VALUE                                                       \
	(VTCR_NSA | VTCR_NSW | VTCR_VS | VTCR_PS | VTCR_TG0 | VTCR_SH0 | \
	 VTCR_ORGN0 | VTCR_IRGN0 | VTCR_SL0 | VTCR_T0SZ)

// ***************************************
// GICv3 虚拟化寄存器 (ICH_*_EL2)
//
// 学习要点：
//   GICv3 原生支持中断虚拟化（不需要像 GICv2 那样额外配 GICH）：
//     - 虚拟 CPU 接口：Guest 在 EL1 直接访问 ICC_*_EL1 系统寄存器，
//       硬件自动重定向到"虚拟视图"（由 HCR_EL2.IMO/FMO 控制路由），
//       因此 Guest 的 GICv3 驱动无需任何修改。
//     - List Register (ICH_LR<n>_EL2)：EL2 向 Guest 注入虚拟中断的
//       通道，一个 LR 条目代表一个虚拟中断。
//     - ICH_VMCR_EL2：Guest 看到的 ICC_PMR/BPR/IGRPEN1 的后端。
//   这就是 GICv3 相对 BCM2837（软件模拟中断控制器）的巨大优势。
// ***************************************

#define ICH_HCR_EL2  S3_4_C12_C11_0 /* Hypervisor 控制接口 */
#define ICH_VTR_EL2  S3_4_C12_C11_6 /* 虚拟化特征（只读，LR 个数等） */
#define ICH_VMCR_EL2 S3_4_C12_C11_7 /* 虚拟机控制（PMR/BPR/GRPEN 后端） */
#define ICH_LR0_EL2  S3_4_C12_C12_0 /* List Register 0 */
#define ICH_LR1_EL2  S3_4_C12_C12_1 /* List Register 1 */

/* LR 数量（我们使用的数量，实际硬件可能更多） */
#define NR_VGIC_LRS   2

/* ICH_HCR_EL2 位 */
#define ICH_HCR_EL2_EN (1 << 0) /* 使能虚拟 CPU 接口 */

/* ICH_VMCR_EL2 位（Guest 视角 ICC_* 的后端，与 QEMU 编码一致） */
#define ICH_VMCR_VENG1 (1UL << 1)  /* ICC_IGRPEN1_EL1.EnableGrp1 */
#define ICH_VMCR_VPMR_SHIFT 24     /* ICC_PMR_EL1 在 bits[31:24] */

/* ICH_LR<n>_EL2 位（与 QEMU/KVM 的编码一致） */
#define ICH_LR_EOI     (1UL << 41) /* guest EOIR 时硬件去活对应物理中断 */
#define ICH_LR_GROUP1  (1UL << 60) /* 虚拟中断属于 Group 1 */
#define ICH_LR_HW      (1UL << 61) /* 映射到物理中断（pINTID [41:32]） */
#define ICH_LR_PENDING (1UL << 62) /* state: pending（state 字段 [63:62]） */
#define ICH_LR_ACTIVE  (1UL << 63) /* state: active */
#define ICH_LR_STATE_MASK (3UL << 62) /* state 字段整体掩码 */

/* 虚拟定时器相关 */
#define CNTVOFF_EL2  S3_4_C14_C0_3 /* 虚拟计数器偏移 */
#define CNTHCTL_EL2  S3_4_C14_C1_0 /* EL1 访问 CNTP/CNTV 的 trap 控制 */

/*
 * EL1 视角的虚拟定时器寄存器（EL2 可直接访问 = 访问"当前加载的
 * 那个 VM"的虚拟定时器；世界切换时用于保存/恢复 per-VM 状态）
 */
#define CNTV_CTL_EL0  S3_3_C14_C3_1  /* 虚拟定时器控制 */
#define CNTV_CVAL_EL0 S3_3_C14_C3_2  /* 虚拟定时器比较值 */

/* CNTV_CTL_EL0 位 */
#define CNTV_CTL_ENABLE  (1UL << 0)
#define CNTV_CTL_IMASK   (1UL << 1)
#define CNTV_CTL_ISTATUS (1UL << 2)

/* ICC_DIR_EL1：单独 deactivate 物理中断（需 EOImode=1 配合） */
#define ICC_DIR_EL1 S3_0_C12_C11_1

/* Flags for get_page_from_gva, gvirt_to_maddr etc */
#define GV2M_READ  (0u<<0)
#define GV2M_WRITE (1u<<0)
#define GV2M_EXEC  (1u<<1)

/* Indirect stringification.  Doing two levels allows the parameter to be a
 * macro itself.  For example, compile with -DFOO=bar, __stringify(FOO)
 * converts to "bar".
 */	
#define __stringify_1(x...) #x
#define __stringify(x...)   __stringify_1(x)

/* Access to system registers */
#define READ_SYSREG32(name) ((uint32_t)READ_SYSREG64(name))

#define WRITE_SYSREG32(v, name) WRITE_SYSREG64((uint64_t)v, name)

#define WRITE_SYSREG64(v, name) do {                    \
   	uint64_t _r = v;                                    \
    asm volatile("msr "__stringify(name)", %0" : : "r" (_r));       \
} while (0)
#define READ_SYSREG64(name) ({                          \
    uint64_t _r;                                        \
    asm volatile("mrs  %0, "__stringify(name) : "=r" (_r));         \
    _r; })

#define READ_SYSREG(name)     READ_SYSREG64(name)
#define WRITE_SYSREG(v, name) WRITE_SYSREG64(v, name)

