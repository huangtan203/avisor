/* Tegra234 bring-up image: firmware must load the raw image at 0x80080000. */
SECTIONS
{
  . = 0x80080000;

  .text.boot : { *(.text.boot) }
  . = ALIGN(0x00001000);

  el1_test_begin = .;
  .text.user : { build/el1_* (.text) }
  .rodata.user : { build/el1_* (.rodata) }
  .data.user : { build/el1_* (.data) }
  .bss.user : { build/el1_* (.bss) }
  el1_test_end = .;

  .text : { *(.text) }
  .rodata : { *(.rodata) }
  .data : { *(.data) }
  image_payload_end = .;

  . = ALIGN(0x8);
  bss_begin = .;
  .bss : { *(.bss*) }
  bss_end = .;

  . = ALIGN(0x00001000);
  pg_dir = .;
  .data.pgd : { . += (128 * (1 << 12)); }
  image_end = .;
}
