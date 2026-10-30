/**
 * @file fixtures-main.cpp
 * @brief The WeAct fixtures (cpp3, c2, c1, mylib1) on the dual-core RP2350.
 *
 * Same rule as main.cpp: load and patch on core 0, at boot, then start SMP. As on the
 * WeAct board the fixtures are read from the host over semihosting. For XIP they are
 * patched and written to flash first, then loaded from there.
 *
 * c1, c2 and cpp3 import from each other. Under scl::reloc those calls are absolute.
 * Under the r10 models they go through a cross-call trampoline, whose lookup is loader
 * code running at call time on both cores, so TEST_CROSS_CALL_LOOKUP expects it.
 */
#include <cmsis-plus/rtos/os.h>

#include <bsp/led.hpp>
#include <bsp/multicore.hpp>
#include <bsp/uart.hpp>
#if defined(TEST_PSRAM)
#include <bsp/psram.hpp>
#endif

#include <hw_result.hpp>

#include "scl/cache.h"
#include "scl/os-hooks.h"
#include "scl/symtab-runtime.hpp"

#if defined(TEST_LOADER_RELOC)
#include "reloc/reloc-loader.hpp"
#elif defined(TEST_LOADER_NOLIB)
#include "dynamic-nolib/dl.hpp"
#include "loader/sources/memory-byte-source.hpp"
#elif defined(TEST_LOADER_XIP)
#include "xip/dynamic-newlib/adapter.hpp"
#endif

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>

using namespace os::rtos;
using u32 = std::uint32_t;

extern "C" {
void port_smp_secondary_start(void);
unsigned port_core1_stack_top(void);
unsigned port_cpu_id(void);
void port_smp_core0_ipi_enable(void);
extern std::uint8_t _simage_arena_start[], _simage_arena_end[];
}

namespace os::rtos::scheduler {
extern thread* os_idle_thread_core[OS_NCPU];
}

// ---- output: UART, and semihosting for the runner -----------------------------
namespace {
void say(const char* s) {
    uart::write(s);
    if (!uos_debug_host_attached()) return;
    register unsigned r0 __asm__("r0") = 0x04u;  // SYS_WRITE0
    register const char* r1 __asm__("r1") = s;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
}
void sayf(const char* fmt, unsigned a = 0, unsigned b = 0, unsigned c = 0, unsigned d = 0) {
    char l[120];
    std::snprintf(l, sizeof l, fmt, a, b, c, d);
    say(l);
}

unsigned semi(unsigned op, const void* arg) {
    register unsigned r0 __asm__("r0") = op;
    register const void* r1 __asm__("r1") = arg;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}

/* Read a host file into buf; returns its size, or 0. */
std::size_t read_host_file(const char* path, std::byte* buf, std::size_t cap) {
    u32 open_args[3] = {reinterpret_cast<u32>(path), 1u, static_cast<u32>(std::strlen(path))};
    const int h = static_cast<int>(semi(0x01u, open_args));
    if (h < 0) return 0;
    u32 flen_arg = static_cast<u32>(h);
    const std::size_t n = semi(0x0Cu, &flen_arg);
    std::size_t got = 0;
    if (n > 0 && n <= cap) {
        u32 read_args[3] = {static_cast<u32>(h), reinterpret_cast<u32>(buf), static_cast<u32>(n)};
        if (semi(0x06u, read_args) == 0) got = n;  // SYS_READ returns bytes NOT read
    }
    semi(0x02u, &flen_arg);
    return got;
}

// ---- SIO spinlocks: 0 is the kernel's ------------------------------------------
constexpr u32 kAppLock = 0xD0000100u + 30u * 4u, kLoaderLock = 0xD0000100u + 29u * 4u;
inline u32 irq_off() {
    u32 pm;
    __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(pm) : : "memory");
    return pm;
}
inline void irq_restore(u32 pm) { __asm__ volatile("msr primask, %0" : : "r"(pm) : "memory"); }
inline void spin_take(u32 reg) {
    while (*reinterpret_cast<volatile u32*>(reg) == 0) {
    }
    __asm__ volatile("dmb" : : : "memory");
}
inline void spin_give(u32 reg) {
    __asm__ volatile("dmb" : : : "memory");
    *reinterpret_cast<volatile u32*>(reg) = 1;
}
u32 g_app_pm[2];
}  // namespace

