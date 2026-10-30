/* embed.S — the module image, in flash, at a stable aligned address. */
    .section .rodata.smpmod, "a"
    .balign 8
    .global smpmod_image_start
smpmod_image_start:
    .incbin SMPMOD_PATH
    .global smpmod_image_end
smpmod_image_end:

/* The loader arena. Its section decides where the module's RAM part lives. */
    .section SCL_ARENA_SECTION, "aw", %nobits
    .balign 8
    .global _simage_arena_start
_simage_arena_start:
    .space SCL_ARENA_BYTES
    .global _simage_arena_end
_simage_arena_end:
