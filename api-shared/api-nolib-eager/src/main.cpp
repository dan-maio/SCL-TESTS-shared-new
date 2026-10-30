/**
 * @file main.cpp — api-nolib-eager: scl::dynamic-nolib with an external table.
 *
 * New APIs: fw::dl_set_symtab(), fw::dl_set_lazy_binding(false), and the lock and
 * cache hooks. The module is copied into the RAM arena and its import is bound at
 * load, so calling it must not enter the loader: the run checks that the lock
 * count does not change across the calls.
 * main also exports a decoy app_gain() returning 99, reachable only through
 * .dynsym, so the result proves the table was used.
 * Output on USART1 (PA9, 115200) + the semihosting console. WeAct F411CE.
 */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <span>
#include "uart.h"
#include "firmware/semihost-file.hpp"
#include "dynamic-nolib/dl.hpp"

#ifndef SCL_SO_DIR
#define SCL_SO_DIR "."
#endif

extern "C" {
__attribute__((used)) float app_gain(void) { return 99.0f; }
float gain_from_table(void) { return 1.5f; }
}

/* r10 = the module GOT base register; set it before calling into the module */
register void* r10_val asm("r10");

static void line(const char* s) { uart_puts(s); puts(s); }
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

class semihost_source final : public loader::byte_source {
public:
    explicit semihost_source(firmware::semihost_file& f) : f_(f) {}
    std::size_t size() const noexcept override { return f_.size(); }
    loader::result<void> read_at(std::size_t off, std::span<std::byte> out) noexcept override {
        return f_.read_at(off, out);
    }
private:
    firmware::semihost_file& f_;
};

static float call_f(fw::handle& h, const char* name, float a, float b) {
    auto fn = fw::dlsym(h, name);
    if (!fn) return 0;
    r10_val = reinterpret_cast<void*>(h.got_base);
    return reinterpret_cast<float (*)(float, float)>(*fn)(a, b);
}

int main(void) {
    uart_init();
    line("api-nolib-eager: dl_set_symtab + dl_set_lazy_binding(false) (scl::dynamic-nolib)");

    static const scl_sym_entry table[] = {
        {"app_gain", (std::uint32_t)(std::uintptr_t)&gain_from_table},
    };
    fw::dl_set_symtab(table, 1);
    fw::dl_set_lazy_binding(false);

    firmware::semihost_file f(SCL_SO_DIR "/floatlib.so");
    if (!f.is_open()) { line("LOAD FAILED"); line("RESULT: FAILURE"); for (;;) {} }
    semihost_source src(f);
    auto h = fw::dlopen_source(src, "floatlib.so");
    if (!h) { line("LOAD FAILED"); line("RESULT: FAILURE"); for (;;) {} }

    /* dlsym itself takes the lock, so resolve first and compare around the calls only. */
    auto add = fw::dlsym(**h, "float_add");
    auto scale = fw::dlsym(**h, "float_scale");
    if (!add || !scale) { line("FAIL: dlsym"); line("RESULT: FAILURE"); for (;;) {} }
    const int locks_before = g_locks;
    r10_val = reinterpret_cast<void*>((*h)->got_base);
    const float sum = reinterpret_cast<float (*)(float, float)>(*add)(2.5f, 3.5f);
    r10_val = reinterpret_cast<void*>((*h)->got_base);
    const float scaled = reinterpret_cast<float (*)(float, float)>(*scale)(2.0f, 3.0f);
    const bool eager = g_locks == locks_before;

    const float prod = call_f(**h, "float_mul", 2.5f, 3.5f);

    char l[96];
    std::snprintf(l, sizeof l, "  add=%u mul=%u scale=%u (x1000) eager=%d",
                  milli(sum), milli(prod), milli(scaled), eager);
    line(l);

    fw::dlclose(**h);
    const bool hooks = check_hooks();
    if (!hooks) line("FAIL: hooks");
    if (!eager) line("FAIL: a call entered the loader");
    const bool ok = near(sum, 6.0f) && near(prod, 8.75f) && near(scaled, 9.0f);
    if (!ok) line("FAIL: values");
    line(ok && hooks && eager ? "RESULT: SUCCESS" : "RESULT: FAILURE");
    for (;;) {}
}
