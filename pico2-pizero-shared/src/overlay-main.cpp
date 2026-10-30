/**
 * @file overlay-main.cpp
 * @brief The WeAct overlay fixtures (ov1, ov2, ovcpp) on the dual-core RP2350.
 *
 * The modules are linked with the firmware and share one window, so swapping the
 * resident module IS the load. Under the rule of this suite every swap happens on
 * core 0, at boot. One module then stays resident for good and both cores call it.
 */
#include <cmsis-plus/rtos/os.h>

#include <bsp/led.hpp>
#include <bsp/multicore.hpp>
#include <bsp/uart.hpp>
#if defined(TEST_PSRAM)
#include <bsp/psram.hpp>
#endif

#include <hw_result.hpp>

#include "overlay/overlay.h"
#include "scl/cache.h"
#include "scl/os-hooks.h"

#include <cstdint>
#include <cstdio>

using namespace os::rtos;
using u32 = std::uint32_t;

extern "C" {
void port_smp_secondary_start(void);
unsigned port_core1_stack_top(void);
unsigned port_cpu_id(void);
void port_smp_core0_ipi_enable(void);
extern std::uint8_t _ovl_vma[], _ov1_lma[], _ov1_size[], _ov2_lma[], _ov2_size[], _ovcpp_lma[], _ovcpp_size[];
__attribute__((long_call)) u32 ov1_use_fm(void);
__attribute__((long_call)) u32 ov1_count(void);
__attribute__((long_call)) u32 ov2_sum(void);
__attribute__((long_call)) u32 ovcpp_method(u32 seed, u32 x);
__attribute__((long_call)) u32 ovcpp_fcompute(u32 seed, u32 x);
__attribute__((long_call)) u32 ovcpp_heap(u32 n);
extern u32 ov2_data;
extern const u32 ov2_const;
extern const u32 ovcpp_magic;
}

namespace os::rtos::scheduler {
extern thread* os_idle_thread_core[OS_NCPU];
}

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

constexpr u32 kAppLock = 0xD0000100u + 30u * 4u;
u32 g_app_pm[2];
}  // namespace

// ---- what the overlays import from main; they are linked to these directly -------
extern "C" {
void app_lock(void) {
    u32 pm;
    __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(pm) : : "memory");
    while (*reinterpret_cast<volatile u32*>(kAppLock) == 0) {
    }
    __asm__ volatile("dmb" : : : "memory");
    g_app_pm[port_cpu_id() & 1u] = pm;
}
void app_unlock(void) {
    const u32 pm = g_app_pm[port_cpu_id() & 1u];
    __asm__ volatile("dmb" : : : "memory");
    *reinterpret_cast<volatile u32*>(kAppLock) = 1;
    __asm__ volatile("msr primask, %0" : : "r"(pm) : "memory");
}

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
u32 fm_var = 123;
struct FmInfo { u32 id, flags; };
extern const FmInfo fm_info;
const FmInfo fm_info = {0x00F1, 0x0007};

volatile unsigned g_published, g_locks, g_locks_after_publish, g_lock_depth, g_lock_max;
volatile unsigned g_cleans, g_invalidates;
void scl_loader_lock(void) {
    ++g_locks;
    if (g_published) ++g_locks_after_publish;
    if (++g_lock_depth > g_lock_max) g_lock_max = g_lock_depth;
}
void scl_loader_unlock(void) { --g_lock_depth; }

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

scl_overlay_module g_mods[3];
scl_overlay_region g_ovl;
const char* g_boot_error = "boot_load did not run";
unsigned g_boot_cpu = 99;
constexpr unsigned kGold = 16;
u32 g_gold_method[kGold], g_gold_f[kGold], g_gold_heap;

void show_value(const char* what, u32 v) {
    say("  ");
    say(what);
    sayf(" = %u\n", static_cast<unsigned>(v));
}

bool load(int i) {
    sayf("\n--- core %u: swap overlay %u into the window ---\n", port_cpu_id(), static_cast<unsigned>(i));
    return scl_overlay_load(&g_ovl, i) == 0;
}

void boot_load() {
    g_boot_cpu = port_cpu_id();
#if defined(TEST_PSRAM)
    if (!psram::init()) { g_boot_error = "psram::init"; return; }
#endif
    g_mods[0] = {_ovl_vma, _ov1_lma, reinterpret_cast<u32>(_ov1_size), "ov1"};
    g_mods[1] = {_ovl_vma, _ov2_lma, reinterpret_cast<u32>(_ov2_size), "ov2"};
    g_mods[2] = {_ovl_vma, _ovcpp_lma, reinterpret_cast<u32>(_ovcpp_size), "ovcpp"};
    scl_overlay_init(&g_ovl, g_mods, 3);

    if (!load(0)) { g_boot_error = "load ov1"; return; }
    const u32 fm = ov1_use_fm();
    show_value("ov1_use_fm()", fm);
    show_value("ov1_count()", ov1_count());
    if (fm != 123u + 0xF1u + 7u + 0xC1u) { g_boot_error = "ov1_use_fm"; return; }

    if (!load(1)) { g_boot_error = "load ov2"; return; }
    const u32 sum = ov2_sum();
    show_value("ov2_sum()", sum);
    show_value("ov2_data", ov2_data);
    show_value("ov2_const", ov2_const);
    if (sum != 0x24C0u) { g_boot_error = "ov2_sum"; return; }

    /* Swapping back restores ov1's initial image: its counter must start over. */
    if (!load(0)) { g_boot_error = "reload ov1"; return; }
    const u32 count = ov1_count();
    show_value("ov1_count() after the swap back", count);
    if (count != 1u) { g_boot_error = "ov1 was not restored from flash"; return; }

    /* ovcpp stays resident: nothing swaps the window once SMP runs. */
    if (!load(2)) { g_boot_error = "load ovcpp"; return; }
    show_value("ovcpp_magic", ovcpp_magic);
    if (ovcpp_magic != 0x3333u) { g_boot_error = "ovcpp_magic"; return; }
    g_gold_heap = ovcpp_heap(8);
    if (g_gold_heap != 533u) { g_boot_error = "ovcpp_heap"; return; }
    g_trace = 0;
    for (unsigned i = 0; i < kGold; ++i) {
        g_gold_method[i] = ovcpp_method(7, i * 5u + 1u);
        g_gold_f[i] = ovcpp_fcompute(7, i * 5u + 1u);
    }
    g_boot_error = nullptr;
    scl_publish_release();
    g_published = 1;
}

}  // namespace

