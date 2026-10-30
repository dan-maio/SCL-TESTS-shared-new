/* embed-arena.S — the loader arena only; fixtures arrive over semihosting. */
    .section SCL_ARENA_SECTION, "aw", %nobits
    .balign 8
    .global _simage_arena_start
_simage_arena_start:
    .space SCL_ARENA_BYTES
    .global _simage_arena_end
_simage_arena_end:
