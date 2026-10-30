/**
 * @file fixtures-main.cpp
 * @brief The WeAct fixtures on the dual-core RP2350.
 *
 * TEST_GROUP 1 loads cpp3, c2, c1, mylib1. TEST_GROUP 2 loads libprov, libcons, mylib2.
 * TEST_GROUP 3 adds mylib1 to group 2, for scl::static, which has no cyclic imports.
 * TEST_GROUP 4 loads libcppeh, ehx_catch, libcppsh: C++ exceptions, RTTI and virtuals.
 * TEST_GROUP 5 loads mylib1 and slib, and calls them from a timer interrupt on each core
 * while threads run inside the same modules.
 * TEST_EH_THROW really throws; other loaders register no unwind tables, so they make
 * only the non-throwing calls, as the WeAct dynamic-newlib test does.
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
#if defined(TEST_XIP_STATIC)
#include "xip/static/adapter.hpp"
namespace xipl = xip::stat;
#elif defined(TEST_XIP_NOLIB)
#include "xip/dynamic-nolib/adapter.hpp"
namespace xipl = xip::nolib;
#else
#include "xip/dynamic-newlib/adapter.hpp"
namespace xipl = xip::newlib;
#endif
#elif defined(TEST_LOADER_NEWLIB)
#include "dynamic-newlib/dynamic-linker.hpp"
#include "dynamic-newlib/link-context.hpp"
#include "loader/sources/memory-byte-source.hpp"
#elif defined(TEST_LOADER_OPTIMAL)
#include "optimal/loader.hpp"
#elif defined(TEST_LOADER_STATIC)
#include "elf-reader.h"
#include "module.h"
#endif

#if TEST_GROUP == 4
#include "cross-error.hpp"
#include <cxxabi.h>
#include <exception>
#include <new>
#include <typeinfo>
#endif

#include <cmath>
#include <cstdarg>
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
unsigned g_app_depth[2];
}  // namespace

// ---- what the fixtures import from main ----------------------------------------
extern "C" {
/* Recursive: a fixture called under the lock may allocate, and malloc takes it too. */
void app_lock(void) {
    const u32 pm = irq_off();
    const unsigned me = port_cpu_id() & 1u;
    if (g_app_depth[me]++ == 0) {
        spin_take(kAppLock);
        g_app_pm[me] = pm;
    }
}
void app_unlock(void) {
    const unsigned me = port_cpu_id() & 1u;
    if (--g_app_depth[me] == 0) {
        const u32 pm = g_app_pm[me];
        spin_give(kAppLock);
        irq_restore(pm);
    }
}

/* The fixtures trace on every call. Printed during the load and the walkthroughs, and
 * silent during the stress loop, where it would only measure the console. */
volatile unsigned g_trace = 1;
void trace_event(const char* origin, const char* fn, u32 arg) {
    if (!g_trace) return;
    char l[96];
    std::snprintf(l, sizeof l, "  [trace] %s: %s(%u)\n", origin, fn, static_cast<unsigned>(arg));
    say(l);
}
void trace_u32(const char* label, u32 v) {
    if (!g_trace) return;
    char l[96];
    std::snprintf(l, sizeof l, "  [trace] %s%u\n", label, static_cast<unsigned>(v));
    say(l);
}
u32 fm_callback(u32 x) { return x + 1000; }
u32 fm_var = 123;
struct FmInfo { u32 id, flags; };
FmInfo fm_info = {0x00F1, 0x0007};
unsigned AA = 1000, BB = 2000;
unsigned HOST_K = 1000;
unsigned host_f(unsigned x) { return x * 2 + 1; }
char* s1 = const_cast<char*>("main-s1");
unsigned f(unsigned x) { return x + 10; }
unsigned w(unsigned x) { return x + 20; }
int fixture_printf(const char* fmt, ...) {
    if (!g_trace) return 0;
    char l[120];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(l, sizeof l, fmt, ap);
    va_end(ap);
    say("  [printf] ");
    say(l);
    return n;
}
int fixture_puts(const char* s) {
    if (!g_trace) return 0;
    say("  [printf] ");
    say(s);
    say("\n");
    return 0;
}

void* locked_malloc(unsigned n) { app_lock(); void* p = std::malloc(n); app_unlock(); return p; }
void locked_free(void* p) { app_lock(); std::free(p); app_unlock(); }
void* locked_new(unsigned n) { return locked_malloc(n); }
void locked_delete(void* p) { locked_free(p); }
void locked_delete2(void* p, unsigned) { locked_free(p); }
double fixture_log(double x) { return static_cast<double>(logf(static_cast<float>(x))); }
float __aeabi_d2f(double);
double __aeabi_f2d(float);

#if defined(TEST_LOADER_XIP) || defined(TEST_LOADER_NEWLIB) || defined(TEST_LOADER_STATIC)
/* These loaders read main's relocation table from here. This firmware has none. */
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

#if TEST_GROUP == 4
extern "C" {
void* __cxa_allocate_exception(unsigned);
void __cxa_throw(void*, void*, void (*)(void*));
void* __cxa_begin_catch(void*);
void __cxa_end_catch();
void __cxa_end_cleanup();
int __gxx_personality_v0();
void __aeabi_unwind_cpp_pr0();
void __aeabi_unwind_cpp_pr1();
void* __dynamic_cast(const void*, const void*, const void*, int);
void __cxa_pure_virtual();
extern char _ZTVN10__cxxabiv117__class_type_infoE[], _ZTVN10__cxxabiv120__si_class_type_infoE[];
void* locked_new_nothrow(unsigned n, const std::nothrow_t&) { return locked_malloc(n); }
void locked_delete_nothrow(void* p, const std::nothrow_t&) { locked_free(p); }
}
#endif

