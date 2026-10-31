/**
 * @file overlay-main.cpp
 * @brief The WeAct overlay fixtures (ov1, ov2, ovcpp) on the dual-core RP2350.
 *
 * The modules are linked with the firmware and share one window, so swapping the
 * resident module IS the load. Under the rule of this suite every swap happens on
 * core 0, at boot. One module then stays resident for good and both cores call it.
 */
#include <cmsis-plus/rtos/os.h>

#include <exception_handler.hpp>
#include <led.hpp>
#include <smp.hpp>
#include <test-smp-boot.hpp>
#include <uart.hpp>

#include <hw_result.hpp>

#include "overlay/overlay.h"
#include "scl/cache.h"
#include "scl/os-hooks.h"

#include <cstdint>
#include <cstdio>

using namespace os::rtos;
using u32 = std::uint32_t;

extern "C" {
unsigned port_cpu_id(void);
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

namespace {
unsigned semi(unsigned op, const void* arg) {
    register unsigned r0 __asm__("r0") = op;
    register const void* r1 __asm__("r1") = arg;
    __asm__ volatile("svc 0x123456" : "+r"(r0) : "r"(r1) : "memory", "lr");
    return r0;
}
/* SYS_WRITE, not SYS_WRITE0: OpenOCD reads a string one byte at a time, which takes about a
 * second per line on a Cortex-A over SWD, but it reads a counted buffer as one block. */
void say(const char* s) {
    uart::uart1.puts_uart(s);
    static int tt = -1;
    const u32 n = static_cast<u32>(std::strlen(s));
    if (tt < 0) {
        u32 open_args[3] = {reinterpret_cast<u32>(":tt"), 4u, 3u};
        tt = static_cast<int>(semi(0x01u, open_args));
    }
    u32 write_args[3] = {static_cast<u32>(tt), reinterpret_cast<u32>(s), n};
    semi(0x05u, write_args);
}
void sayf(const char* fmt, unsigned a = 0, unsigned b = 0, unsigned c = 0, unsigned d = 0) {
    char l[120];
    std::snprintf(l, sizeof l, fmt, a, b, c, d);
    say(l);
}

inline u32 irq_off() {
    u32 cpsr;
    __asm__ volatile("mrs %0, cpsr\n cpsid i" : "=r"(cpsr) : : "memory");
    return cpsr;
}
inline void irq_restore(u32 cpsr) {
    if (!(cpsr & 0x80u)) __asm__ volatile("cpsie i" : : : "memory");
}
volatile u32 kAppLock, kLoaderLock;
inline void spin_take(volatile u32& l) {
    while (__atomic_exchange_n(&l, 1u, __ATOMIC_ACQUIRE) != 0u) {
    }
}
inline void spin_give(volatile u32& l) { __atomic_store_n(&l, 0u, __ATOMIC_RELEASE); }
u32 g_app_pm[OS_NCPU];
}  // namespace

// ---- what the overlays import from main; they are linked to these directly -------
extern "C" {
void app_lock(void) {
    const u32 pm = irq_off();
    spin_take(kAppLock);
    g_app_pm[cpu_slot()] = pm;
}
void app_unlock(void) {
    const u32 pm = g_app_pm[cpu_slot()];
    spin_give(kAppLock);
    irq_restore(pm);
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

/* Code is written as data, so clean it to the point of unification and drop stale instructions.
 * Inner-shareable: the other cores' caches are covered. */
namespace {
void clean_to_pou(const void* addr, std::size_t len) {
    constexpr u32 kLine = 64u;
    const u32 end = reinterpret_cast<u32>(addr) + len;
    for (u32 a = reinterpret_cast<u32>(addr) & ~(kLine - 1u); a < end; a += kLine)
        __asm__ volatile("mcr p15, 0, %0, c7, c11, 1" : : "r"(a) : "memory");   // DCCMVAU
    __asm__ volatile("dsb ish" : : : "memory");
}
void invalidate_icache() {
    __asm__ volatile("mcr p15, 0, %0, c7, c1, 0\n mcr p15, 0, %0, c7, c1, 6\n dsb ish\n isb"
                     : : "r"(0u) : "memory");   // ICIALLUIS, BPIALLIS
}
}  // namespace
void scl_cache_clean(const void* addr, size_t len) { ++g_cleans; clean_to_pou(addr, len); }
void scl_cache_invalidate(const void* addr, size_t len) { ++g_invalidates; clean_to_pou(addr, len); invalidate_icache(); }
void scl_log_puts(const char* s) { uart::uart1.puts(s); }
void scl_log_putc(char c) { const char l[2] = {c, 0}; uart::uart1.puts(l); }
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

namespace {

void walkthrough(unsigned core) {
    sayf("\n--- fixture calls on core %u ---\n", core);
    g_trace = 1;
    show_value("ovcpp_method(7,5)", ovcpp_method(7, 5));
    show_value("ovcpp_fcompute(7,5)", ovcpp_fcompute(7, 5));
    show_value("ovcpp_heap(8)", ovcpp_heap(8));
    g_trace = 0;
}

constexpr unsigned kPinned = OS_NCPU, kWorkers = kPinned + 1, kIters = TEST_ITERS;
struct Worker {
    unsigned id;
    volatile unsigned errors, done, cores_seen, unpublished, first_bad;
} g_workers[kWorkers];
thread::stack::element_t g_stacks[kWorkers][4096];
volatile unsigned g_walked;

void* worker(void* arg) {
    auto* w = static_cast<Worker*>(arg);
    if (w->id < kPinned) {
        this_thread::thread().cpu_affinity(1u << w->id);
        if (port_cpu_id() != w->id) this_thread::yield();
    }
    if (!g_published) w->unpublished = 1;
    __asm__ volatile("dmb" : : : "memory");
    if (w->id < kPinned) {
        while (g_walked != w->id) sysclock.sleep_for(5);
        walkthrough(w->id);
        if (w->id == kPinned - 1u) say("\n--- both cores now call ovcpp together (traces off) ---\n");
        __asm__ volatile("dmb" : : : "memory");
        g_walked = w->id + 1;
    }
    while (g_walked < kPinned) sysclock.sleep_for(5);

    auto expect = [&](bool ok, unsigned code) {
        if (!ok) { ++w->errors; if (!w->first_bad) w->first_bad = code; }
    };
    for (unsigned i = 0; i < kIters; ++i) {
        const unsigned k = i % kGold;
        expect(ovcpp_method(7, k * 5u + 1u) == g_gold_method[k], 1);
        expect(ovcpp_fcompute(7, k * 5u + 1u) == g_gold_f[k], 2);
        // ovcpp_heap calls malloc directly: the kernel's is safe across cores, and it would turn
        // interrupts back on under app_lock.
        const u32 h = ovcpp_heap(8);
        expect(h == g_gold_heap, 3);
        w->cores_seen = w->cores_seen | (1u << cpu_slot());
        if ((i & 0x7Fu) == 0) this_thread::yield();
    }
    w->done = 1;
    return nullptr;
}

}  // namespace

int os_main(int, char*[]) {
    (void)os::rtos::interrupts::uncritical_section::enter();
    this_thread::thread().cpu_affinity(1u << 0);
    led::init();
    say("\n" TEST_NAME ": overlay fixtures swapped at boot on the RK3506, then SMP\n");

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

    smp_install_boot_threads();
    smp::start_secondary_cores();
    check(test_wait_secondaries(3000) >= 0 && test_secondaries_joined(), "every core joined");
    static thread* threads[kWorkers];
    for (unsigned i = 0; i < kWorkers; ++i) {
        g_workers[i].id = i;
        thread::attributes attr = thread::initializer;
        attr.th_stack_address = g_stacks[i];
        attr.th_stack_size_bytes = sizeof g_stacks[i];
        char name[8];
        std::snprintf(name, sizeof name, "wrk%u", i);
        threads[i] = new thread(name, worker, &g_workers[i], attr);
        if (i < kPinned) threads[i]->cpu_affinity(1u << i);
    }

    say("  all cores running ovcpp now\n");

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
    sayf("  the free thread saw cores 0x%x\n", g_workers[kPinned].cores_seen);
    sayf("  loader locks=%u (after publish=%u); cleans=%u invalidates=%u\n", g_locks, g_locks_after_publish,
         g_cleans, g_invalidates);

    check(done == kWorkers, "all workers finished");
    check(unpublished == 0, "every worker saw the published overlay");
    check(errors == 0, "ovcpp returned correct values on both cores");
    {
        bool stayed = true;
        for (unsigned c = 0; c < kPinned; ++c) stayed = stayed && g_workers[c].cores_seen == (1u << c);
        check(stayed, "pinned workers stayed on their core");
    }
    check(g_locks > 0, "the swaps took the loader lock");
    check(g_locks_after_publish == 0, "no swap happened after publication");
    check(g_cleans > 0 && g_invalidates > 0, "the swaps made their cache calls");
    say(ok ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    if (ok) hw_result::ok();
    hw_result::fail();
    return 0;
}

// ---- start-up: load at boot, on core 0, before the kernel starts ---------------------------
extern "C" {
extern char __heap_start[], __heap_end[], __fiq_stack_top[], __irq_stack_top[];
void os_startup_initialize_hardware_early(void) {}
void os_startup_initialize_hardware(void) {
    uart::uart1.init();
    os_startup_initialize_free_store(__heap_start, static_cast<std::size_t>(__heap_end - __heap_start));
    exception::init();
}
[[noreturn]] static void main_trampoline(void) { std::exit(os_main(0, nullptr)); }
extern void os_startup_create_thread_idle(void);
extern os::rtos::thread* os_main_thread;
int main(int, char*[]) {
    boot_load();   // every static constructor has run; the other cores are still parked
#if defined(OS_HAS_INTERRUPTS_STACK)
    interrupts::stack()->set(reinterpret_cast<thread::stack::element_t*>(__fiq_stack_top), __irq_stack_top - __fiq_stack_top);
    interrupts::stack()->initialize();
#endif
    scheduler::initialize();
    static thread::stack::element_t main_stack[8192];
    thread::attributes attr = thread::initializer;
    attr.th_stack_address = main_stack;
    attr.th_stack_size_bytes = sizeof main_stack;
    static thread main_thread{"main", reinterpret_cast<thread::func_t>(main_trampoline), nullptr, attr};
    os_main_thread = &main_thread;
    os_startup_create_thread_idle();
    scheduler::start();
    return 0;
}
}

// ---- newlib's file calls: scl::static's log pulls them in, and nothing here uses files -------
extern "C" {
__attribute__((weak)) int _close(int) { return -1; }
__attribute__((weak)) int _fstat(int, void*) { return -1; }
__attribute__((weak)) int _isatty(int) { return 0; }
__attribute__((weak)) int _lseek(int, int, int) { return -1; }
__attribute__((weak)) int _read(int, void*, unsigned) { return -1; }
__attribute__((weak)) int _write(int, const void*, unsigned) { return -1; }
}
