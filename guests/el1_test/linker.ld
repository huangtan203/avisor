/*
 * aVisor Guest - 链接脚本 (EL1 测试程序)
 *
 * Guest 链接在自己的 IPA 0x40000000（QEMU virt 的标准内核加载地址）。
 * 真实物理地址由 Hypervisor 的 Stage 2 页表决定——
 * 这正是虚拟化的本质：Guest 以为自己独占一台机器。
 */

SECTIONS
{
    . = 0x40000000;

    .text.start : { KEEP(*(.text.start)) }
    .text       : { *(.text*) }
    .rodata     : { *(.rodata*) }
    .data       : { *(.data*) }

    . = ALIGN(8);
    __bss_start = .;
    .bss        : { *(.bss*) *(COMMON) }
    . = ALIGN(8);
    __bss_end = .;
}
