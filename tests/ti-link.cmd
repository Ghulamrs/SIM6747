/* One flat memory for the C6747 and every section in it: enough to ask the
   linker whether a program resolves, which is all this leg asks. */
--rom_model
--stack_size=0x4000
--heap_size=0x100000
MEMORY
{
    RAM : origin = 0xC0000000, length = 0x04000000
}
SECTIONS
{
    .text        > RAM
    .const       > RAM
    .data        > RAM
    .bss         > RAM
    .far         > RAM
    .fardata     > RAM
    .neardata    > RAM
    .rodata      > RAM
    .cinit       > RAM
    .init_array  > RAM
    .switch      > RAM
    .cio         > RAM
    .stack       > RAM
    .sysmem      > RAM
    .vm6747.eh   > RAM
}