namespace {

#define SYM(n, p) {n, reinterpret_cast<u32>(p)}
const scl_sym_entry kMainSyms[] = {
    SYM("trace_event", &trace_event), SYM("trace_u32", &trace_u32),
    SYM("fm_callback", &fm_callback), SYM("fm_var", &fm_var), SYM("fm_info", &fm_info),
    SYM("sinf", static_cast<float (*)(float)>(&sinf)), SYM("cosf", static_cast<float (*)(float)>(&cosf)),
    SYM("logf", static_cast<float (*)(float)>(&logf)), SYM("sqrtf", static_cast<float (*)(float)>(&sqrtf)),
    SYM("malloc", &locked_malloc), SYM("free", &locked_free), SYM("_Znwj", &locked_new),
    SYM("_ZdlPv", &locked_delete), SYM("_ZdlPvj", &locked_delete2),
    SYM("HOST_K", &HOST_K),
#if !defined(TEST_HIDE_HOST_F)
    SYM("host_f", &host_f),
#endif
    SYM("AA", &AA), SYM("BB", &BB), SYM("s1", &s1), SYM("f", &f), SYM("w", &w),
    SYM("printf", &fixture_printf), SYM("puts", &fixture_puts), SYM("strcat", &strcat), SYM("strlen", &strlen),
    SYM("log", &fixture_log), SYM("__aeabi_d2f", &__aeabi_d2f), SYM("__aeabi_f2d", &__aeabi_f2d),
#if TEST_GROUP == 4
    SYM("__cxa_allocate_exception", &__cxa_allocate_exception), SYM("__cxa_throw", &__cxa_throw),
    SYM("__cxa_begin_catch", &__cxa_begin_catch), SYM("__cxa_end_catch", &__cxa_end_catch),
    SYM("__cxa_end_cleanup", &__cxa_end_cleanup), SYM("__gxx_personality_v0", &__gxx_personality_v0),
    SYM("__aeabi_unwind_cpp_pr0", &__aeabi_unwind_cpp_pr0), SYM("__aeabi_unwind_cpp_pr1", &__aeabi_unwind_cpp_pr1),
    SYM("__dynamic_cast", &__dynamic_cast), SYM("__cxa_pure_virtual", &__cxa_pure_virtual),
    SYM("_ZTVN10__cxxabiv117__class_type_infoE", &_ZTVN10__cxxabiv117__class_type_infoE),
    SYM("_ZTVN10__cxxabiv120__si_class_type_infoE", &_ZTVN10__cxxabiv120__si_class_type_infoE),
    SYM("_ZSt7nothrow", &std::nothrow), SYM("_ZnwjRKSt9nothrow_t", &locked_new_nothrow),
    SYM("_ZdlPvRKSt9nothrow_t", &locked_delete_nothrow),
#endif
};
constexpr std::size_t kNumSyms = sizeof kMainSyms / sizeof kMainSyms[0];

// ---- the fixtures, filled at boot and read-only afterwards ----------------------
using fn2 = u32 (*)(u32, u32);
struct Fn { fn2 f; u32 got; };
struct Fixtures {
#if TEST_GROUP == 1
    Fn cpp3_method, cpp3_recurse, cpp3_float_cross, cpp3_heap, c2_add, c2_param, c2_state,
        c1_call_c2, c1_use_fm, c1_state, library_function, retstr;
#elif TEST_GROUP == 5
    Fn library_function, my_systick_handler, s_add, s_scale, s_via_helper, s_apply, s_use_host, s_get_a, s_get_b,
        s_print, s_text;
#elif TEST_GROUP == 4
    Fn eh_selftest, eh_throw, eh_dyncast, ehx_catch_cross, cppsh_make, cppsh_compute, cppsh_instances, cppsh_free;
#else
    Fn prov_value, cons_call, lf, lf2, lf3, xxx;
#if TEST_GROUP == 3
    Fn library_function, retstr;
#endif
#endif
} g_fx;

#if TEST_GROUP == 4
}  // namespace
/* In embed-arena.S: an exception unwinds through it, so it carries unwind info. */
extern "C" u32 call_mod(fn2, u32 /*got*/, u32 /*a*/, u32 /*b*/);
namespace {
#else
__attribute__((naked)) u32 call_mod(fn2, u32 /*got*/, u32 /*a*/, u32 /*b*/) {
    __asm__ volatile(
        "push {r10, lr}\n mov r10, r1\n mov r12, r0\n"
        "mov r0, r2\n mov r1, r3\n blx r12\n pop {r10, pc}\n");
}
#endif
inline u32 call(const Fn& f, u32 a = 0, u32 b = 0) { return call_mod(f.f, f.got, a, b); }

const char* g_boot_error = "boot_load did not run";
const char* g_missing = nullptr;   // from the loader's first_missing_import()
unsigned g_boot_cpu = 99;
u32 g_text_addr, g_data_got;

constexpr std::size_t kStageEach = 32 * 1024;
#if TEST_GROUP == 1
constexpr int kCount = 4, kSetCount = 3;
const char* const kNames[kCount] = {"cpp3", "c2", "c1", "mylib1"};
#elif TEST_GROUP == 5
constexpr int kCount = 2, kSetCount = 1;
const char* const kNames[kCount] = {"mylib1", "slib"};
#elif TEST_GROUP == 4
constexpr int kCount = 3, kSetCount = 2;
const char* const kNames[kCount] = {"libcppeh", "ehx_catch", "libcppsh"};
#elif TEST_GROUP == 2
constexpr int kCount = 3, kSetCount = 2;
const char* const kNames[kCount] = {"libprov", "libcons", "mylib2"};
#else
constexpr int kCount = 4, kSetCount = 2;
const char* const kNames[kCount] = {"libprov", "libcons", "mylib1", "mylib2"};
#endif
alignas(8) std::byte g_stage[kCount][kStageEach];
std::size_t g_size[kCount];

bool read_fixtures() {
    for (int i = 0; i < kCount; ++i) {
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

std::span<const std::byte> g_flash_image[kCount];
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
    for (int i = 0; i < kCount; ++i) {
        std::span<std::byte> img{g_stage[i], g_size[i]};
#if defined(TEST_XIP_STATIC)
        xip::stat::bind_host_imports(img, kXipBase + off);
#else
        xipl::patch_main_imports(img, kXipBase + off);
#endif
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
    static std::span<const std::byte> imgs[kSetCount];
    for (int i = 0; i < kSetCount; ++i) imgs[i] = {g_stage[i], g_size[i]};
    static reloc::module_set cluster;
    auto rs = reloc::load_set(std::span<const std::span<const std::byte>>(imgs, kSetCount),
                              {arena, total * 3 / 4}, scl::symtab::source(table));
    if (!rs) { g_boot_error = "reloc::load_set"; return false; }
    cluster = *rs;
    static reloc::module m1;
    auto r1 = reloc::load({g_stage[kCount - 1], g_size[kCount - 1]}, {arena + total * 3 / 4, total / 4}, scl::symtab::source(table));
    if (!r1) { g_boot_error = "reloc::load mylib1"; return false; }
    m1 = *r1;
#define GET(field, h) ok = ok && set(g_fx.field, reinterpret_cast<fn2>(reloc::sym(h, #field)), 0, "missing " #field)
#if TEST_GROUP == 1
    GET(cpp3_method, cluster); GET(cpp3_recurse, cluster); GET(cpp3_float_cross, cluster);
    GET(cpp3_heap, cluster); GET(c2_add, cluster); GET(c2_param, cluster); GET(c2_state, cluster);
    GET(c1_call_c2, cluster); GET(c1_use_fm, cluster); GET(c1_state, cluster);
    GET(library_function, m1); GET(retstr, m1);
#elif TEST_GROUP == 5
    GET(library_function, cluster); GET(my_systick_handler, cluster);
    GET(s_add, m1); GET(s_scale, m1); GET(s_via_helper, m1); GET(s_apply, m1); GET(s_use_host, m1);
    GET(s_get_a, m1); GET(s_get_b, m1); GET(s_print, m1); GET(s_text, m1);
#elif TEST_GROUP == 4
    GET(eh_selftest, cluster); GET(eh_throw, cluster); GET(eh_dyncast, cluster); GET(ehx_catch_cross, cluster);
    GET(cppsh_make, m1); GET(cppsh_compute, m1); GET(cppsh_instances, m1); GET(cppsh_free, m1);
#else
    GET(prov_value, cluster); GET(cons_call, cluster);
    GET(lf, m1); GET(lf2, m1); GET(lf3, m1); GET(xxx, m1);
#endif
#elif defined(TEST_LOADER_NOLIB)
    fw::dl_set_symtab(kMainSyms, kNumSyms);
    fw::dl_set_lazy_binding(false);
    fw::dl_enable_cross_call(true);
    fw::dl_cross_trace(0);
    static fw::handle* h[kCount];
    for (int i = 0; i < kCount; ++i) {
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
#if TEST_GROUP == 1
    GET(cpp3_method, 0); GET(cpp3_recurse, 0); GET(cpp3_float_cross, 0); GET(cpp3_heap, 0);
    GET(c2_add, 1); GET(c2_param, 1); GET(c2_state, 1);
    GET(c1_call_c2, 2); GET(c1_use_fm, 2); GET(c1_state, 2);
    GET(library_function, 3); GET(retstr, 3);
#elif TEST_GROUP == 5
    GET(library_function, 0); GET(my_systick_handler, 0);
    GET(s_add, 1); GET(s_scale, 1); GET(s_via_helper, 1); GET(s_apply, 1); GET(s_use_host, 1);
    GET(s_get_a, 1); GET(s_get_b, 1); GET(s_print, 1); GET(s_text, 1);
#elif TEST_GROUP == 4
    GET(eh_selftest, 0); GET(eh_throw, 0); GET(eh_dyncast, 0); GET(ehx_catch_cross, 1);
    GET(cppsh_make, 2); GET(cppsh_compute, 2); GET(cppsh_instances, 2); GET(cppsh_free, 2);
#else
    GET(prov_value, 0); GET(cons_call, 1);
    GET(lf, 2); GET(lf2, 2); GET(lf3, 2); GET(xxx, 2);
#endif
    g_missing = fw::dl_first_missing_import();
#elif defined(TEST_LOADER_NEWLIB)
    static firmware::link_context ctx;
    ctx.set_symtab(reinterpret_cast<const firmware::sym_entry*>(kMainSyms), kNumSyms);
    static firmware::dynamic_linker linker(ctx);
    static firmware::library* h[kCount];
    for (int i = 0; i < kCount; ++i) {
        loader::memory_byte_source src{std::span<const std::byte>{g_stage[i], g_size[i]}};
        auto r = linker.load_from_source(src);
        if (!r) { g_boot_error = "load_from_source"; return false; }
        h[i] = *r;
    }
    auto sym = [](firmware::library* m, const char* n) {
        auto v = linker.lookup(m, n);
        return v ? reinterpret_cast<fn2>(*v) : nullptr;
    };
#define GET(field, i) ok = ok && set(g_fx.field, sym(h[i], #field), 0, "missing " #field)
#if TEST_GROUP == 1
    GET(cpp3_method, 0); GET(cpp3_recurse, 0); GET(cpp3_float_cross, 0); GET(cpp3_heap, 0);
    GET(c2_add, 1); GET(c2_param, 1); GET(c2_state, 1);
    GET(c1_call_c2, 2); GET(c1_use_fm, 2); GET(c1_state, 2);
    GET(library_function, 3); GET(retstr, 3);
#elif TEST_GROUP == 5
    GET(library_function, 0); GET(my_systick_handler, 0);
    GET(s_add, 1); GET(s_scale, 1); GET(s_via_helper, 1); GET(s_apply, 1); GET(s_use_host, 1);
    GET(s_get_a, 1); GET(s_get_b, 1); GET(s_print, 1); GET(s_text, 1);
#elif TEST_GROUP == 4
    GET(eh_selftest, 0); GET(eh_throw, 0); GET(eh_dyncast, 0); GET(ehx_catch_cross, 1);
    GET(cppsh_make, 2); GET(cppsh_compute, 2); GET(cppsh_instances, 2); GET(cppsh_free, 2);
#else
    GET(prov_value, 0); GET(cons_call, 1);
    GET(lf, 2); GET(lf2, 2); GET(lf3, 2); GET(xxx, 2);
#endif
    g_missing = linker.first_missing_import();
#elif defined(TEST_LOADER_OPTIMAL)
    alignas(4) static std::byte blob[2048];
    static scl::symtab::view table;
    const u32 at = reinterpret_cast<u32>(blob);
    table = scl::symtab::view(blob, scl::symtab::build_blob(kMainSyms, kNumSyms, blob, sizeof blob, at), at);
    if (!table.valid()) { g_boot_error = "symbol blob"; return false; }
    xip::nopic::set_main_symbols(scl::symtab::source(table));
    static optimal::Module h[kCount];
    for (int i = 0; i < kCount; ++i) {
        if (!h[i].load(std::span<const std::byte>{g_stage[i], g_size[i]})) {
            g_boot_error = "nopic load";
            return false;
        }
    }
    xip::nopic::resolve_all();   // the fixtures that import from each other
#define GET(field, i) ok = ok && set(g_fx.field, reinterpret_cast<fn2>(h[i].sym(#field)), 0, "missing " #field)
#if TEST_GROUP == 1
    GET(cpp3_method, 0); GET(cpp3_recurse, 0); GET(cpp3_float_cross, 0); GET(cpp3_heap, 0);
    GET(c2_add, 1); GET(c2_param, 1); GET(c2_state, 1);
    GET(c1_call_c2, 2); GET(c1_use_fm, 2); GET(c1_state, 2);
    GET(library_function, 3); GET(retstr, 3);
#elif TEST_GROUP == 5
    GET(library_function, 0); GET(my_systick_handler, 0);
    GET(s_add, 1); GET(s_scale, 1); GET(s_via_helper, 1); GET(s_apply, 1); GET(s_use_host, 1);
    GET(s_get_a, 1); GET(s_get_b, 1); GET(s_print, 1); GET(s_text, 1);
#elif TEST_GROUP == 4
    GET(eh_selftest, 0); GET(eh_throw, 0); GET(eh_dyncast, 0); GET(ehx_catch_cross, 1);
    GET(cppsh_make, 2); GET(cppsh_compute, 2); GET(cppsh_instances, 2); GET(cppsh_free, 2);
#else
    GET(prov_value, 0); GET(cons_call, 1);
    GET(lf, 2); GET(lf2, 2); GET(lf3, 2); GET(xxx, 2);
#endif
    g_missing = xip::nopic::first_missing_import();
#elif defined(TEST_LOADER_STATIC)
    static elf_loader::symbol_table syms;
    syms.add_entries(kMainSyms, kNumSyms);
    static scl::Module h[kCount];
    const std::size_t slot = static_cast<std::size_t>(_simage_arena_end - _simage_arena_start) / kCount;
    for (int i = 0; i < kCount; ++i) {
        if (!h[i].load_buf(reinterpret_cast<const uint8_t*>(g_stage[i]), g_size[i],
                           _simage_arena_start + i * slot, slot, &syms)) {
            g_boot_error = "Module::load_buf";
            return false;
        }
    }
#define GET(field, i) ok = ok && set(g_fx.field, h[i].sym<fn2>(#field), 0, "missing " #field)
#if TEST_GROUP == 5
    GET(library_function, 0); GET(my_systick_handler, 0);
    GET(s_add, 1); GET(s_scale, 1); GET(s_via_helper, 1); GET(s_apply, 1); GET(s_use_host, 1);
    GET(s_get_a, 1); GET(s_get_b, 1); GET(s_print, 1); GET(s_text, 1);
#elif TEST_GROUP == 4
    GET(eh_selftest, 0); GET(eh_throw, 0); GET(eh_dyncast, 0); GET(ehx_catch_cross, 1);
    GET(cppsh_make, 2); GET(cppsh_compute, 2); GET(cppsh_instances, 2); GET(cppsh_free, 2);
#else
    GET(prov_value, 0); GET(cons_call, 1);
    GET(library_function, 2); GET(retstr, 2);
    GET(lf, 3); GET(lf2, 3); GET(lf3, 3); GET(xxx, 3);
#endif
    for (int i = 0; i < kCount && !g_missing; ++i) g_missing = h[i].first_missing_import();
#elif defined(TEST_LOADER_XIP)
    xipl::set_symtab(kMainSyms, kNumSyms);
    if (!provision_flash()) return false;
#if defined(TEST_PSRAM)
    if (!psram::init()) { g_boot_error = "psram::init"; return false; }
#endif
#if defined(TEST_XIP_STATIC)
    static xip::stat::SModule h[kCount];
    for (int i = 0; i < kCount; ++i) {
        if (!xip::stat::load(g_flash_image[i], h[i])) { g_boot_error = "xip::stat::load"; return false; }
    }
#define GET(field, i) ok = ok && set(g_fx.field, reinterpret_cast<fn2>(xip::stat::resolve(h[i], #field)), \
                                     h[i].got_base, "missing " #field)
#else
    static void* h[kCount];
    for (int i = 0; i < kCount; ++i) {
        h[i] = xipl::dlopen(g_flash_image[i], kNames[i]);
        if (!h[i]) { g_boot_error = "xip dlopen"; return false; }
        if (!xipl::is_xip(h[i])) { g_boot_error = "fixture was not loaded XIP"; return false; }
    }
#define GET(field, i) ok = ok && set(g_fx.field, reinterpret_cast<fn2>(xipl::dlsym(h[i], #field)), \
                                     xipl::get_got_base(h[i]), "missing " #field)
#endif
#if TEST_GROUP == 1
    GET(cpp3_method, 0); GET(cpp3_recurse, 0); GET(cpp3_float_cross, 0); GET(cpp3_heap, 0);
    GET(c2_add, 1); GET(c2_param, 1); GET(c2_state, 1);
    GET(c1_call_c2, 2); GET(c1_use_fm, 2); GET(c1_state, 2);
    GET(library_function, 3); GET(retstr, 3);
#elif TEST_GROUP == 5
    GET(library_function, 0); GET(my_systick_handler, 0);
    GET(s_add, 1); GET(s_scale, 1); GET(s_via_helper, 1); GET(s_apply, 1); GET(s_use_host, 1);
    GET(s_get_a, 1); GET(s_get_b, 1); GET(s_print, 1); GET(s_text, 1);
#elif TEST_GROUP == 4
    GET(eh_selftest, 0); GET(eh_throw, 0); GET(eh_dyncast, 0); GET(ehx_catch_cross, 1);
    GET(cppsh_make, 2); GET(cppsh_compute, 2); GET(cppsh_instances, 2); GET(cppsh_free, 2);
#else
    GET(prov_value, 0); GET(cons_call, 1);
    GET(lf, 2); GET(lf2, 2); GET(lf3, 2); GET(xxx, 2);
#endif
    g_missing = xipl::first_missing_import();
#endif
    return ok;
}

constexpr unsigned kGold = 16;
u32 g_gold[kGold], g_hits_base, g_c1_base, g_widget_gold;

#if TEST_GROUP == 4
[[noreturn]] void on_terminate() {
    say("  FAIL: std::terminate");
    const std::type_info* t = abi::__cxa_current_exception_type();
    say(t ? ", exception in flight: " : ", no exception in flight");
    if (t) say(t->name());
    say("\nRESULT: FAIL\n");
    hw_result::fail();
    for (;;) {}
}
#endif

/* Boot time: core 1 is still in the bootrom and the kernel's main() has not run. */
void boot_load() {
    g_boot_cpu = port_cpu_id();
#if TEST_GROUP == 4
    std::set_terminate(on_terminate);
#endif
    if (!read_fixtures()) return;
#if defined(TEST_PSRAM) && !defined(TEST_LOADER_XIP)
    if (!psram::init()) { g_boot_error = "psram::init"; return; }
#endif
    if (!load_fixtures()) return;
#if defined(TEST_EXPECT_MISSING)
    return;   // this image hides a symbol on purpose: its calls would fault
#endif
    if (g_missing) {
        static char msg[80];
        std::snprintf(msg, sizeof msg, "missing import: %s", g_missing);
        g_boot_error = msg;
        return;
    }

#if TEST_GROUP == 1
    g_text_addr = reinterpret_cast<u32>(g_fx.cpp3_method.f);
    g_data_got = g_fx.cpp3_method.got;
#elif TEST_GROUP == 5
    g_text_addr = reinterpret_cast<u32>(g_fx.s_add.f);
    g_data_got = g_fx.s_add.got;
#elif TEST_GROUP == 4
    g_text_addr = reinterpret_cast<u32>(g_fx.eh_dyncast.f);
    g_data_got = g_fx.eh_dyncast.got;
#else
    g_text_addr = reinterpret_cast<u32>(g_fx.lf.f);
    g_data_got = g_fx.lf.got;
#endif
    if ((g_text_addr >> 24) != TEST_EXPECT_TEXT_TOP) { g_boot_error = "fixture text in the wrong memory"; return; }
#if defined(TEST_EXPECT_DATA_TOP)
    if ((g_data_got >> 24) != TEST_EXPECT_DATA_TOP) { g_boot_error = "fixture data in the wrong memory"; return; }
#endif
#if TEST_GROUP == 1
    for (unsigned i = 0; i < kGold; ++i) g_gold[i] = call(g_fx.cpp3_method, 7, i * 5u + 1u);
    if (std::strcmp(reinterpret_cast<const char*>(call(g_fx.retstr)), "Hello mylib1!") != 0 &&
        std::strcmp(reinterpret_cast<const char*>(call(g_fx.retstr)), "Hello from mylib1!") != 0) {
        g_boot_error = "mylib1 retstr";
        return;
    }
    g_hits_base = call(g_fx.c2_state);
    g_c1_base = call(g_fx.c1_state);
#elif TEST_GROUP == 5
    if (call(g_fx.s_get_a) != 15u || call(g_fx.s_get_b) != 30u) { g_boot_error = "slib constructor"; return; }
#elif TEST_GROUP == 4
    if (call(g_fx.eh_dyncast) != 2u) { g_boot_error = "eh_dyncast"; return; }
    if (call(g_fx.ehx_catch_cross, 0) != 0u) { g_boot_error = "ehx_catch_cross(0)"; return; }
    {
        const u32 w = call(g_fx.cppsh_make, 7);
        g_widget_gold = call(g_fx.cppsh_compute, w, 5);
        call(g_fx.cppsh_free, w);
    }
#else
    if (call(g_fx.prov_value, 5) != 105u) { g_boot_error = "libprov prov_value"; return; }
#if TEST_GROUP == 3
    if (std::strcmp(reinterpret_cast<const char*>(call(g_fx.retstr)), "Hello mylib1!") != 0) {
        g_boot_error = "mylib1 retstr";
        return;
    }
#endif
#endif
    g_boot_error = nullptr;
    g_trace = 0;
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

#if TEST_GROUP == 5
// ---- TIMER1 alarms, one per core: the kernel owns TIMER0 ----------------------------
constexpr u32 kTimer1 = 0x400B8000u, kTimelr = kTimer1 + 0x0Cu, kAlarm0 = kTimer1 + 0x10u;
constexpr u32 kIntr = kTimer1 + 0x3Cu, kInte = kTimer1 + 0x40u;
constexpr u32 kPeriodUs = 200;
constexpr unsigned kIrqBase = 4;   // TIMER1_IRQ_0
inline volatile u32& reg(u32 a) { return *reinterpret_cast<volatile u32*>(a); }

constexpr u32 kVtor = 0xE000ED08u;
constexpr unsigned kVectors = 16u + 52u;
alignas(512) u32 g_ram_vectors[kVectors];

volatile unsigned g_irq_stop, g_irq_hits[2], g_irq_errors, g_irq_toggles;

void timer1_irq0();
void timer1_irq1();

void timer1_init() {
    constexpr u32 kResetBit = 24u;
    reg(0x40020000u) &= ~(1u << kResetBit);
    while (!(reg(0x40020008u) & (1u << kResetBit))) {
    }
    reg(0x40108000u + 0x24u + 4u) = 12u;  // TICKS TIMER1_CYCLES: clk_ref 12 MHz -> 1 us
    reg(0x40108000u + 0x24u) = 1u;
    reg(kInte) = 3u;
    /* The board's table has no TIMER1 entries, so patch a RAM copy, as the WeAct test does. */
    const auto* flash = reinterpret_cast<const u32*>(reg(kVtor));
    for (unsigned i = 0; i < kVectors; ++i) g_ram_vectors[i] = flash[i];
    g_ram_vectors[16u + kIrqBase] = reinterpret_cast<u32>(&timer1_irq0);
    g_ram_vectors[16u + kIrqBase + 1u] = reinterpret_cast<u32>(&timer1_irq1);
}

void timer1_start_on_this_core() {
    const unsigned me = port_cpu_id() & 1u;
    __asm__ volatile("cpsid i" : : : "memory");
    reg(kVtor) = reinterpret_cast<u32>(g_ram_vectors);   // VTOR is per core
    __asm__ volatile("dsb\n isb\n cpsie i" : : : "memory");
    reg(kAlarm0 + me * 4u) = reg(kTimelr) + kPeriodUs;
    reg(0xE000E100u) = 1u << (kIrqBase + me);
}

void timer1_isr(unsigned me) {
    reg(kIntr) = 1u << me;
    if (g_irq_stop) return;
    /* Pure calls: nothing shared. They run on top of a thread that may be in a module. */
    if (call(g_fx.s_apply, 3) != 127u) g_irq_errors = g_irq_errors + 1u;
    if (call(g_fx.library_function, 39) != 62u) g_irq_errors = g_irq_errors + 1u;
    /* my_systick_handler counts in the module, with no lock of its own. */
    app_lock();
    const u32 toggled = call(g_fx.my_systick_handler, 1);
    g_irq_toggles = g_irq_toggles + toggled;
    g_irq_hits[me] = g_irq_hits[me] + 1u;
    app_unlock();
    reg(kAlarm0 + me * 4u) = reg(kTimelr) + kPeriodUs;
}
void timer1_irq0() { timer1_isr(0); }
void timer1_irq1() { timer1_isr(1); }
#endif

constexpr unsigned kWorkers = 4, kIters = TEST_ITERS;
constexpr u32 kHitsPerIter = 5;  // c2_add 1, float_cross 1, recurse(4) 2, c1_call_c2 1
struct Worker {
    unsigned id;
    volatile unsigned errors, done, cores_seen, unpublished;
    volatile unsigned first_bad;
} g_workers[kWorkers];
thread::stack::element_t g_stacks[kWorkers][2048];

/* Every fixture call, with its traces and result on the console, like the WeAct apps. */
void show(const char* what, u32 v) {
    char l[100];
    std::snprintf(l, sizeof l, "  %s = %u\n", what, static_cast<unsigned>(v));
    say(l);
}
void walkthrough(unsigned core) {
    char l[64];
    std::snprintf(l, sizeof l, "\n--- fixture calls on core %u ---\n", core);
    say(l);
    g_trace = 1;
#if TEST_GROUP == 1
    show("cpp3_method(7,5)", call(g_fx.cpp3_method, 7, 5));
    show("c1_call_c2(7)", call(g_fx.c1_call_c2, 7));
    show("c2_add(10,20)", call(g_fx.c2_add, 10, 20));
    show("c2_param()", call(g_fx.c2_param));
    show("c1_use_fm()", call(g_fx.c1_use_fm));
    show("cpp3_heap(8)", call(g_fx.cpp3_heap, 8));
    show("cpp3_recurse(4)", call(g_fx.cpp3_recurse, 4));
    show("cpp3_float_cross(10)", call(g_fx.cpp3_float_cross, 10));
    show("library_function(42)", call(g_fx.library_function, 42));
    say("  retstr() = \"");
    say(reinterpret_cast<const char*>(call(g_fx.retstr)));
    say("\"\n");
    show("c2_state()", call(g_fx.c2_state));
    show("c1_state()", call(g_fx.c1_state));
#elif TEST_GROUP == 5
    show("s_add(1,2)", call(g_fx.s_add, 1, 2));
    show("s_scale(2)", call(g_fx.s_scale, 2));
    show("s_via_helper(1)", call(g_fx.s_via_helper, 1));
    show("s_apply(2)", call(g_fx.s_apply, 2));
    show("s_use_host(1)", call(g_fx.s_use_host, 1));
    show("s_get_a()", call(g_fx.s_get_a));
    show("s_get_b()", call(g_fx.s_get_b));
    say("  s_text() = \"");
    say(reinterpret_cast<const char*>(call(g_fx.s_text)));
    say("\"\n");
    call(g_fx.s_print);
    show("library_function(39)", call(g_fx.library_function, 39));
#elif TEST_GROUP == 4
    show("eh_dyncast(0)", call(g_fx.eh_dyncast));
    show("ehx_catch_cross(0)", call(g_fx.ehx_catch_cross, 0));
#if defined(TEST_EH_THROW)
    show("eh_selftest(1) [throw and catch inside libcppeh]", call(g_fx.eh_selftest, 1));
    show("ehx_catch_cross(1) [throw in libcppeh, catch in ehx_catch]", call(g_fx.ehx_catch_cross, 1));
    try {
        call(g_fx.eh_throw, 1);
        say("  eh_throw(1) returned instead of throwing\n");
    } catch (const cross_error& e) {
        show("eh_throw(1) [throw in libcppeh, catch in main], code", e.code());
    }
#endif
    show("cppsh_instances()", call(g_fx.cppsh_instances));
    {
        const u32 w = call(g_fx.cppsh_make, 7);
        show("cppsh_compute(w,5)", call(g_fx.cppsh_compute, w, 5));
        show("cppsh_instances() after make", call(g_fx.cppsh_instances));
        call(g_fx.cppsh_free, w);
        show("cppsh_instances() after free", call(g_fx.cppsh_instances));
    }
#else
    show("prov_value(5)", call(g_fx.prov_value, 5));
    show("cons_call(5)", call(g_fx.cons_call, 5));
    show("lf(7)", call(g_fx.lf, 7));
    show("lf2(7)", call(g_fx.lf2, 7));
    show("lf3(7)", call(g_fx.lf3, 7));
    show("xxx()", call(g_fx.xxx));
#if TEST_GROUP == 3
    show("library_function(42)", call(g_fx.library_function, 42));
    say("  retstr() = \"");
    say(reinterpret_cast<const char*>(call(g_fx.retstr)));
    say("\"\n");
#endif
#endif
    g_trace = 0;
}

volatile unsigned g_walked;   // pinned workers done with their walkthrough

void* worker(void* arg) {
    auto* w = static_cast<Worker*>(arg);
    if (w->id < 2) {
        this_thread::thread().cpu_affinity(1u << w->id);
        if (port_cpu_id() != w->id) this_thread::yield();
    }
    if (!g_published) w->unpublished = 1;
    __asm__ volatile("dmb" : : : "memory");

    // One core at a time prints; nobody else calls a fixture until both are done.
    if (w->id < 2) {
        while (g_walked != w->id) sysclock.sleep_for(5);
        walkthrough(w->id);
#if TEST_GROUP == 1
        if (w->id == 1) {
            g_hits_base = call(g_fx.c2_state);
            g_c1_base = call(g_fx.c1_state);
            say("\n--- both cores now call the fixtures together (traces off) ---\n");
        }
#else
        if (w->id == 1) say("\n--- both cores now call the fixtures together (traces off) ---\n");
#endif
        __asm__ volatile("dmb" : : : "memory");
        g_walked = w->id + 1;
    }
    while (g_walked < 2) sysclock.sleep_for(5);
#if TEST_GROUP == 5
    if (w->id < 2) timer1_start_on_this_core();
#endif

    auto expect = [&](bool ok, unsigned code) {
        if (!ok) { ++w->errors; if (!w->first_bad) w->first_bad = code; }
    };
    for (unsigned i = 0; i < kIters; ++i) {
        const unsigned k = i % kGold;
#if TEST_GROUP == 1
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

#elif TEST_GROUP == 5
        // Interrupted by the timer ISR, which enters the same modules with its own r10.
        expect(call(g_fx.s_add, i, 2) == i + 2u + 15u, 1);
        expect(call(g_fx.s_scale, i) == i * 42u, 2);
        expect(call(g_fx.s_via_helper, i) == (i + 100u) + (i + 1u + 15u), 3);
        expect(call(g_fx.s_apply, i) == i * 42u + 1u, 4);
        expect(call(g_fx.s_use_host, i) == i * 2u + 1u + 1000u, 5);
        expect(call(g_fx.library_function, i) == i + 3u + 20u, 6);
#elif TEST_GROUP == 4
        // Concurrent on both cores: dynamic_cast and the non-throwing path.
        expect(call(g_fx.eh_dyncast) == 2u, 1);
        expect(call(g_fx.ehx_catch_cross, 0) == 0u, 2);

        // libstdc++ here is single-threaded: exception state and cppsh's instance
        // counter are shared, so throws and make/free hold main's lock.
        app_lock();
#if defined(TEST_EH_THROW)
        const u32 self = call(g_fx.eh_selftest, 1);
        const u32 cross = call(g_fx.ehx_catch_cross, 1);
        u32 caught = 0;
        try {
            call(g_fx.eh_throw, 1);
        } catch (const cross_error& e) {
            caught = e.code();
        }
#endif
        const u32 before = call(g_fx.cppsh_instances);
        const u32 widget = call(g_fx.cppsh_make, 7);
        const u32 computed = call(g_fx.cppsh_compute, widget, 5);
        const u32 during = call(g_fx.cppsh_instances);
        call(g_fx.cppsh_free, widget);
        app_unlock();
#if defined(TEST_EH_THROW)
        expect(self == 98u, 3);
        expect(cross == 1007u, 4);
        expect(caught == 7u, 5);
#endif
        expect(computed == g_widget_gold, 6);
        expect(during == before + 1u, 7);
#else
        // Concurrent on both cores: results depend only on read-only state.
        expect(call(g_fx.prov_value, i) == i + 100u, 1);
        expect(call(g_fx.cons_call, i) == i + 101u, 2);     // libcons -> libprov
        expect(call(g_fx.lf, i) == i + 3u + 20u, 3);
        expect(call(g_fx.lf2, i) == i * 3u - 5u + 7u, 4);

        // lf3 writes mylib2's own bbb, and xxx prints, so hold main's lock for lf3.
        app_lock();
        const u32 three = call(g_fx.lf3, i);                 // calls w() in main
        app_unlock();
        expect(three == 2u * ((i + 23u) + (i * 3u + 2u) + (i + 20u)), 5);
#if TEST_GROUP == 3
        expect(call(g_fx.library_function, 39) == 62u, 6);
#endif
#endif
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

#if TEST_GROUP == 5
#endif

int os_main(int, char*[]) {
    this_thread::thread().cpu_affinity(1u << 0);
    led::init();
    say("\n" TEST_NAME ": fixtures loaded at boot on the RP2350, then SMP\n");

#if defined(TEST_EXPECT_MISSING)
    /* Negative test: main's table hides host_f, which slib imports. */
#if defined(TEST_LOADER_RELOC)
    const bool reported = g_boot_error != nullptr && std::strstr(g_boot_error, "reloc::load") != nullptr;
    say(reported ? "  ok    the load was refused\n" : "  FAIL: the load was not refused\n");
#else
    const bool reported = g_missing != nullptr && std::strcmp(g_missing, TEST_EXPECT_MISSING) == 0;
    say("  first_missing_import() = ");
    say(g_missing ? g_missing : "(none)");
    say("\n");
    say(reported ? "  ok    the missing import was reported by name\n"
                 : "  FAIL: the missing import was not reported\n");
#endif
    say(reported ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    if (reported) hw_result::ok();
    hw_result::fail();
#endif

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
    sayf("  text 0x%08x, r10 0x%08x; boot locks=%u depth=%u\n", g_text_addr, g_data_got, g_locks, g_lock_max);
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

#if TEST_GROUP == 5
    timer1_init();
#endif
    say("  core 0: launching core 1; SMP starts now\n");
    multicore::launch_core1(port_smp_secondary_start, port_core1_stack_top());
    port_smp_core0_ipi_enable();

    unsigned done = 0;
    for (unsigned t = 0; t < 1600 && done < kWorkers; ++t) {
        sysclock.sleep_for(100);
        done = 0;
        for (auto& w : g_workers) done += w.done;
    }

#if TEST_GROUP == 5
    g_irq_stop = 1;
    sysclock.sleep_for(5);
    const u32 hits0 = g_irq_hits[0], hits1 = g_irq_hits[1];
    sayf("  interrupts on core0=%u core1=%u; errors in ISR=%u\n", hits0, hits1, g_irq_errors);
    sayf("  my_systick_handler toggles=%u (expected %u)\n", g_irq_toggles, (hits0 + hits1) / 500u);
#endif
    unsigned errors = 0, unpublished = 0, first_bad = 0;
    for (auto& w : g_workers) {
        errors += w.errors;
        unpublished += w.unpublished;
        if (!first_bad) first_bad = w.first_bad;
    }
    [[maybe_unused]] const u32 total = kWorkers * kIters;
    sayf("  workers done=%u errors=%u first failing check=%u\n", done, errors, first_bad);
#if TEST_GROUP == 1
    const u32 hits = call(g_fx.c2_state) - g_hits_base, c1 = call(g_fx.c1_state) - g_c1_base;
    sayf("  c2 hits=%u of %u; c1 calls=%u of %u\n", hits, kHitsPerIter * total, c1, total);
#endif
    sayf("  free threads saw cores 0x%x 0x%x\n", g_workers[2].cores_seen, g_workers[3].cores_seen);
    sayf("  loader locks=%u (after publish=%u) max depth=%u\n", g_locks, g_locks_after_publish, g_lock_max);
    sayf("  cleans=%u invalidates=%u\n", g_cleans, g_invalidates);

    check(done == kWorkers, "all workers finished");
    check(unpublished == 0, "every worker saw the published fixtures");
    check(errors == 0, "fixtures returned correct values on both cores");
#if TEST_GROUP == 1
    check(hits == kHitsPerIter * total, "c2 counted every locked call");
    check(c1 == total, "c1 counted every locked call");
#endif
#if TEST_GROUP == 5
    check(hits0 >= 100u && hits1 >= 100u, "the timer interrupt ran on both cores");
    check(g_irq_errors == 0u, "fixtures returned correct values inside the interrupts");
    check(g_irq_toggles == (hits0 + hits1) / 500u, "every locked call from an interrupt was counted");
#endif
    check(g_workers[0].cores_seen == 1u && g_workers[1].cores_seen == 2u, "pinned workers stayed on their core");
    check(boot_locks > 0, "the load took the loader lock");
    check(g_cleans > 0 && g_invalidates > 0, "the loader made its cache calls");
#if defined(TEST_CROSS_CALL_LOOKUP) && !(TEST_GROUP == 5 && defined(TEST_XIP_STATIC))
    check(g_locks_after_publish > 0, "cross-module calls used the call-time lookup");
#else
    check(g_locks_after_publish == 0, "no loader code ran after publication");
#endif
    say(ok ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    if (ok) hw_result::ok();
    hw_result::fail();
    return 0;
}
