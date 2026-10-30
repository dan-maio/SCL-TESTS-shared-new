/**
 * @file main.cpp — api-static-boot-table: scl::static with a compile-time table.
 *
 * New APIs: SCL_REGISTER_SYM(), scl::symtab::registered(), source_registered(),
 * scl_publish_release(),
 * symbol_table::add_entries(), and the lock and cache hooks.
 * The module imports app_gain() from main; it resolves through the registered
 * table, with no host tool and no ELF parsing.
 * Output on USART1 + semihosting.
 */
#include "module.h"
#include "elf-reader.h"
#include "scl/symtab-runtime.hpp"
#include "scl/cache.h"
#include "diag/trace.h"
#include "stm32f4xx_hal.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" { void uart_init(void); void uart_puts(const char* s); }

extern "C" {
float float_add(float a, float b);
float float_mul(float a, float b);
float float_scale(float a, float b);
float app_gain(void) { return 1.5f; }
}

SCL_REGISTER_SYM(app_gain);
SCL_REGISTER_SYM(memcpy);
SCL_REGISTER_SYM(memset);

#ifndef DL_LIB_DIR
#define DL_LIB_DIR "."
#endif

static elf_loader::symbol_table g_syms;
alignas(8) static uint8_t g_exec[16 * 1024];

static void line(const char* s) { uart_puts(s); trace_printf("%s\n", s); }
static bool near(float v, float want) { float d = v - want; return d < 0.001f && d > -0.001f; }
static unsigned milli(float v) { return (unsigned)(v * 1000.0f + 0.5f); }

// ---- new hooks: count every call, so the run proves the loader really uses them ----
static volatile int g_lock_depth, g_lock_max, g_locks, g_unlocks, g_cleans, g_invalidates;
extern "C" void scl_loader_lock(void) {
    ++g_locks; if (++g_lock_depth > g_lock_max) g_lock_max = g_lock_depth;
}
extern "C" void scl_loader_unlock(void) { ++g_unlocks; --g_lock_depth; }
extern "C" void scl_cache_clean(const void*, size_t) { ++g_cleans; }
extern "C" void scl_cache_invalidate(const void*, size_t) { ++g_invalidates; }

static bool check_hooks() {
    char l[96];
    std::snprintf(l, sizeof l, "  hooks: locks=%d unlocks=%d max_depth=%d cleans=%d invalidates=%d",
                  g_locks, g_unlocks, g_lock_max, g_cleans, g_invalidates);
    line(l);
    return g_locks > 0 && g_locks == g_unlocks && g_lock_depth == 0 &&
           g_cleans > 0 && g_invalidates > 0;
}

extern "C" int main(void) {
    uart_init();
    HAL_Init();
    line("api-static-boot-table: SCL_REGISTER_SYM + add_entries (scl::static)");

    const scl_sym_entry* table = nullptr;
    const std::size_t n = scl::symtab::registered(&table);
    char l[96];
    std::snprintf(l, sizeof l, "  registered %u symbols", (unsigned)n); line(l);
    if (n != 3) { line("FAIL: registered table"); line("RESULT: FAILURE"); std::exit(1); }
    g_syms.add_entries(table, n);

    auto by_name = scl::symtab::source_registered();
    auto gain_addr = by_name("app_gain");
    if (!gain_addr || *gain_addr != (uint32_t)(uintptr_t)&app_gain || by_name("not_registered")) {
        line("FAIL: source_registered"); line("RESULT: FAILURE"); std::exit(1);
    }

    scl::Module m;
    if (!m.load_file(DL_LIB_DIR "/floatlib.so", g_exec, sizeof g_exec, &g_syms)) {
        line("LOAD FAILED"); line("RESULT: FAILURE"); std::exit(1);
    }

    scl_publish_release();

    const float sum = float_add(2.5f, 3.5f), prod = float_mul(2.5f, 3.5f);
    const float scaled = float_scale(2.0f, 3.0f);   // 2 * 3 * app_gain() = 9
    std::snprintf(l, sizeof l, "  add=%u mul=%u scale=%u (x1000)", milli(sum), milli(prod), milli(scaled));
    line(l);

    const bool hooks = check_hooks();
    if (!hooks) line("FAIL: hooks");
    const bool ok = near(sum, 6.0f) && near(prod, 8.75f) && near(scaled, 9.0f);
    if (!ok) line("FAIL: values");
    line(ok && hooks ? "RESULT: SUCCESS" : "RESULT: FAILURE");
    std::exit(0);
    for (;;) {}
}
