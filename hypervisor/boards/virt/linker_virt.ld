/*
 * aVisor Hypervisor - QEMU virt 平台链接脚本
 *
 * 学习要点：
 *   QEMU virt 平台的 DRAM 从 0x40000000 开始，
 *   QEMU 把内核加载到 0x40000000 处执行。
 *
 *   内存布局（虚拟地址 = 物理地址，等值映射）：
 *   0x4000_0000  --+  .text.boot  (启动代码)
 *                  |  .text       (代码段)
 *                  |  .rodata     (只读数据)
 *                  |  .data       (数据段)
 *                  |  .bss        (未初始化数据)
 *                  |  pg_dir      (页表，4KB 对齐)
 *                  |  内核栈      (从高地址向低地址增长)
 *   LOW_MEMORY   --+
 *                  |  页池（可分配页面）
 *   HIGH_MEMORY  --+
 *
 * 注意：
 *   和 raspi 版本不同，这里链接地址是 0x40000000 而不是 0x0。
 *   因为 QEMU virt 平台的 DRAM 从 0x40000000 开始，
 *   0x0 地址是外设区域（Flash、GIC 等），不能用来存代码。
 */

SECTIONS
{
  /*
   * QEMU virt 平台的 -kernel 参数会把镜像加载到 DRAM_BASE + 0x80000
   * （即 0x40080000），这是 ARM64 Linux 内核的标准加载偏移。
   * 所以链接地址必须是 0x40080000，而不是 0x40000000。
   */
  . = 0x40080000;

  .text.boot : { *(.text.boot) }
  . = ALIGN(0x00001000);

  el1_test_begin = .;
  .text.user : { build/el1_* (.text) }
  .rodata.user : { build/el1_* (.rodata) }
  .data.user : { build/el1_* (.data) }
  .bss.user : { build/el1_* (.bss) }
  el1_test_end = .;

  .text :  { *(.text) }
  .rodata : { *(.rodata) }
  .data : { *(.data) }

  . = ALIGN(0x8);
  bss_begin = .;
  .bss : { *(.bss*) }
  bss_end = .;

  . = ALIGN(0x00001000);
  pg_dir = .;
  .data.pgd : { . += (5 * (1 << 12)); }
}