// ---- what the fixtures import from main ----------------------------------------
extern "C" {
void app_lock(void) {
    const u32 pm = irq_off();
    spin_take(kAppLock);
    g_app_pm[port_cpu_id() & 1u] = pm;
}
void app_unlock(void) {
    const u32 pm = g_app_pm[port_cpu_id() & 1u];
    spin_give(kAppLock);
    irq_restore(pm);
}

/* The fixtures trace on every call; on two cores that would only measure the UART. */
void trace_event(const char*, const char*, u32) {}
void trace_u32(const char*, u32) {}
u32 fm_callback(u32 x) { return x + 1000; }
u32 fm_var = 123;
struct FmInfo { u32 id, flags; };
FmInfo fm_info = {0x00F1, 0x0007};
unsigned AA = 1000, BB = 2000;
char* s1 = const_cast<char*>("main-s1");
unsigned f(unsigned x) { return x + 10; }
unsigned w(unsigned x) { return x + 20; }
int fixture_printf(const char* fmt, ...) { say("  [fixture] "); say(fmt); return 0; }
int fixture_puts(const char* s) { say("  [fixture] "); say(s); say("\n"); return 0; }

void* locked_malloc(unsigned n) { app_lock(); void* p = std::malloc(n); app_unlock(); return p; }
void locked_free(void* p) { app_lock(); std::free(p); app_unlock(); }
void* locked_new(unsigned n) { return locked_malloc(n); }
void locked_delete(void* p) { locked_free(p); }
void locked_delete2(void* p, unsigned) { locked_free(p); }
double fixture_log(double x) { return static_cast<double>(logf(static_cast<float>(x))); }
float __aeabi_d2f(double);
double __aeabi_f2d(float);

#if defined(TEST_LOADER_XIP)
/* dynamic-newlib reads main's relocation table from here. This firmware has none. */
extern const u32 _reloc[10];
const u32 _reloc[10] = {};
#endif

// ---- loader lock: a real recursive lock, and counters ---------------------------
volatile unsigned g_published;
volatile unsigned g_locks, g_lock_max, g_locks_after_publish;
volatile unsigned g_cleans, g_invalidates;
namespace {
volatile int g_lock_owner = -1;
unsigned g_lock_depth;
u32 g_lock_pm;
}  // namespace
void scl_loader_lock(void) {
    const u32 pm = irq_off();
    const int me = static_cast<int>(port_cpu_id());
    if (g_lock_owner != me) {
        spin_take(kLoaderLock);
        g_lock_owner = me;
        g_lock_pm = pm;
    }
    ++g_lock_depth;
    ++g_locks;
    if (g_published) ++g_locks_after_publish;
    if (g_lock_depth > g_lock_max) g_lock_max = g_lock_depth;
}
void scl_loader_unlock(void) {
    if (--g_lock_depth == 0) {
        const u32 pm = g_lock_pm;
        g_lock_owner = -1;
        spin_give(kLoaderLock);
        irq_restore(pm);
    }
}

namespace {
constexpr u32 kXipBase = 0x10000000u, kXipEnd = 0x12000000u, kXipMaint = 0x18000000u;
void xip_maint(const void* addr, std::size_t len, unsigned op) {
    const u32 a = reinterpret_cast<u32>(addr);
    if (a >= kXipBase && a < kXipEnd) {
        const u32 o0 = (a - kXipBase) & ~7u, o1 = ((a - kXipBase) + len + 7u) & ~7u;
        for (u32 o = o0; o < o1; o += 8u)
            *reinterpret_cast<volatile std::uint8_t*>(kXipMaint + o + op) = 0;
    }
    __asm__ volatile("dsb sy\n isb" : : : "memory");
}
}  // namespace
void scl_cache_clean(const void* addr, size_t len) { ++g_cleans; xip_maint(addr, len, 3u); }
void scl_cache_invalidate(const void* addr, size_t len) { ++g_invalidates; xip_maint(addr, len, 2u); }
void scl_log_puts(const char* s) { uart::write(s); }
void scl_log_putc(char c) { uart::write(&c, 1); }
}  // extern "C"

