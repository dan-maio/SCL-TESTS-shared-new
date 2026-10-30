/**
 * @file main.cpp — api-optimal-boot-table: scl::optimal with a boot-time table.
 *
 * New APIs: scl::symtab::build_blob(), view, source(),
 * xip::nopic::set_main_symbols(), and the lock and cache hooks.
 * main also exports a decoy app_gain() returning 99. The table maps the name to
 * gain_from_table() instead, so the result proves the table replaced .dynsym.
 */
#include "optimal/loader.hpp"
#include "scl/symtab-runtime.hpp"
#include "firmware/semihost-file.hpp"
#include "fs-archive.hpp"
#include "scl/image-memory.hpp"
#include "diag/trace.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

extern "C" { void uart_init(void); void uart_puts(const char* s); }

#ifndef DL_LIB_DIR
#define DL_LIB_DIR "."
#endif

extern "C" {
float float_add(float, float);
float float_mul(float, float);
float float_scale(float, float);
float app_gain(void) { return 99.0f; }     // decoy: reachable only through .dynsym
float gain_from_table(void) { return 1.5f; }
}

namespace {

void line(const char* s) { uart_puts(s); trace_printf("%s\n", s); }
bool near(float v, float want) { float d = v - want; return d < 0.001f && d > -0.001f; }
unsigned milli(float v) { return (unsigned)(v * 1000.0f + 0.5f); }

std::span<const std::byte> provide(const char* name) {
    char path[160];
    std::snprintf(path, sizeof(path), "%s/%s", DL_LIB_DIR, name);
    firmware::semihost_file src(path);
    if (!src.is_open()) { trace_printf("absent: %s\n", name); return {}; }
    const std::size_t sz = src.size();
    auto memp = fw::image_arena().alloc(sz);
    if (!memp) return {};
    auto* buf = static_cast<std::byte*>(*memp);
    bool ok = src.read_at(0, std::span<std::byte>(buf, sz)).has_value() &&
              fw::fs_add(name, buf, (std::uint32_t)sz).has_value();
    fw::image_arena().free(buf);
    if (!ok) return {};
    const fw::FsEntry* e = fw::fs_find(name);
    return e ? std::span<const std::byte>(fw::fs_blob(*e), e->size) : std::span<const std::byte>{};
}

}  // namespace

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
    line("api-optimal-boot-table: build_blob + view + set_main_symbols (scl::optimal)");

    const scl_sym_entry entries[] = {
        {"app_gain", (std::uint32_t)(std::uintptr_t)&gain_from_table},
    };
    alignas(4) static std::byte blob[128];
    const std::size_t used = scl::symtab::build_blob(
        entries, 1, blob, sizeof blob, (std::uint32_t)(std::uintptr_t)blob);
    static scl::symtab::view table;
    table = scl::symtab::view(blob, used, (std::uint32_t)(std::uintptr_t)blob);
    if (!table.valid() || table.lookup("app_gain") == 0) {
        line("FAIL: blob"); line("RESULT: FAILURE"); std::exit(1);
    }
    xip::nopic::set_main_symbols(scl::symtab::source(table));

    fw::fs_format();
    optimal::Module mods[1];
    std::span<const std::byte> blobs[1];
    blobs[0] = provide("floatlib.so");
    if (optimal::load_and_link(blobs, 1, mods) != 1) {
        line("LOAD FAILED"); line("RESULT: FAILURE"); std::exit(1);
    }

    const float sum = float_add(2.5f, 3.5f), prod = float_mul(2.5f, 3.5f);
    const float scaled = float_scale(2.0f, 3.0f);   // 9 from the table; 594 from the decoy
    char l[96];
    std::snprintf(l, sizeof l, "  add=%u mul=%u scale=%u (x1000)", milli(sum), milli(prod), milli(scaled));
    line(l);

    bool hooks = check_hooks();
    if (g_lock_max < 2) hooks = false;     // load_and_link -> nopic::load nests the lock
    if (!hooks) line("FAIL: hooks");
    const bool ok = near(sum, 6.0f) && near(prod, 8.75f) && near(scaled, 9.0f);
    if (!ok) line("FAIL: values");
    line(ok && hooks ? "RESULT: SUCCESS" : "RESULT: FAILURE");
    std::exit(0);
    for (;;) {}
}
