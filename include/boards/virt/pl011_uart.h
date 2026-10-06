// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - PL011 UART 驱动
 *
 * PL011 是 ARM 公司设计的标准 UART 控制器，被 QEMU virt、
 * 树莓派（PL011 是 UART0，Mini UART 是 UART1）、
 * 以及很多真实 ARM SoC 广泛使用。
 *
 * 学习要点：
 *   PL011 比 Mini UART 功能更全，是工业级 UART 控制器的标准。
 *   掌握 PL011 编程比 Mini UART 更有通用价值。
 *
 * 寄存器偏移（参见 ARM PrimeCell UART (PL011) TRM）：
 *   0x00 UARTDR    - 数据寄存器
 *   0x18 UARTFR    - 标志寄存器
 *   0x24 UARTIBRD  - 整数波特率除数
 *   0x28 UARTFBRD  - 小数波特率除数
 *   0x2C UARTLCR_H - 行控制寄存器
 *   0x30 UARTCR    - 控制寄存器
 *   0x34 UARTIFLS  - FIFO 中断级别选择
 *   0x38 UARTIMSC  - 中断屏蔽设置/清除
 *   0x40 UARTMIS   - 屏蔽后的中断状态
 *   0x44 UARTICR   - 中断清除寄存器
 */

#pragma once

#include "boards/virt/base.h"
#include "common/types.h"

/* PL011 寄存器基地址（虚拟地址） */
#define PL011_UART0_BASE    UART_BASE

/* 寄存器偏移 */
#define UARTDR              (PL011_UART0_BASE + 0x00)
#define UARTFR              (PL011_UART0_BASE + 0x18)
#define UARTIBRD            (PL011_UART0_BASE + 0x24)
#define UARTFBRD            (PL011_UART0_BASE + 0x28)
#define UARTLCR_H           (PL011_UART0_BASE + 0x2C)
#define UARTCR              (PL011_UART0_BASE + 0x30)
#define UARTIFLS            (PL011_UART0_BASE + 0x34)
#define UARTIMSC            (PL011_UART0_BASE + 0x38)
#define UARTRIS             (PL011_UART0_BASE + 0x3C)
#define UARTMIS             (PL011_UART0_BASE + 0x40)
#define UARTICR             (PL011_UART0_BASE + 0x44)

/* UARTFR 标志位 */
#define UARTFR_TXFF         (1 << 5)    /* 发送 FIFO 满 */
#define UARTFR_RXFE         (1 << 4)    /* 接收 FIFO 空 */
#define UARTFR_BUSY         (1 << 3)    /* UART 忙 */

/* UARTLCR_H 行控制位 */
#define UARTLCR_H_FEN       (1 << 4)    /* 使能 FIFO */
#define UARTLCR_H_WLEN_8    (3 << 5)    /* 8 位数据 */

/* UARTCR 控制位 */
#define UARTCR_UARTEN       (1 << 0)    /* UART 使能 */
#define UARTCR_TXE          (1 << 8)    /* 发送使能 */
#define UARTCR_RXE          (1 << 9)    /* 接收使能 */

/* 中断位（UARTIMSC / UARTMIS / UARTICR） */
#define UART_RXIM           (1 << 4)    /* 接收中断 */
#define UART_TXIM           (1 << 5)    /* 发送中断 */
#define UART_RTIM           (1 << 6)    /* 接收超时中断 */

/* QEMU virt 平台 PL011 的中断号（SPI 中断，需要加 32） */
#define VIRT_UART0_IRQ      (33 + 32)   /* SPI #33 -> GIC 中断号 65 */

/* 函数声明 */
void uart_init(void);
void uart_send(char c);
char uart_recv(void);
int uart_is_rx_ready(void);
void handle_uart_irq(void);
void _putchar(char c);