namespace {

#define SYM(n, p) {n, reinterpret_cast<u32>(p)}
const scl_sym_entry kMainSyms[] = {
    SYM("trace_event", &trace_event), SYM("trace_u32", &trace_u32),
    SYM("fm_callback", &fm_callback), SYM("fm_var", &fm_var), SYM("fm_info", &fm_info),
    SYM("sinf", static_cast<float (*)(float)>(&sinf)), SYM("cosf", static_cast<float (*)(float)>(&cosf)),
    SYM("logf", static_cast<float (*)(float)>(&logf)), SYM("sqrtf", static_cast<float (*)(float)>(&sqrtf)),
    SYM("malloc", &locked_malloc), SYM("free", &locked_free), SYM("_Znwj", &locked_new),
    SYM("_ZdlPv", &locked_delete), SYM("_ZdlPvj", &locked_delete2),
    SYM("AA", &AA), SYM("BB", &BB), SYM("s1", &s1), SYM("f", &f), SYM("w", &w),
    SYM("printf", &fixture_printf), SYM("puts", &fixture_puts), SYM("strcat", &strcat), SYM("strlen", &strlen),
    SYM("log", &fixture_log), SYM("__aeabi_d2f", &__aeabi_d2f), SYM("__aeabi_f2d", &__aeabi_f2d),
};
constexpr std::size_t kNumSyms = sizeof kMainSyms / sizeof kMainSyms[0];

// ---- the fixtures, filled at boot and read-only afterwards ----------------------
using fn2 = u32 (*)(u32, u32);
struct Fn { fn2 f; u32 got; };
struct Fixtures {
    Fn cpp3_method, cpp3_recurse, cpp3_float_cross, cpp3_heap, c2_add, c2_param, c2_state,
        c1_call_c2, c1_use_fm, c1_state, library_function, retstr;
} g_fx;

__attribute__((naked)) u32 call_mod(fn2, u32 /*got*/, u32 /*a*/, u32 /*b*/) {
    __asm__ volatile(
        "push {r10, lr}\n mov r10, r1\n mov r12, r0\n"
        "mov r0, r2\n mov r1, r3\n blx r12\n pop {r10, pc}\n");
}
inline u32 call(const Fn& f, u32 a = 0, u32 b = 0) { return call_mod(f.f, f.got, a, b); }

const char* g_boot_error = "boot_load did not run";
unsigned g_boot_cpu = 99;
u32 g_text_addr, g_data_got;

constexpr std::size_t kStageEach = 32 * 1024;
alignas(8) std::byte g_stage[4][kStageEach];
std::size_t g_size[4];
const char* const kNames[4] = {"cpp3", "c2", "c1", "mylib1"};

bool read_fixtures() {
    for (int i = 0; i < 4; ++i) {
        char path[200];
        std::snprintf(path, sizeof path, "%s/%s%s", FIXTURE_DIR, kNames[i], FIXTURE_EXT);
        g_size[i] = read_host_file(path, g_stage[i], kStageEach);
        say("  [host] "); say(kNames[i]); sayf(": %u bytes\n", g_size[i]);
        if (g_size[i] == 0) { g_boot_error = "fixture not readable over semihosting"; return false; }
    }
    return true;
}

#if defined(TEST_LOADER_XIP)
// ---- flash provisioning through the bootrom, as the WeAct flat-fs does ----------
constexpr u32 kFlashOffset = 8u * 1024u * 1024u, kFlashSector = 4096u;
using rom_void = void (*)(void);
using rom_erase = void (*)(u32, std::size_t, u32, std::uint8_t);
using rom_prog = void (*)(u32, const std::uint8_t*, std::size_t);
struct Rom { rom_void connect, exit_xip, flush, enter_xip; rom_erase erase; rom_prog prog; } g_rom;

void* rom_lookup(char a, char b) {
    using lookup_t = void* (*)(u32, u32);
    const auto fn = reinterpret_cast<lookup_t>(
        static_cast<std::uintptr_t>(*reinterpret_cast<const std::uint16_t*>(0x16u)));
    return fn(static_cast<u32>(a) | (static_cast<u32>(b) << 8), 0x0004u);  // ARM secure
}

/* Runs from SRAM: flash is not readable between exit_xip and enter_xip. */
__attribute__((section(".data.flash_write"), noinline, long_call)) void
flash_write_ram(u32 offset, const std::uint8_t* data, std::size_t n) {
    g_rom.connect();
    g_rom.exit_xip();
    g_rom.erase(offset, (n + kFlashSector - 1u) & ~(kFlashSector - 1u), kFlashSector, 0x20u);
    g_rom.prog(offset, data, (n + 255u) & ~255u);
    g_rom.flush();
    g_rom.enter_xip();
}

std::span<const std::byte> g_flash_image[4];
bool provision_flash() {
    g_rom = Rom{reinterpret_cast<rom_void>(rom_lookup('I', 'F')),
                reinterpret_cast<rom_void>(rom_lookup('E', 'X')),
                reinterpret_cast<rom_void>(rom_lookup('F', 'C')),
                reinterpret_cast<rom_void>(rom_lookup('C', 'X')),
                reinterpret_cast<rom_erase>(rom_lookup('R', 'E')),
                reinterpret_cast<rom_prog>(rom_lookup('R', 'P'))};
    if (!g_rom.connect || !g_rom.exit_xip || !g_rom.flush || !g_rom.enter_xip || !g_rom.erase || !g_rom.prog) {
        g_boot_error = "bootrom flash functions not found";
        return false;
    }
    u32 off = kFlashOffset;
    for (int i = 0; i < 4; ++i) {
        std::span<std::byte> img{g_stage[i], g_size[i]};
        xip::newlib::patch_main_imports(img);
        const u32 pm = irq_off();
        flash_write_ram(off, reinterpret_cast<const std::uint8_t*>(img.data()), img.size());
        irq_restore(pm);
        const auto* in_flash = reinterpret_cast<const std::byte*>(kXipBase + off);
        if (std::memcmp(in_flash, img.data(), img.size()) != 0) {
            g_boot_error = "flash verify failed";
            return false;
        }
        g_flash_image[i] = {in_flash, img.size()};
        off += (static_cast<u32>(img.size()) + kFlashSector - 1u) & ~(kFlashSector - 1u);
    }
    return true;
}
#endif

bool load_fixtures() {
    auto set = [](Fn& out, fn2 p, u32 got, const char* name) {
        out = Fn{p, got};
        if (!p) { g_boot_error = name; return false; }
        return true;
    };
    bool ok = true;
#if defined(TEST_LOADER_RELOC)
    alignas(4) static std::byte blob[2048];
    static scl::symtab::view table;
    const u32 at = reinterpret_cast<u32>(blob);
    table = scl::symtab::view(blob, scl::symtab::build_blob(kMainSyms, kNumSyms, blob, sizeof blob, at), at);
    if (!table.valid()) { g_boot_error = "symbol blob"; return false; }
    const std::size_t total = static_cast<std::size_t>(_simage_arena_end - _simage_arena_start);
    auto* arena = reinterpret_cast<std::byte*>(_simage_arena_start);
    static std::span<const std::byte> imgs[3];
    for (int i = 0; i < 3; ++i) imgs[i] = {g_stage[i], g_size[i]};
    static reloc::module_set cluster;
    auto rs = reloc::load_set(std::span<const std::span<const std::byte>>(imgs, 3),
                              {arena, total * 3 / 4}, scl::symtab::source(table));
    if (!rs) { g_boot_error = "reloc::load_set"; return false; }
    cluster = *rs;
    static reloc::module m1;
    auto r1 = reloc::load({g_stage[3], g_size[3]}, {arena + total * 3 / 4, total / 4}, scl::symtab::source(table));
    if (!r1) { g_boot_error = "reloc::load mylib1"; return false; }
    m1 = *r1;
#define GET(field, h) ok = ok && set(g_fx.field, reinterpret_cast<fn2>(reloc::sym(h, #field)), 0, "missing " #field)
    GET(cpp3_method, cluster); GET(cpp3_recurse, cluster); GET(cpp3_float_cross, cluster);
    GET(cpp3_heap, cluster); GET(c2_add, cluster); GET(c2_param, cluster); GET(c2_state, cluster);
    GET(c1_call_c2, cluster); GET(c1_use_fm, cluster); GET(c1_state, cluster);
    GET(library_function, m1); GET(retstr, m1);
#elif defined(TEST_LOADER_NOLIB)
    fw::dl_set_symtab(kMainSyms, kNumSyms);
    fw::dl_set_lazy_binding(false);
    fw::dl_enable_cross_call(true);
    fw::dl_cross_trace(0);
    static fw::handle* h[4];
    for (int i = 0; i < 4; ++i) {
        loader::memory_byte_source src{std::span<const std::byte>{g_stage[i], g_size[i]}};
        auto r = fw::dlopen_source(src, kNames[i]);
        if (!r) { g_boot_error = "dlopen_source"; return false; }
        h[i] = *r;
    }
    auto sym = [](fw::handle* m, const char* n) {
        auto v = fw::dlsym(*m, n);
        return v ? reinterpret_cast<fn2>(*v) : nullptr;
    };
#define GET(field, i) ok = ok && set(g_fx.field, sym(h[i], #field), h[i]->got_base, "missing " #field)
    GET(cpp3_method, 0); GET(cpp3_recurse, 0); GET(cpp3_float_cross, 0); GET(cpp3_heap, 0);
    GET(c2_add, 1); GET(c2_param, 1); GET(c2_state, 1);
    GET(c1_call_c2, 2); GET(c1_use_fm, 2); GET(c1_state, 2);
    GET(library_function, 3); GET(retstr, 3);
#elif defined(TEST_LOADER_XIP)
    xip::newlib::set_symtab(kMainSyms, kNumSyms);
    if (!provision_flash()) return false;
#if defined(TEST_PSRAM)
    if (!psram::init()) { g_boot_error = "psram::init"; return false; }
#endif
    static void* h[4];
    for (int i = 0; i < 4; ++i) {
        h[i] = xip::newlib::dlopen(g_flash_image[i], kNames[i]);
        if (!h[i]) { g_boot_error = "xip dlopen"; return false; }
        if (!xip::newlib::is_xip(h[i])) { g_boot_error = "fixture was not loaded XIP"; return false; }
    }
#define GET(field, i) ok = ok && set(g_fx.field, reinterpret_cast<fn2>(xip::newlib::dlsym(h[i], #field)), \
                                     xip::newlib::get_got_base(h[i]), "missing " #field)
    GET(cpp3_method, 0); GET(cpp3_recurse, 0); GET(cpp3_float_cross, 0); GET(cpp3_heap, 0);
    GET(c2_add, 1); GET(c2_param, 1); GET(c2_state, 1);
    GET(c1_call_c2, 2); GET(c1_use_fm, 2); GET(c1_state, 2);
    GET(library_function, 3); GET(retstr, 3);
#endif
    return ok;
}

constexpr unsigned kGold = 16;
u32 g_gold[kGold], g_hits_base, g_c1_base;

/* Boot time: core 1 is still in the bootrom and the kernel's main() has not run. */
void boot_load() {
    g_boot_cpu = port_cpu_id();
    if (!read_fixtures()) return;
#if defined(TEST_PSRAM) && !defined(TEST_LOADER_XIP)
    if (!psram::init()) { g_boot_error = "psram::init"; return; }
#endif
    if (!load_fixtures()) return;

    g_text_addr = reinterpret_cast<u32>(g_fx.cpp3_method.f);
    g_data_got = g_fx.cpp3_method.got;
    if ((g_text_addr >> 24) != TEST_EXPECT_TEXT_TOP) { g_boot_error = "fixture text in the wrong memory"; return; }
#if defined(TEST_EXPECT_DATA_TOP)
    if ((g_data_got >> 24) != TEST_EXPECT_DATA_TOP) { g_boot_error = "fixture data in the wrong memory"; return; }
#endif
    for (unsigned i = 0; i < kGold; ++i) g_gold[i] = call(g_fx.cpp3_method, 7, i * 5u + 1u);
    if (std::strcmp(reinterpret_cast<const char*>(call(g_fx.retstr)), "Hello mylib1!") != 0 &&
        std::strcmp(reinterpret_cast<const char*>(call(g_fx.retstr)), "Hello from mylib1!") != 0) {
        g_boot_error = "mylib1 retstr";
        return;
    }
    g_hits_base = call(g_fx.c2_state);
    g_c1_base = call(g_fx.c1_state);
    g_boot_error = nullptr;
    scl_publish_release();
    g_published = 1;
}
}  // namespace

