/**
 * @file main.cpp — simple XIP dynamic (nolib) demo.
 *
 * Provision the flat-fs, XIP-load a few split PIC .so fixtures (R-X executes
 * from flash, RW copied to RAM), call exports via pic_call (r10), read globals,
 * unload. No test framework.
 */
#include "test-helpers.hpp"
#include "firmware/semihost-file.hpp"
#include "image-memory.hpp"
#include "dynamic-nolib/dl.hpp"
#include "xip/segment-map.hpp"
#include "xip/relocate.hpp"
#include <cstdio>
#include <cstring>

#ifndef DL_LIB_DIR
#define DL_LIB_DIR "."
#endif

/* ---- ABI the fixtures import from main (exported via -pie -Wl,-E) -------- */
struct FmInfo { std::uint32_t id, flags; };
extern "C" {
unsigned int AA = 42, BB = 84;
char*        s1 = const_cast<char*>("main-s1");
unsigned int f(unsigned int x) { return x + 10; }
unsigned int w(unsigned int x) { return x + 20; }
void trace_event(const char* o, const char* f, std::uint32_t a) {
    trace_printf("[trace] %s: %s(%u)\n", o, f, a); }
void trace_u32(const char* l, std::uint32_t v) { trace_printf("[trace] %s%u\n", l, v); }
std::uint32_t fm_var = 123;
struct FmInfo   fm_info = {0x00F1, 0x0007};
std::uint32_t fm_callback(std::uint32_t x) { return x + 1000; }
}
#include "link-anchor.hpp"   // anchors the borrowed libc/libsupc++/libm set

namespace {

bool can_xip(std::span<const std::byte> image) noexcept {
    xip::SegmentMap seg;
    return xip::classify_segments(image, seg) && seg.split &&
           xip::text_is_immutable(image, seg);
}

void provision(const char* name) {
    char path[160];
    std::size_t i = 0;
    for (const char* d = DL_LIB_DIR; *d && i + 1 < sizeof(path); ++d) path[i++] = *d;
    if (i + 1 < sizeof(path)) path[i++] = '/';
    for (const char* n = name; *n && i + 1 < sizeof(path); ++n) path[i++] = *n;
    path[i] = 0;

    firmware::semihost_file src(path);
    if (!src.is_open()) { trace_printf("  [prov] absent: %s\n", name); return; }
    const std::size_t sz = src.size();
    auto memp = fw::image_arena().alloc(sz);
    if (!memp) { trace_printf("  [prov] OOM: %s\n", name); return; }
    auto* buf = static_cast<std::byte*>(*memp);
    bool ok = false;
    if (src.read_at(0, std::span<std::byte>{buf, sz}).has_value()) {
        if (can_xip(std::span<const std::byte>{buf, sz}))
            xip::nolib::patch_main_imports(std::span<std::byte>{buf, sz});
        ok = fw::fs_add(name, buf, static_cast<std::uint32_t>(sz)).has_value();
    }
    fw::image_arena().free(buf);
    trace_printf("  [prov] %s %s\n", name, ok ? "ok" : "FAILED");
}

void test_one(const char* so, const char* sym, unsigned arg) {
    void* h = xopen(so);
    if (!h) { trace_printf("  !%s dlopen FAILED\n", so); return; }
    const std::uint32_t g = xip::nolib::get_got_base(h);
    trace_printf("  %s: %s %s(%u) = %u\n", so,
                 xip::nolib::is_xip(h) ? "XIP" : "copy-whole", sym, arg,
                 C1(h, g, sym, arg));
    xip::nolib::dlclose(h);
}

}  // namespace

int main(void) {
    trace_printf("scl-fw-xip-nolib1: XIP dynamic (nolib) — simple\n");

    fw::fs_format();
    provision("mylib1.so");
    provision("mylib2.so");

    /* mylib1: globals (ctor aa=20, bb=40) + functions */
    void* h = xopen("mylib1.so");
    if (h) {
        const std::uint32_t g = xip::nolib::get_got_base(h);
        trace_printf("  mylib1 %s: aa=%u bb=%u\n",
                     xip::nolib::is_xip(h) ? "XIP" : "copy-whole",
                     RD(h, "aa"), RD(h, "bb"));
        trace_printf("  mylib1 library_function(39) = %u\n", C1(h, g, "library_function", 39));
        trace_printf("  mylib1 library_function2(10) = %u\n", C1(h, g, "library_function2", 10));
        trace_printf("  mylib1 retstr() = \"%s\"\n", CSTR(h, g, "retstr"));
        xip::nolib::dlclose(h);
    }

    /* mylib2 */
    test_one("mylib2.so", "lf", 7);

    trace_printf("RESULT: done\n");
    for (;;) { }
}
