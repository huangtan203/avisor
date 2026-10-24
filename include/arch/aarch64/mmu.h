// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor
 *
 * A Tiny Hypervisor for IoT Development
 *
 * Copyright (c) 2022 Deng Jie (mr.dengjie@gmail.com).
 */

#pragma once

#define MM_TYPE_PAGE_TABLE 0x3
#define MM_TYPE_PAGE	   0x3
#define MM_TYPE_BLOCK	   0x1

#define MM_ACCESS (1 << 10)
#define MM_nG	  (0 << 11)
#define MM_SH	  (3 << 8)

/*
 * Memory region attributes:
 *
 *   n = AttrIndx[2:0]
 *			n	MAIR
 *   DEVICE_nGnRnE	    000	00000000
 *   NORMAL_CACHEABLE		001	11111111
 */
#define MT_DEVICE_nGnRnE    0x0
#define MT_NORMAL_CACHEABLE 0x1

#define MT_DEVICE_nGnRnE_FLAGS	  0x00
#define MT_NORMAL_CACHEABLE_FLAGS 0x44 //TODO: temp. non-cacheable

#define MAIR_VALUE                                           \
	(MT_DEVICE_nGnRnE_FLAGS << (8 * MT_DEVICE_nGnRnE)) | \
		(MT_NORMAL_CACHEABLE_FLAGS << (8 * MT_NORMAL_CACHEABLE))

#define MMU_FLAGS \
	(MM_TYPE_BLOCK | (MT_NORMAL_CACHEABLE << 2) | MM_nG | MM_SH | MM_ACCESS)
#define MMU_DEVICE_FLAGS \
	(MM_TYPE_BLOCK | (MT_DEVICE_nGnRnE << 2) | MM_nG | MM_SH | MM_ACCESS)

#define MM_STAGE2_ACCESS  (1 << 10)
#define MM_STAGE2_SH	  (3 << 8)
#define MM_STAGE2_AP	  (3 << 6)
#define MM_STAGE2_MEMATTR (0x4 << 2) // Normal, Inner Non-cacheable, Outer Non-cacheable

#define MMU_STAGE2_PAGE_FLAGS                                            \
	(MM_TYPE_PAGE | MM_STAGE2_ACCESS | MM_STAGE2_SH | MM_STAGE2_AP | \
	 MM_STAGE2_MEMATTR)

/*
 * Phase 22: Stage-2 2MB Block 映射标志
 *
 * Stage-2 L2 可以直接做 block 映射（2MB 粒度），跳过 L3 表：
 *   - bit[1:0] = 01 → block descriptor
 *   - 其他位含义与 page descriptor 相同
 *   - 物理地址必须 2MB 对齐（bit[20:0] 为 0）
 *
 * 好处：
 *   1. 节省 1 级页表（每个 2MB 大页节省 4KB 的 L3 表）
 *   2. TLB 命中后性能更高（单条 TLB 覆盖 2MB 而非 4KB）
 *   3. 减少 translation fault 的次数（一次 fault 映射 2MB）
 */
#define MMU_STAGE2_BLOCK_FLAGS                                            \
	(MM_TYPE_BLOCK | MM_STAGE2_ACCESS | MM_STAGE2_SH | MM_STAGE2_AP | \
	 MM_STAGE2_MEMATTR)

#define MMU_STAGE2_BLOCK_AP_MASK  (3 << 6)

/*
 * Phase 21: Stage-2 细粒度权限常量
 *
 * Stage-2 AP[2:1] 位在 descriptor bit[7:6]，共 2 位：
 *   AP[2:1]  | 读 | 写
 *   ---------|----|----
 *     00     | 无 | 无  (NONE)
 *     01     | 允许 | 无  (RO)
 *     10     | 允许 | 允许 (RW)
 *     11     | 允许 | 允许 (RW，同 10)
 *
 * 注意：和 Stage-1 AP 编码不同！Stage-2 的 RO 是 01，不是 10。
 *
 * QEMU 实现注意：实测 QEMU 中 AP=10 时读操作会异常触发 Permission Fault，
 * 与 ARM 架构手册描述不符。统一使用 AP=11 表示 RW 以兼容 QEMU。
 */
#define S2_AP_RW    (3 << 6)   /* 11 = RW */
#define S2_AP_RO    (1 << 6)   /* 01 = RO */
#define S2_AP_NONE  (0 << 6)   /* 00 = NONE */
#define S2_AP_MASK  (3 << 6)

#define MM_STAGE2_AP_NONE	 (0 << 6)
#define MM_STAGE2_DEVICE_MEMATTR (0x0 << 2)

/* Phase 24: KSM 共享页标记（使用 Stage-2 descriptor 的 IGNORED bit 55）
 * ARMv8 Stage-2 page/block descriptor 的 bits [55:50] 是 IGNORED，
 * 软件可自由使用。用 bit 55 标记 KSM 共享页，用于 permission fault
 * 时快速判断是否需要 CoW 分离。
 */
#define MM_STAGE2_KSM_SHARED (1ULL << 55)

/* Phase 35: 迁移期间目标 VM 新分配页的临时标记（取消时回收） */
#define MM_STAGE2_MIGRATION_NEW (1ULL << 54)

/*
 * Stage-2 页表 descriptor 的物理地址掩码。
 *
 * 48-bit IPA 下，Stage-2 descriptor 的 Output Address 是 bits[47:12]。
 * 注意不能用 PAGE_MASK（0xfffffffffffff000），因为 Stage-2 descriptor
 * 的高位 bit[63:48] 是 IGNORED / 软件可用位（如 KSM 的 bit 55），
 * 必须一并清掉，否则提取出的 PA 会带着 bit 55 等高位，导致读非法地址。
 */
#define MM_STAGE2_PA_MASK (0x0000FFFFFFFFF000ULL)
#define MMU_STAGE2_MMIO_PAGE_FLAGS                                            \
	(MM_TYPE_PAGE | MM_STAGE2_ACCESS | MM_STAGE2_SH | MM_STAGE2_AP_NONE | \
	 MM_STAGE2_DEVICE_MEMATTR)

/*
 * Device 内存 + 完全访问权限（PCI 直通用）
 *
 * 与 MMU_STAGE2_MMIO_PAGE_FLAGS 的区别：后者 AP=00（无访问权限），
 * 用于让 MMIO 地址陷入 EL2 做 trap-and-emulate；直通则需要 Guest
 * 直接访问真实设备，因此用 AP=11（读/写） + device 内存属性。
 */
#define MMU_STAGE2_DEVICE_PAGE_FLAGS                                          \
	(MM_TYPE_PAGE | MM_STAGE2_ACCESS | MM_STAGE2_SH | MM_STAGE2_AP |      \
	 MM_STAGE2_DEVICE_MEMATTR)


#define TCR_T0SZ   (64 - 48)
#define TCR_TG0_4K (0 << 14)
#define TCR_VALUE  (TCR_T0SZ | TCR_TG0_4K)