/* Linked with --wrap=main: every static constructor has run, including SCL's own, and
 * the kernel's main() has not. A load from an earlier constructor would be wiped when
 * SCL's globals are constructed afterwards. */
extern "C" int __real_main(int, char**);
extern "C" int __wrap_main(int argc, char** argv) {
    boot_load();
    return __real_main(argc, argv);
}
namespace {

constexpr unsigned kWorkers = 4, kIters = TEST_ITERS;
constexpr u32 kHitsPerIter = 5;  // c2_add 1, float_cross 1, recurse(4) 2, c1_call_c2 1
struct Worker {
    unsigned id;
    volatile unsigned errors, done, cores_seen, unpublished;
    volatile unsigned first_bad;
} g_workers[kWorkers];
thread::stack::element_t g_stacks[kWorkers][2048];

void* worker(void* arg) {
    auto* w = static_cast<Worker*>(arg);
    if (w->id < 2) {
        this_thread::thread().cpu_affinity(1u << w->id);
        if (port_cpu_id() != w->id) this_thread::yield();
    }
    if (!g_published) w->unpublished = 1;
    __asm__ volatile("dmb" : : : "memory");

    auto expect = [&](bool ok, unsigned code) {
        if (!ok) { ++w->errors; if (!w->first_bad) w->first_bad = code; }
    };
    for (unsigned i = 0; i < kIters; ++i) {
        const unsigned k = i % kGold;
        // Concurrent on both cores: results depend only on read-only state.
        expect(call(g_fx.cpp3_method, 7, k * 5u + 1u) == g_gold[k], 1);
        expect(call(g_fx.c2_param) == 0x24C0u, 2);
        expect(call(g_fx.c1_use_fm) == 123u + 0xF1u + 7u + 0xC1u, 3);
        expect(call(g_fx.library_function, 39) == 62u, 4);
        expect(call(g_fx.cpp3_heap, 8) == 533u, 5);

        // These bump module counters that have no lock of their own, so hold main's.
        app_lock();
        const u32 h = call(g_fx.c2_state);
        const u32 sum = call(g_fx.c2_add, i, 3);
        const u32 cross = call(g_fx.cpp3_float_cross, 10);
        const u32 deep = call(g_fx.cpp3_recurse, 4);
        call(g_fx.c1_call_c2, 7);
        app_unlock();
        expect(sum == i + 3u + h + 1u + 0x2222u, 6);
        expect(cross == 10u + 0x2200u, 7);
        expect(deep == 10u, 8);

        w->cores_seen = w->cores_seen | (1u << (port_cpu_id() & 1u));
        if ((i & 0x7Fu) == 0) this_thread::yield();
    }
    w->done = 1;
    return nullptr;
}

void* idle1(void*) {
    this_thread::thread().priority(thread::priority::idle);
    for (;;) {
        __asm__ volatile("dsb\n wfi" : : : "memory");
        this_thread::yield();
    }
    return nullptr;
}

}  // namespace

