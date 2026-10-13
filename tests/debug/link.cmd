--rom_model
--stack_size=0x100000
--heap_size=0x100000
MEMORY { RAM : origin = 0xC0000000, length = 0x04000000 }
SECTIONS
{
    .text > RAM  .const > RAM  .data > RAM  .bss > RAM  .far > RAM  .fardata > RAM
    .neardata > RAM  .rodata > RAM  .cinit > RAM  .init_array > RAM  .switch > RAM
    .cio > RAM  .stack > RAM  .sysmem > RAM  .vm6747.eh > RAM
}
