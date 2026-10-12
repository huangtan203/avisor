// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - 内嵌 Guest 镜像加载器（virt 平台）
 *
 * 学习要点：
 *   加载一个 Guest VM 需要：
 *     1. 为 Guest 的每一页 IPA 分配一个真实物理页（页池）
 *     2. 建立 Stage 2 映射：IPA -> 物理页（map_stage2_page）
 *     3. 把镜像内容拷进物理页（Hypervisor 视角的等值映射地址）
 *     4. 设置入口 PC（ERET 后 Guest 从这里开始执行）
 *
 *   对应树莓派版本的 raw_binary_loader（从 FAT 文件读），
 *   这里改从 Hypervisor 镜像内嵌的 .text.user 段读。
 *
 *   注意：Guest 的栈不在这里映射——Guest 第一次压栈时触发
 *   Stage 2 translation fault，handle_mem_abort 会按需分配
 *   （demand paging），这正是"懒分配"的演示。
 */

#include "common/mm.h"
#include "common/sched.h"
#include "common/task.h"
#include "common/utils.h"

#include "boards/virt/guest_loader.h"

/* 内嵌 Guest 镜像的边界（guest_image.S 中定义） */
extern char el1_test_begin[];
extern char el1_test_end[];

/*
 * 加载器参数在 hypervisor_main 中构造，
 * 这里只负责"把字节搬到 Guest 的物理页 + 建映射"。
 */
int embedded_binary_loader(void *arg, struct pt_regs *regs)
{
	struct embedded_binary_loader_args *args = arg;
	unsigned long src = (unsigned long)args->image_begin;
	unsigned long len = args->image_end - args->image_begin;
	unsigned long ipa = args->load_addr;

	if (len == 0 || len > GUEST_MAX_IMAGE_SIZE)
		return -1;

	for (unsigned long off = 0; off < len; off += PAGE_SIZE) {
		/*
		 * allocate_task_page：
		 *   从页池取一个物理页 + 在 current VM 的
		 *   Stage 2 表中映射 IPA = (ipa + off) -> 该物理页。
		 * 返回值是物理页的等值映射虚拟地址（可直接 memcpy 目标）。
		 */
		void *page = allocate_task_page(current, ipa + off);

		if (!page)
			return -1;

		unsigned long chunk = len - off;

		if (chunk > PAGE_SIZE)
			chunk = PAGE_SIZE;

		memcpy(page, (void *)(src + off), chunk);
	}

	(void)strncpy(current->name, "el1_test", 36);

	/* 入口 = 加载地址（guest 的 start.S 链接在段首） */
	regs->pc = args->entry_point;
	/*
	 * sp 不在这里设置：kernel_exit 不会恢复 pt_regs.sp，
	 * Guest 的 start.S 会自己设栈（GUEST_STACK_TOP）。
	 */
	regs->sp = 0;

	return 0;
}