int os_main(int, char*[]) {
    this_thread::thread().cpu_affinity(1u << 0);
    led::init();
    say("\n" TEST_NAME ": WeAct fixtures loaded at boot, then SMP\n");

    bool ok = true;
    auto check = [&](bool c, const char* what) {
        say(c ? "  ok    " : "  FAIL: ");
        say(what);
        say("\n");
        if (!c) ok = false;
    };
    if (g_boot_error) { say("  boot_load: "); say(g_boot_error); say("\n"); }
    check(g_boot_error == nullptr, "fixtures loaded at boot");
    check(g_boot_cpu == 0, "load ran on core 0");
    if (!ok) { say("RESULT: FAIL\n"); hw_result::fail(); }
    sayf("  cpp3 text 0x%08x, r10 0x%08x; boot locks=%u depth=%u\n", g_text_addr, g_data_got, g_locks, g_lock_max);
    const unsigned boot_locks = g_locks;

    static thread::stack::element_t idle1_stack[512];
    {
        thread::attributes attr = thread::initializer;
        attr.th_stack_address = idle1_stack;
        attr.th_stack_size_bytes = sizeof idle1_stack;
        scheduler::os_idle_thread_core[1] = new thread("idle1", idle1, nullptr, attr);
    }
    static thread* threads[kWorkers];
    for (unsigned i = 0; i < kWorkers; ++i) {
        g_workers[i].id = i;
        thread::attributes attr = thread::initializer;
        attr.th_stack_address = g_stacks[i];
        attr.th_stack_size_bytes = sizeof g_stacks[i];
        char name[8];
        std::snprintf(name, sizeof name, "wrk%u", i);
        threads[i] = new thread(name, worker, &g_workers[i], attr);
        if (i < 2) threads[i]->cpu_affinity(1u << i);
    }

    say("  core 0: launching core 1; SMP starts now\n");
    multicore::launch_core1(port_smp_secondary_start, port_core1_stack_top());
    port_smp_core0_ipi_enable();

    unsigned done = 0;
    for (unsigned t = 0; t < 1600 && done < kWorkers; ++t) {
        sysclock.sleep_for(100);
        done = 0;
        for (auto& w : g_workers) done += w.done;
    }

    unsigned errors = 0, unpublished = 0, first_bad = 0;
    for (auto& w : g_workers) {
        errors += w.errors;
        unpublished += w.unpublished;
        if (!first_bad) first_bad = w.first_bad;
    }
    const u32 total = kWorkers * kIters;
    const u32 hits = call(g_fx.c2_state) - g_hits_base, c1 = call(g_fx.c1_state) - g_c1_base;
    sayf("  workers done=%u errors=%u first failing check=%u\n", done, errors, first_bad);
    sayf("  c2 hits=%u of %u; c1 calls=%u of %u\n", hits, kHitsPerIter * total, c1, total);
    sayf("  free threads saw cores 0x%x 0x%x\n", g_workers[2].cores_seen, g_workers[3].cores_seen);
    sayf("  loader locks=%u (after publish=%u) max depth=%u\n", g_locks, g_locks_after_publish, g_lock_max);
    sayf("  cleans=%u invalidates=%u\n", g_cleans, g_invalidates);

    check(done == kWorkers, "all workers finished");
    check(unpublished == 0, "every worker saw the published fixtures");
    check(errors == 0, "fixtures returned correct values on both cores");
    check(hits == kHitsPerIter * total, "c2 counted every locked call");
    check(c1 == total, "c1 counted every locked call");
    check(g_workers[0].cores_seen == 1u && g_workers[1].cores_seen == 2u, "pinned workers stayed on their core");
    check(boot_locks > 0, "the load took the loader lock");
    check(g_cleans > 0 && g_invalidates > 0, "the loader made its cache calls");
#if defined(TEST_CROSS_CALL_LOOKUP)
    check(g_locks_after_publish > 0, "cross-module calls used the call-time lookup");
#else
    check(g_locks_after_publish == 0, "no loader code ran after publication");
#endif
    say(ok ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    if (ok) hw_result::ok();
    hw_result::fail();
    return 0;
}