extern "C" int __real_main(int, char**);
extern "C" int __wrap_main(int argc, char** argv) {
    boot_load();
    return __real_main(argc, argv);
}

namespace {

void walkthrough(unsigned core) {
    sayf("\n--- fixture calls on core %u ---\n", core);
    g_trace = 1;
    show_value("ovcpp_method(7,5)", ovcpp_method(7, 5));
    show_value("ovcpp_fcompute(7,5)", ovcpp_fcompute(7, 5));
    show_value("ovcpp_heap(8)", ovcpp_heap(8));
    g_trace = 0;
}

constexpr unsigned kWorkers = 4, kIters = TEST_ITERS;
struct Worker {
    unsigned id;
    volatile unsigned errors, done, cores_seen, unpublished, first_bad;
} g_workers[kWorkers];
thread::stack::element_t g_stacks[kWorkers][2048];
volatile unsigned g_walked;

void* worker(void* arg) {
    auto* w = static_cast<Worker*>(arg);
    if (w->id < 2) {
        this_thread::thread().cpu_affinity(1u << w->id);
        if (port_cpu_id() != w->id) this_thread::yield();
    }
    if (!g_published) w->unpublished = 1;
    __asm__ volatile("dmb" : : : "memory");
    if (w->id < 2) {
        while (g_walked != w->id) sysclock.sleep_for(5);
        walkthrough(w->id);
        if (w->id == 1) say("\n--- both cores now call ovcpp together (traces off) ---\n");
        __asm__ volatile("dmb" : : : "memory");
        g_walked = w->id + 1;
    }
    while (g_walked < 2) sysclock.sleep_for(5);

    auto expect = [&](bool ok, unsigned code) {
        if (!ok) { ++w->errors; if (!w->first_bad) w->first_bad = code; }
    };
    for (unsigned i = 0; i < kIters; ++i) {
        const unsigned k = i % kGold;
        expect(ovcpp_method(7, k * 5u + 1u) == g_gold_method[k], 1);
        expect(ovcpp_fcompute(7, k * 5u + 1u) == g_gold_f[k], 2);
        // ovcpp_heap calls malloc directly, and newlib's is not safe across cores.
        app_lock();
        const u32 h = ovcpp_heap(8);
        app_unlock();
        expect(h == g_gold_heap, 3);
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
    say("\n" TEST_NAME ": WeAct overlay fixtures swapped at boot, then SMP\n");

    bool ok = true;
    auto check = [&](bool c, const char* what) {
        say(c ? "  ok    " : "  FAIL: ");
        say(what);
        say("\n");
        if (!c) ok = false;
    };
    if (g_boot_error) { say("  boot_load: "); say(g_boot_error); say("\n"); }
    check(g_boot_error == nullptr, "overlays swapped and checked at boot");
    check(g_boot_cpu == 0, "swaps ran on core 0");
    if (!ok) { say("RESULT: FAIL\n"); hw_result::fail(); }
    sayf("  window 0x%08x, resident overlay %d; boot locks=%u\n", reinterpret_cast<unsigned>(_ovl_vma),
         static_cast<unsigned>(scl_overlay_current(&g_ovl)), g_locks);

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
    sayf("  workers done=%u errors=%u first failing check=%u\n", done, errors, first_bad);
    sayf("  free threads saw cores 0x%x 0x%x\n", g_workers[2].cores_seen, g_workers[3].cores_seen);
    sayf("  loader locks=%u (after publish=%u); cleans=%u invalidates=%u\n", g_locks, g_locks_after_publish,
         g_cleans, g_invalidates);

    check(done == kWorkers, "all workers finished");
    check(unpublished == 0, "every worker saw the published overlay");
    check(errors == 0, "ovcpp returned correct values on both cores");
    check(g_workers[0].cores_seen == 1u && g_workers[1].cores_seen == 2u, "pinned workers stayed on their core");
    check(g_locks > 0, "the swaps took the loader lock");
    check(g_locks_after_publish == 0, "no swap happened after publication");
    check(g_cleans > 0 && g_invalidates > 0, "the swaps made their cache calls");
    say(ok ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    if (ok) hw_result::ok();
    hw_result::fail();
    return 0;
}
