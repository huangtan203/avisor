// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor
 *
 * A Tiny Hypervisor for IoT Development
 *
 * Copyright (c) 2022 Deng Jie (mr.dengjie@gmail.com).
 */

.globl irq_vector_init
irq_vector_init:
	adr x0, vectors		/* load VBAR_EL2 with virtual address */
	msr vbar_el2, x0	/* vector table address */
	ret

.globl enable_irq
enable_irq:
	msr daifclr, #2		/* 清除 I 位（IRQ mask）
				   注意：DAIF 顺序是 D(9) A(8) I(7) F(6)
				   daifclr 立即数 bit 顺序是 F(0) I(1) A(2) D(3)
				   所以 #2 = bit 1 = I（IRQ） */
	ret

.globl disable_irq
disable_irq:
	msr daifset, #2		/* 设置 I 位（IRQ mask） */
	ret
