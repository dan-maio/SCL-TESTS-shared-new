/**
 * @file main.cpp
 * @brief Shared-library loading on the dual-core RP2350 under µOS++ SMP.
 *
 * The rule under test: every load and patch happens on core 0, at boot, before
 * the kernel's main() and before core 1 leaves the bootrom. Only then does the SMP
 * system start, and from there on both cores only CALL the module.
 *
 *   boot_load()      runs as a C++ constructor, after clocks/UART/heap are up
 *   os_main()        checks the load, launches core 1, runs the workers
 *   workers          two pinned to a core and two free to migrate
 *
 * Checks: load on core 0 before the scheduler; no loader lock taken after
 * publication; module text in the expected memory; correct values on both cores;
 * no lost shared increments.
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

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>

using namespace os::rtos;

extern "C" {
void port_smp_secondary_start(void);
unsigned port_core1_stack_top(void);
unsigned port_cpu_id(void);
void port_smp_core0_ipi_enable(void);
extern const std::byte smpmod_image_start[], smpmod_image_end[];
extern std::uint8_t _simage_arena_start[], _simage_arena_end[];
}

namespace os::rtos::scheduler {
extern thread* os_idle_thread_core[OS_NCPU];
}

// ---- what the module imports from main --------------------------------------
namespace {
constexpr std::uint32_t kSioSpinlock = 0xD0000100u + 30u * 4u;  // lock 0 is the kernel's
std::uint32_t g_primask[2];
}  // namespace

extern "C" {
void app_lock(void) {
    std::uint32_t pm;
    __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(pm) : : "memory");
    while (*reinterpret_cast<volatile std::uint32_t*>(kSioSpinlock) == 0) {
    }
    __asm__ volatile("dmb" : : : "memory");
    g_primask[port_cpu_id() & 1u] = pm;
}
void app_unlock(void) {
    const std::uint32_t pm = g_primask[port_cpu_id() & 1u];
    __asm__ volatile("dmb" : : : "memory");
    *reinterpret_cast<volatile std::uint32_t*>(kSioSpinlock) = 1;
    __asm__ volatile("msr primask, %0" : : "r"(pm) : "memory");
}
unsigned app_cpu_id(void) { return port_cpu_id(); }

#if defined(TEST_LOADER_XIP)
/* dynamic-newlib reads main's relocation table from here. This firmware has none. */
extern const std::uint32_t _reloc[10];
const std::uint32_t _reloc[10] = {};
#endif

// ---- SCL hooks: count them, and do real XIP cache maintenance for PSRAM ------
volatile unsigned g_published;
volatile unsigned g_locks, g_lock_depth, g_lock_max, g_locks_off_core0, g_locks_after_publish;
volatile unsigned g_cleans, g_invalidates;

void scl_loader_lock(void) {
    ++g_locks;
    if (port_cpu_id() != 0) ++g_locks_off_core0;
    if (g_published) ++g_locks_after_publish;
    if (++g_lock_depth > g_lock_max) g_lock_max = g_lock_depth;
}
void scl_loader_unlock(void) { --g_lock_depth; }

namespace {
constexpr std::uint32_t kXipBase = 0x10000000u, kXipEnd = 0x12000000u, kXipMaint = 0x18000000u;
void xip_maint(const void* addr, std::size_t len, unsigned op) {
    const std::uint32_t a = reinterpret_cast<std::uint32_t>(addr);
    if (a >= kXipBase && a < kXipEnd) {
        const std::uint32_t o0 = (a - kXipBase) & ~7u, o1 = ((a - kXipBase) + len + 7u) & ~7u;
        for (std::uint32_t o = o0; o < o1; o += 8u)
            *reinterpret_cast<volatile std::uint8_t*>(kXipMaint + o + op) = 0;
    }
    __asm__ volatile("dsb sy\n isb" : : : "memory");
}
}  // namespace
void scl_cache_clean(const void* addr, size_t len) { ++g_cleans; xip_maint(addr, len, 3u); }
void scl_cache_invalidate(const void* addr, size_t len) { ++g_invalidates; xip_maint(addr, len, 2u); }

