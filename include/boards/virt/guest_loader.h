// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - 内嵌 Guest 镜像加载器接口（virt 平台）
 */

#pragma once

#include "common/task.h"

/* 内嵌镜像尺寸上限（防止链接出错时越界拷贝） */
#define GUEST_MAX_IMAGE_SIZE (4 * 1024 * 1024)

struct embedded_binary_loader_args {
	unsigned long load_addr;   /* Guest IPA 加载地址 */
	unsigned long entry_point; /* Guest 入口 */
	unsigned long image_begin; /* 内嵌镜像起始（Hypervisor VA） */
	unsigned long image_end;   /* 内嵌镜像结束（Hypervisor VA） */
};

int embedded_binary_loader(void *arg, struct pt_regs *regs);
