// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - PL011 UART 驱动实现
 *
 * PL011 是 ARM PrimeCell UART，是 ARM 平台最标准的串口控制器。
 *
 * 学习要点：
 *   1. UART 初始化流程：波特率设置 -> 数据位/停止位配置 -> 使能发送/接收
 *   2. 查询式发送：循环等待 TX FIFO 非满，然后写数据寄存器
 *   3. 查询式接收：循环等待 RX FIFO 非空，然后读数据寄存器
 *   4. 中断式接收：使能 RX 中断，在中断处理函数里读数据
 *
 * 与树莓派 Mini UART 的区别：
 *   - PL011 是全功能 UART，有独立的波特率发生器
 *   - PL011 有 16 字节深度的 FIFO（Mini UART 只有 8 字节）
 *   - PL011 支持硬件流控（CTS/RTS）
 *   - PL011 寄存器布局更规范
 */

#include "boards/virt/pl011_uart.h"
#include "boards/virt/gicv3.h"
#include "common/printf.h"
#include "common/task.h"
#include "common/utils.h"

/*
 * 当前 UART 转发到哪个任务
 * 0 = Hypervisor 自己的控制台
 * >0 = 对应 guest VM 的控制台
 *
 * 注意：这个变量定义在 task.c 中（原始 raspi 代码也在那里），
 * 这里只做声明。
 */
extern int uart_forwarded_task;

/* 判断某个任务是否是当前 UART 转发的目标 */
int is_uart_forwarded_task(struct task_struct *tsk)
{
    return tsk->pid == uart_forwarded_task;
}

/*
 * Phase 1：UART 中断处理暂用简化版
 * （等 shell 移植好了再启用完整的中断处理）
 */
#ifdef VIRT_SHELL_ENABLED
#include "common/fifo.h"
#include "common/sched.h"
#include "common/shell.h"
#include "common/task.h"

extern int uart_forwarded_task;
#endif

/*
 * 查询式发送一个字符
 * 等待发送 FIFO 不满，然后写入数据寄存器
 */
void uart_send(char c)
{
    /* 等待 TX FIFO 有空间 */
    while (get32(UARTFR) & UARTFR_TXFF)
        ;

    /* 写入数据寄存器 */
    put32(UARTDR, c);
}

/*
 * 查询式接收一个字符（阻塞等待）
 */
char uart_recv(void)
{
    /* 等待 RX FIFO 有数据 */
    while (get32(UARTFR) & UARTFR_RXFE)
        ;

    /* 读取数据寄存器 */
    return get32(UARTDR) & 0xFF;
}

/*
 * 非阻塞检查是否有数据可读
 */
int uart_is_rx_ready(void)
{
    return !(get32(UARTFR) & UARTFR_RXFE);
}

/*
 * UART 接收中断处理
 *
 * 学习要点：
 *   在 Hypervisor 中，串口中断需要决定：
 *   - 如果当前控制台在 Hypervisor，字符送给 shell
 *   - 如果当前控制台切到了某个 VM，字符送入 VM 的输入 FIFO
 *   这就是"串口复用"（UART muxing）的概念。
 *
 * Phase 1：简化版，只打印收到的字符（回显）
 * Phase 2：完整版本，支持 shell 和 VM 控制台切换
 */
#ifdef VIRT_SHELL_ENABLED
void handle_uart_irq(void)
{
    int tsk_id;
    /* 0 表示 Hypervisor 自己的控制台 */
    if (uart_forwarded_task == 0) {
        shell_kick();
    } else {
        static int is_escaped = 0;

        char received = get32(UARTDR) & 0xff;

        struct task_struct *tsk;
        if (is_escaped) {
            is_escaped = 0;
            if (isdigit(received)) {
                tsk_id = received - '0';
                if (tsk_id > nr_tasks - 1)
                    goto clear_int;
                uart_forwarded_task = tsk_id;
                printf("\nSwitched to console: %d\n",
                       uart_forwarded_task);
                tsk = task[uart_forwarded_task];
                if (tsk->state == TASK_RUNNING)
                    flush_task_console(tsk);
            } else if (received == 'l') {
                show_task_list();
            } else if (received == '@') {
                goto enqueue_char;
            }
        } else if (received == '@') {
            is_escaped = 1;
        } else {
enqueue_char:
            tsk = task[uart_forwarded_task];
            if (tsk->state == TASK_RUNNING)
                enqueue_fifo(tsk->console.in_fifo, received);
        }
    }

clear_int:
    /* PL011 的接收中断是自动清除的（读取 DR 就清除了）
     * 读空 RX FIFO，确保中断被清除 */
    while (!(get32(UARTFR) & UARTFR_RXFE)) {
        (void)get32(UARTDR);
    }
}
#else
/* Phase 1 简化版：读一个字符并回显 */
void handle_uart_irq(void)
{
    while (!(get32(UARTFR) & UARTFR_RXFE)) {
        char c = get32(UARTDR) & 0xff;
        /* 回显 */
        uart_send(c);
        if (c == '\r')
            uart_send('\n');
    }
}
#endif

/*
 * UART 初始化
 *
 * 学习要点：
 *   PL011 初始化步骤：
 *   1. 关闭 UART（UARTCR.UARTEN = 0）
 *   2. 设置波特率（整数部分 + 小数部分）
 *   3. 设置行控制（数据位、停止位、FIFO使能）
 *   4. 使能 TX/RX
 *   5. 使能 UART
 *   6. 使能中断
 *
 * 波特率计算公式（来自 PL011 TRM）：
 *   BAUDDIV = UARTCLK / (16 * BaudRate)
 *   整数部分: IBRD = floor(BAUDDIV)
 *   小数部分: FBRD = floor(frac(BAUDDIV) * 64 + 0.5)
 *
 * QEMU 的 PL011 时钟频率通常是 24MHz。
 */
void uart_init(void)
{
    unsigned int uartclk = 24000000;  /* QEMU virt PL011 时钟：24MHz */
    unsigned int baudrate = 115200;
    unsigned int bauddiv;
    unsigned int ibrd, fbrd;
    unsigned int lcr_h;

    /* 1. 关闭 UART，便于配置 */
    put32(UARTCR, 0);

    /* 2. 计算并设置波特率 */
    bauddiv = uartclk * 4 / baudrate;  /* 简化计算 */
    ibrd = bauddiv >> 6;               /* 整数部分 */
    fbrd = bauddiv & 0x3F;             /* 小数部分 (6位) */

    put32(UARTIBRD, ibrd);
    put32(UARTFBRD, fbrd);

    /* 3. 设置行控制：8位数据 + 使能 FIFO */
    lcr_h = UARTLCR_H_WLEN_8 | UARTLCR_H_FEN;
    put32(UARTLCR_H, lcr_h);

    /* 4 & 5. 使能 UART、TX、RX */
    put32(UARTCR, UARTCR_UARTEN | UARTCR_TXE | UARTCR_RXE);

    /* 6. 使能接收中断（通过 GIC 路由） */
    put32(UARTIMSC, UART_RXIM | UART_RTIM);

    /*
     * 注意：在 GIC 初始化之前调用 uart_init 的话，
     * 这里只是配置了 UART 内部的中断使能。
     * 真正的中断使能在 gic_init() 之后通过 gic_enable_irq() 完成。
     */
}

/*
 * printf 系统调用的底层输出函数
 * 整个系统的 printf 最终都调用到这里
 */
void _putchar(char c)
{
    /* 把 \n 转成 \r\n，符合串口终端习惯 */
    if (c == '\n')
        uart_send('\r');
    uart_send(c);
}
