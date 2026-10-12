// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * aVisor Hypervisor - 内嵌 Guest 镜像
 *
 * 学习要点：
 *   把 Guest 的裸机二进制（el1_test.bin）直接嵌进 Hypervisor 镜像，
 *   运行时由 loader 拷贝到分配给 VM 的物理页（经 Stage 2 映射后
 *   成为 Guest 视角的 IPA 0x40000000）。
 *
 *   这对应树莓派版本从 SD 卡 FAT 分区加载 guest .bin 的场景——
 *   QEMU virt 没有现成的 FAT 加载链路，内嵌是最简单的起步方式。
 *
 *   GUEST_BIN_PATH 由 Makefile 传入（-DGUEST_BIN_PATH="..."），
 *   .incbin 的相对路径相对于 make 执行时的 CWD（项目根目录）。
 */

.section .text.user

.globl el1_test_begin
el1_test_begin:
    .incbin GUEST_BIN_PATH

.globl el1_test_end
el1_test_end:
