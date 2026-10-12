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

#define MM_STAGE2_AP_NONE	 (0 << 6)
#define MM_STAGE2_DEVICE_MEMATTR (0x0 << 2)
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
