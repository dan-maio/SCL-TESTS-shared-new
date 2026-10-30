/**
 * @file main.cpp — api-dynamic-xip-semihost-table: main's table built at boot.
 *
 * New APIs: scl::symtab::collect_buffered(), collect(), for_each() and the scl_sym_entry overload of
 * xip::newlib::set_symtab(). There is no gen-symtab step: main reads .symtab and
 * .strtab over semihosting from a copy of its ELF reduced to the imported
 * symbols (see CMakeLists.txt). The module imports app_gain().
 * Output on USART1 (PA9, 115200) + the semihosting console. WeAct F411CE.
 */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <span>
#include "uart.h"
#include "firmware/semihost-file.hpp"
#include "scl/image-memory.hpp"
#include "fs-archive.hpp"
#include "xip/segment-map.hpp"
#include "xip/relocate.hpp"
#include "xip/dynamic-newlib/adapter.hpp"
#include "scl/symtab-runtime.hpp"
#include "loader/elf-image.hpp"

#ifndef SCL_SO_DIR
#define SCL_SO_DIR "."
#endif

extern "C" __attribute__((used)) float app_gain(void) { return 1.5f; }

/* r10 = the module GOT base register; set it before an XIP call */
register void* r10_val asm("r10");

static void line(const char* s) { uart_puts(s); puts(s); }
static void fstr(float v, char* p) {
    if (v < 0) { *p++ = '-'; v = -v; }
    unsigned ip = (unsigned)v, fp = (unsigned)((v - (float)ip) * 1000.0f + 0.5f);
    char* q = p; int n = 0; unsigned t = ip; do { ++n; t /= 10; } while (t);
    q += n; *q-- = 0; t = ip; do { *q-- = char('0' + t % 10); t /= 10; } while (t);
    p += n; *p++ = '.'; *p++ = char('0' + fp/100%10); *p++ = char('0' + fp/10%10);
    *p++ = char('0' + fp%10); *p = 0;
}

/* --- the POSIX dlopen API, over the XIP dynamic loader ---------------------- */
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

/* Sized for the reduced ELF. g_strbuf must stay valid: the table points into it. */
static scl::symtab::entry g_tab[8];
alignas(4) static std::byte g_symbuf[SYMBUF_KB * 1024];
static char g_strbuf[STRBUF_KB * 1024];

static bool load_symtab() {
    firmware::semihost_file f(FIRMWARE_ELF);
    if (!f.is_open()) { line("FAIL: cannot open firmware ELF"); return false; }
    semihost_source src(f);
    auto img = loader::elf_image::parse(src);
    if (!img) { line("FAIL: firmware ELF parse"); return false; }
    const std::size_t n = scl::symtab::collect_buffered(
        *img, g_tab, 8, g_symbuf, sizeof g_symbuf, g_strbuf, sizeof g_strbuf);
    char l[64]; std::snprintf(l, sizeof l, "  collected %u symbols at boot", (unsigned)n); line(l);
    if (n == 0) { line("FAIL: collect_buffered"); return false; }

    /* The streaming paths must agree with the buffered one. */
    static scl::symtab::entry tab2[8];
    static char names[64];
    const std::size_t n2 = scl::symtab::collect(*img, tab2, 8, names, sizeof names);
    std::size_t visited = 0;
    scl::symtab::for_each(*img, [&](std::string_view, std::uint32_t) { ++visited; });
    if (n2 != n || visited != n || std::strcmp(tab2[0].name, g_tab[0].name) != 0 ||
        tab2[0].addr != g_tab[0].addr) {
        line("FAIL: collect/for_each disagree"); return false;
    }
    xip::newlib::set_symtab(g_tab, n);
    return true;
}
static bool can_xip(std::span<const std::byte> img) noexcept {
    xip::SegmentMap seg;
    return xip::classify_segments(img, seg) && seg.split && xip::text_is_immutable(img, seg);
}
/* dlopen(path): semihost-read the .so into the flat-fs, XIP-load it, return handle */
static void* dlopen(const char* path, int) {
    const char* name = std::strrchr(path, '/'); name = name ? name + 1 : path;
    firmware::semihost_file src(path);
    if (!src.is_open()) return nullptr;
    const std::size_t sz = src.size();
    auto memp = fw::image_arena().alloc(sz);
    if (!memp) return nullptr;
    auto* buf = static_cast<std::byte*>(*memp);
    void* h = nullptr;
    if (src.read_at(0, std::span<std::byte>{buf, sz}).has_value()) {
        if (can_xip(std::span<const std::byte>{buf, sz}))
            xip::newlib::patch_main_imports(std::span<std::byte>{buf, sz});   /* pre-bind PLT for XIP */
        if (fw::fs_add(name, buf, (std::uint32_t)sz).has_value()) {
            const fw::FsEntry* e = fw::fs_find(name);
            if (e) h = xip::newlib::dlopen(std::span<const std::byte>{ fw::fs_blob(*e), e->size }, name);
        }
    }
    fw::image_arena().free(buf);
    return h;
}
static void* dlsym(void* h, const char* n) { return xip::newlib::dlsym(h, n); }
static void  dlclose(void* h)              { xip::newlib::dlclose(h); }

/* an r10-aware float call: set r10 = the module's GOT base, then call by pointer */
static float call_f(void* h, const char* name, float a, float b) {
    void* fn = dlsym(h, name);
    if (!fn) return 0;
    r10_val = reinterpret_cast<void*>(xip::newlib::get_got_base(h));
    return reinterpret_cast<float(*)(float,float)>(fn)(a, b);
}

int main(void) {
    uart_init();
    line("api-dynamic-xip-semihost-table: collect_buffered + set_symtab (scl::xip-dynamic-newlib)");
    if (!load_symtab()) { line("RESULT: FAILURE"); for (;;) {} }
    fw::fs_format();

    void* h = dlopen(SCL_SO_DIR "/floatlib.so", 0);
    if (!h) { line("  dlopen failed"); line("RESULT: FAILURE"); for (;;) {} }
    line("  loaded XIP (code from flash)");

    char sa[16], sm[16], l[80];
    fstr(call_f(h, "float_add", 2.5f, 3.5f), sa);   /* 6.000 */
    fstr(call_f(h, "float_mul", 2.5f, 3.5f), sm);   /* 8.750 */
    std::snprintf(l, sizeof l, "  float_add(2.500, 3.500) = %s", sa); line(l);
    std::snprintf(l, sizeof l, "  float_mul(2.500, 3.500) = %s", sm); line(l);
    const float scaled = call_f(h, "float_scale", 2.0f, 3.0f);   /* 2 * 3 * app_gain() = 9 */
    fstr(scaled, sa);
    std::snprintf(l, sizeof l, "  float_scale(2.000, 3.000) = %s", sa); line(l);

    dlclose(h);
    const float d = scaled - 9.0f;
    if (d > 0.001f || d < -0.001f) { line("FAIL: values"); line("RESULT: FAILURE"); for (;;) {} }
    line("RESULT: SUCCESS");
    for (;;) { }
}