void scl_log_puts(const char* s) { uart::write(s); }
void scl_log_putc(char c) { uart::write(&c, 1); }
}

// ---- loaded module, filled at boot and read-only afterwards -------------------
namespace {

using mod_fn = std::uint32_t (*)(std::uint32_t, std::uint32_t);
struct Module {
    mod_fn magic, compute, fmix, note, shared, core_count, walk, rw;
    std::uint32_t got;
};
Module g_mod;
const char* g_boot_error = "boot_load did not run";
unsigned g_boot_cpu = 99;
std::uint32_t g_text_addr;

/* Enter the module with r10 = its GOT base. Harmless for the r10-free model. */
__attribute__((naked)) std::uint32_t
call_mod(mod_fn, std::uint32_t /*got*/, std::uint32_t /*a*/, std::uint32_t /*b*/) {
    __asm__ volatile(
        "push {r10, lr}\n mov r10, r1\n mov r12, r0\n"
        "mov r0, r2\n mov r1, r3\n blx r12\n pop {r10, pc}\n");
}
inline std::uint32_t call(mod_fn f, std::uint32_t a = 0, std::uint32_t b = 0) {
    return call_mod(f, g_mod.got, a, b);
}

const scl_sym_entry kMainSyms[] = {
    {"app_lock", reinterpret_cast<std::uint32_t>(&app_lock)},
    {"app_unlock", reinterpret_cast<std::uint32_t>(&app_unlock)},
    {"app_cpu_id", reinterpret_cast<std::uint32_t>(&app_cpu_id)},
};

#if defined(TEST_STAGE)
/* Flash and PSRAM share the QMI, so copy through SRAM (psram-exec's board fix). */
alignas(8) std::byte g_stage[96 * 1024];
#endif

std::span<const std::byte> module_image() {
    const std::size_t n = static_cast<std::size_t>(smpmod_image_end - smpmod_image_start);
#if defined(TEST_STAGE)
    if (n > sizeof g_stage) return {};
    std::memcpy(g_stage, smpmod_image_start, n);
    return {g_stage, n};
#else
    return {smpmod_image_start, n};
#endif
}

bool load_module(std::span<const std::byte> image) {
#if defined(TEST_LOADER_RELOC)
    alignas(4) static std::byte blob[256];
    static scl::symtab::view table;
    const std::uint32_t at = reinterpret_cast<std::uint32_t>(blob);
    table = scl::symtab::view(blob, scl::symtab::build_blob(kMainSyms, 3, blob, sizeof blob, at), at);
    if (!table.valid()) { g_boot_error = "symbol blob"; return false; }
    static reloc::module m;
    auto r = reloc::load(image,
                         {reinterpret_cast<std::byte*>(_simage_arena_start),
                          static_cast<std::size_t>(_simage_arena_end - _simage_arena_start)},
                         scl::symtab::source(table));
    if (!r) { g_boot_error = "reloc::load"; return false; }
    m = *r;
    auto get = [](const char* n) { return reinterpret_cast<mod_fn>(reloc::sym(m, n)); };
    g_mod.got = 0;
#elif defined(TEST_LOADER_NOLIB)
    fw::dl_set_symtab(kMainSyms, 3);
    fw::dl_set_lazy_binding(false);
    static loader::memory_byte_source src{image};
    auto r = fw::dlopen_source(src, "smpmod.so");
    if (!r) { g_boot_error = "dlopen_source"; return false; }
    static fw::handle* h;
    h = *r;
    auto get = [](const char* n) {
        auto v = fw::dlsym(*h, n);
        return v ? reinterpret_cast<mod_fn>(*v) : nullptr;
    };
    g_mod.got = h->got_base;
#elif defined(TEST_LOADER_XIP)
    xip::newlib::set_symtab(kMainSyms, 3);
    static void* h;
    h = xip::newlib::dlopen(image, "smpmod");
    if (!h) { g_boot_error = "xip dlopen"; return false; }
    if (!xip::newlib::is_xip(h)) { g_boot_error = "module was not loaded XIP"; return false; }
    auto get = [](const char* n) { return reinterpret_cast<mod_fn>(xip::newlib::dlsym(h, n)); };
    g_mod.got = xip::newlib::get_got_base(h);
#endif
    g_mod.magic = get("mod_magic");
    g_mod.compute = get("mod_compute");
    g_mod.fmix = get("mod_fmix");
    g_mod.note = get("mod_note");
    g_mod.shared = get("mod_shared");
    g_mod.core_count = get("mod_core_count");
#if defined(TEST_BIG)
    g_mod.walk = get("mod_walk");
    g_mod.rw = get("mod_rw");
    if (!g_mod.walk || !g_mod.rw) { g_boot_error = "missing big export"; return false; }
#endif
    if (!g_mod.magic || !g_mod.compute || !g_mod.fmix || !g_mod.note || !g_mod.shared ||
        !g_mod.core_count) {
        g_boot_error = "missing export";
        return false;
    }
    return true;
}

/* Boot time: core 1 is still in the bootrom and the kernel's main() has not run. */
__attribute__((constructor(150))) void boot_load() {
    g_boot_cpu = port_cpu_id();
#if defined(TEST_PSRAM)
    if (!psram::init()) { g_boot_error = "psram::init"; return; }
#endif
    if (!load_module(module_image())) return;

    g_text_addr = reinterpret_cast<std::uint32_t>(g_mod.compute);
    const std::uint32_t top = g_text_addr >> 24;
    if (top != TEST_EXPECT_TEXT_TOP) { g_boot_error = "module text in the wrong memory"; return; }
#if defined(TEST_EXPECT_DATA_TOP)
    if ((g_mod.got >> 24) != TEST_EXPECT_DATA_TOP) { g_boot_error = "module data in the wrong memory"; return; }
#endif
    if (call(g_mod.magic) != 0x5CA1AB1Eu) { g_boot_error = "module constructor did not run"; return; }

    g_boot_error = nullptr;
    scl_publish_release();
    g_published = 1;
}

// ---- local copies of the module's pure functions, for checking ----------------
const std::uint32_t kTable[8] = {0x9E3779B1u, 0x85EBCA77u, 0xC2B2AE3Du, 0x27D4EB2Fu,
                                 0x165667B1u, 0xD3A2646Cu, 0xFD7046C5u, 0xB55A4F09u};
std::uint32_t expect_compute(std::uint32_t a, std::uint32_t b) {
    std::uint32_t h = a ^ b;
    for (unsigned i = 0; i < 8; ++i) h = (h ^ kTable[i]) * 31u + i;
    return h;
}
std::uint32_t expect_fmix(std::uint32_t a, std::uint32_t b) {
    volatile float x = (float)(a & 0xFFFFu), y = (float)(b & 0xFFFFu);
    return (std::uint32_t)(x * 0.5f + y * 0.25f);
}

#if defined(TEST_BIG)
std::uint32_t expect_walk(std::uint32_t start, std::uint32_t count) {
    std::uint32_t sum = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t k = (start + i * 97u) % (16u * 1024u);
        sum += (k * 2654435761u) ^ 0xA5A5A5A5u;
    }
    return sum;
}
#endif

