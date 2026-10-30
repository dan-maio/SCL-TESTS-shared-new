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

#if defined(TEST_EH_UNWIND)
/* Enter a module with r10 = its GOT base. Exceptions unwind through this frame. */
    .syntax unified
    .thumb
    .text
    .global call_mod
    .type call_mod, %function
    .thumb_func
call_mod:
    .fnstart
    push {r10, lr}
    .save {r10, lr}
    mov r10, r1
    mov r12, r0
    mov r0, r2
    mov r1, r3
    blx r12
    pop {r10, pc}
    .fnend
    .size call_mod, . - call_mod
#endif
