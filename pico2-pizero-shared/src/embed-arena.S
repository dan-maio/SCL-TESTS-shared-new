/* embed-arena.S — the loader arena only; fixtures arrive over semihosting. */
    .section SCL_ARENA_SECTION, "aw", %nobits
    .balign 8
    .global _simage_arena_start
_simage_arena_start:
    .space SCL_ARENA_BYTES
    .global _simage_arena_end
_simage_arena_end:

#if defined(SCL_WINDOW_TEXT)
/* scl::optimal's fixed windows: fixtures are linked at these addresses. */
    .global _sxip_text_start, _sxip_text_end, _sxip_data_start, _sxip_data_end
    .set _sxip_text_start, SCL_WINDOW_TEXT
    .set _sxip_text_end,   SCL_WINDOW_TEXT + 0x8000
    .set _sxip_data_start, SCL_WINDOW_DATA
    .set _sxip_data_end,   SCL_WINDOW_DATA + 0x8000
#endif