constexpr unsigned kWorkers = 4, kIters = TEST_ITERS;
struct Worker {
    unsigned id;
    volatile unsigned errors, done, cores_seen, unpublished;
} g_workers[kWorkers];
thread::stack::element_t g_stacks[kWorkers][1024];

void* worker(void* arg) {
    auto* w = static_cast<Worker*>(arg);
    if (w->id < 2) {
        this_thread::thread().cpu_affinity(1u << w->id);
        if (port_cpu_id() != w->id) this_thread::yield();
    }
    if (!g_published) w->unpublished = 1;
    __asm__ volatile("dmb" : : : "memory");

    std::uint32_t seed = 0x1234567u * (w->id + 1);
    for (unsigned i = 0; i < kIters; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const std::uint32_t a = seed, b = seed >> 7;
        if (call(g_mod.compute, a, b) != expect_compute(a, b)) ++w->errors;
        if (call(g_mod.fmix, a, b) != expect_fmix(a, b)) ++w->errors;
#if defined(TEST_BIG)
        if (call(g_mod.walk, a, 512) != expect_walk(a, 512)) ++w->errors;
        w->errors = w->errors + call(g_mod.rw, w->id, a);
#endif
        call(g_mod.note);
        w->cores_seen = w->cores_seen | (1u << (port_cpu_id() & 1u));
        if ((i & 0x3FFu) == 0) this_thread::yield();
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

/* Results go to the UART and over semihosting, which the runner reads. */
void say(const char* s) {
    uart::write(s);
    if (!uos_debug_host_attached()) return;
    register unsigned r0 __asm__("r0") = 0x04u;  // SYS_WRITE0
    register const char* r1 __asm__("r1") = s;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
}
void sayf(const char* fmt, unsigned a = 0, unsigned b = 0, unsigned c = 0, unsigned d = 0) {
    char l[120];
    const int n = std::snprintf(l, sizeof l, fmt, a, b, c, d);
    (void)n;
    say(l);
}

}  // namespace

int os_main(int, char*[]) {
    this_thread::thread().cpu_affinity(1u << 0);
    led::init();
    say("\n" TEST_NAME ": shared-library load at boot, then SMP\n");

    bool ok = true;
    auto check = [&](bool c, const char* what) {
        say(c ? "  ok    " : "  FAIL: ");
        say(what);
        say("\n");
        if (!c) ok = false;
    };
    if (g_boot_error) { say("  boot_load: "); say(g_boot_error); say("\n"); }
    check(g_boot_error == nullptr, "module loaded at boot");
    check(g_boot_cpu == 0, "load ran on core 0");
    if (!ok) { say("RESULT: FAIL\n"); hw_result::fail(); }

    sayf("  module text at 0x%08x, r10=0x%08x, boot locks=%u depth=%u\n", g_text_addr, g_mod.got,
         g_locks, g_lock_max);
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
    for (unsigned t = 0; t < 2400 && done < kWorkers; ++t) {
        sysclock.sleep_for(100);
        done = 0;
        for (auto& w : g_workers) done += w.done;
    }

    unsigned errors = 0, unpublished = 0;
    for (auto& w : g_workers) { errors += w.errors; unpublished += w.unpublished; }
    const std::uint32_t total = kWorkers * kIters, shared = call(g_mod.shared);
    const std::uint32_t c0 = call(g_mod.core_count, 0), c1 = call(g_mod.core_count, 1);
    sayf("  workers done=%u errors=%u shared=%u of %u\n", done, errors, shared, total);
    sayf("  calls on core0=%u core1=%u; free threads saw cores 0x%x 0x%x\n", c0, c1,
         g_workers[2].cores_seen, g_workers[3].cores_seen);
    sayf("  loader locks=%u after publish=%u off core 0=%u; cleans=%u\n", g_locks,
         g_locks_after_publish, g_locks_off_core0, g_cleans);
    sayf("  invalidates=%u max lock depth=%u\n", g_invalidates, g_lock_max);

    check(done == kWorkers, "all workers finished");
    check(unpublished == 0, "every worker saw the published module");
    check(errors == 0, "module returned correct values on both cores");
    check(shared == total, "no shared increment was lost");
    check(c0 + c1 == total, "per-core counts add up");
    check(c0 >= kIters && c1 >= kIters, "both cores called the module");
    check(g_workers[0].cores_seen == 1u && g_workers[1].cores_seen == 2u, "pinned workers stayed on their core");
    check(boot_locks > 0, "the load took the loader lock");
    check(g_locks_off_core0 == 0, "no loader lock was taken off core 0");
    check(g_locks_after_publish == 0, "no loader code ran after publication");
    check(g_cleans > 0 && g_invalidates > 0, "the loader made its cache calls");

    say(ok ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    if (ok) hw_result::ok();
    hw_result::fail();
    return 0;
}
