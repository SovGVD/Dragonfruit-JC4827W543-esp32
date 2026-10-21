/* Flat "ROM" image for the 68k benchmark workload. It is mapped where the Palm
 * ROM lives (BUS_ROM_BASE) so the JIT treats it exactly like ROM code; data and
 * bss are copied/cleared into emulated RAM by crt0. */
ENTRY(_start)
MEMORY {
    rom (rx) : ORIGIN = 0x10C08000, LENGTH = 256K
    ram (rw) : ORIGIN = 0x00001000, LENGTH = 1M
}
SECTIONS {
    .text : {
        LONG(0x000F0000)          /* reset SSP */
        LONG(_start)              /* reset PC  */
        KEEP(*(.text.start))
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > rom
    .data : {
        _data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        _data_end = .;
    } > ram AT > rom
    _data_load = LOADADDR(.data);
    .bss (NOLOAD) : {
        _bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > ram
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
