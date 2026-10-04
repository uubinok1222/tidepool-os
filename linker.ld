/* linker.ld - bản đồ bộ nhớ của Tidepool OS (x86_64, nạp tại 1 MiB bởi GRUB). */
ENTRY(_start)

SECTIONS
{
    . = 1M;

    /* Header Multiboot2 phải nằm trong 32 KiB đầu của file nhân */
    .boot : ALIGN(8) { KEEP(*(.multiboot_header)) }

    .text   : ALIGN(4K) { *(.text .text.*) }
    .rodata : ALIGN(4K) { *(.rodata .rodata.*) }
    .data   : ALIGN(4K) { *(.data .data.*) }
    .bss    : ALIGN(4K) { *(COMMON) *(.bss .bss.*) }

    . = ALIGN(4K);
    _kernel_end = .;

    /DISCARD/ : { *(.eh_frame) *(.note .note.*) *(.comment) }
}
