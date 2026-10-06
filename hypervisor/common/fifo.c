// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor
 *
 * A Tiny Hypervisor for IoT Development
 *
 * Copyright (c) 2022 Deng Jie (mr.dengjie@gmail.com).
 */

#include "common/fifo.h"
#include "common/mm.h"

#define FIFO_SIZE 256 // warning: DO NOT exceed page size

struct fifo {
	unsigned int head;
	unsigned int tail;
	unsigned int used;
	unsigned long buf[FIFO_SIZE];
};

#define NEXT_INDEX(i) (((i) + 1) == FIFO_SIZE ? 0 : ((i) + 1))

int is_empty_fifo(struct fifo *fifo)
{
	return fifo->used == 0;
}

int is_full_fifo(struct fifo *fifo)
{
	return fifo->used == FIFO_SIZE;
}

static void dbg_putc(char c)
{
	volatile unsigned int *uart_fr = (volatile unsigned int *)(0x09000000 + 0x18);
	volatile unsigned int *uart_dr = (volatile unsigned int *)(0x09000000 + 0x00);
	while (*uart_fr & (1 << 5)) { }
	*uart_dr = c;
}

struct fifo *create_fifo()
{
	dbg_putc('f');
	struct fifo *fifo = (struct fifo *)allocate_page();
	dbg_putc('g');
	fifo->head = 0;
	fifo->tail = 0;
	fifo->used = 0;
	dbg_putc('h');

	return fifo;
}

void clear_fifo(struct fifo *fifo)
{
	fifo->head = 0;
	fifo->tail = 0;
	fifo->used = 0;
}

int enqueue_fifo(struct fifo *fifo, unsigned long val)
{
	if (is_full_fifo(fifo))
		return -1;

	fifo->buf[fifo->head] = val;
	fifo->head = NEXT_INDEX(fifo->head);
	fifo->used++;

	return 0;
}

int dequeue_fifo(struct fifo *fifo, unsigned long *val)
{
	if (is_empty_fifo(fifo))
		return -1;

	if (val)
		*val = fifo->buf[fifo->tail];

	fifo->tail = NEXT_INDEX(fifo->tail);
	fifo->used--;

	return 0;
}

int used_of_fifo(struct fifo *fifo)
{
	return fifo->used;
}
