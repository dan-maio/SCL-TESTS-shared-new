/**
 * @file main.cpp — simple scl::reloc (ET_REL) loader demo.
 *
 * Load ET_REL modules (built -mlong-calls -fno-pic -r), resolve their imports
 * against main (and across a loaded set), run their constructors, call exported
 * functions. No GOT, no PLT, no r10 — a resolved export is a plain function
 * pointer. Plain C-style code; no test framework.
 */
#include "board.h"
#include "semihost.h"
#include "reloc/reloc-loader.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

/* ---- main's exported ABI (what the modules import) ----------------------- */
extern "C" {
void trace_event(const char* o, const char* f, std::uint32_t a) {
    trace_puts("["); trace_puts(o); trace_puts("] "); trace_puts(f); trace_puts("()\n");
}
void trace_u32(const char* l, std::uint32_t v) {
    trace_puts(l);
    char b[10]; int i = 0;
    if (!v) trace_putc('0'); else { while (v) { b[i++] = char('0' + v % 10); v /= 10; } while (i) trace_putc(b[--i]); }
    trace_putc('\n');
}
std::uint32_t          fm_var  = 170u;
struct FmInfo { std::uint32_t id, flags; } fm_info = { 0xF1u, 7u };
std::uint32_t fm_callback(std::uint32_t x) { return x + 0x100u; }

std::uint32_t AA = 5, BB = 55;
const char* s1 = "Help....\n";
std::uint32_t f(std::uint32_t x) { return 3 * x; }
std::uint32_t w(std::uint32_t x) { return AA + x; }

/* provided by crt.cpp (static heap) — declared here for the resolver */
void* malloc(std::size_t n);
void  free(void* p);
std::size_t strlen(const char* s);   /* libc-shim.c */
/* libm's logf/sqrtf set errno on domain errors; -nostdlib has no newlib */
int* __errno(void) { static int e; return &e; }

/* math the fixtures borrow from libm (linked into this firmware) */
float sinf(float), cosf(float), logf(float), sqrtf(float);

/* minimal printf for the fixtures (%u / %s) */
int printf(const char* fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    trace_puts("[printf] ");
    while (*fmt) {
        if (*fmt == '%' && (fmt[1] == 'u' || fmt[1] == 's')) {
            fmt++;
            if (*fmt == 'u') { unsigned v = __builtin_va_arg(ap, unsigned); trace_u32("", v); }
            else             trace_puts(__builtin_va_arg(ap, const char*));
        } else trace_putc(*fmt);
        fmt++;
    }
    __builtin_va_end(ap);
    return 0;
}
char* strcat(char* d, const char* s) { char* r = d; while (*d) d++; while ((*d++ = *s++)); return r; } /* strlen: provided by libc-shim.c */
double log(double x) { return static_cast<double>(logf(static_cast<float>(x))); }
float __aeabi_d2f(double d); double __aeabi_f2d(float f);   /* libgcc soft-double */
}

/* ---- the resolver: undefined module symbols -> main ---------------------- */
static std::uint32_t addr(void* p) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(p)); }
template <class F> static std::uint32_t addr(F fn) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(fn)); }

static loader::result<std::uint32_t> main_resolve(void*, std::string_view n) {
    if (n == "trace_event")  return addr((void*)&trace_event);
    if (n == "trace_u32")    return addr((void*)&trace_u32);
    if (n == "fm_callback")  return addr((void*)&fm_callback);
    if (n == "fm_var")       return addr(&fm_var);
    if (n == "fm_info")      return addr(&fm_info);
    if (n == "sinf")         return addr((void*)&sinf);
    if (n == "cosf")         return addr((void*)&cosf);
    if (n == "logf")         return addr((void*)&logf);
    if (n == "sqrtf")        return addr((void*)&sqrtf);
    if (n == "malloc")       return addr((void*)&malloc);
    if (n == "free")         return addr((void*)&free);
    if (n == "AA")           return addr(&AA);
    if (n == "BB")           return addr(&BB);
    if (n == "s1")           return addr(&s1);
    if (n == "f")            return addr((void*)&f);
    if (n == "w")            return addr((void*)&w);
    if (n == "printf")       return addr((void*)&printf);
    if (n == "strcat")       return addr((void*)&strcat);
    if (n == "strlen")       return addr((void*)&strlen);
    if (n == "log")          return addr((void*)&log);
    if (n == "__aeabi_d2f")   return addr((void*)&__aeabi_d2f);
    if (n == "__aeabi_f2d")   return addr((void*)&__aeabi_f2d);
    if (n == "_Znwj")   return addr(static_cast<void* (*)(std::size_t)>(::operator new));
    if (n == "_ZdlPvj") return addr(static_cast<void (*)(void*, std::size_t)>(::operator delete));
    return loader::unexpected(loader::error::SYMBOL_NOT_FOUND);
}

/* ---- helpers ------------------------------------------------------------- */
#ifndef RELOC_LIB_DIR
#define RELOC_LIB_DIR "."
#endif

static std::size_t read_file(const char* path, std::byte* buf, std::size_t cap) {
    int h = semihost_open(path, 1);
    if (h < 0) return 0;
    long n = semihost_flen(h);
    std::size_t sz = 0;
    if (n > 0 && static_cast<std::size_t>(n) <= cap && semihost_read(h, buf, static_cast<int>(n)) == 0)
        sz = static_cast<std::size_t>(n);
    semihost_close(h);
    return sz;
}

// One reusable image+arena slot: groups are loaded, exercised, then overwritten
// by the next group (a load_set needs up to 3 module images at once).
alignas(8) static std::byte g_img[3][8 * 1024];
alignas(8) static std::byte g_arena[16 * 1024];
static std::span<const std::byte> g_spans[3];

// Read RELOC_LIB_DIR/<name>.rel.o into the @p i-th image buffer.
static std::size_t read_mod(const char* name, int i) {
    char path[128]; int k = 0;
    for (const char* p = RELOC_LIB_DIR "/"; *p; ++p) path[k++] = *p;
    for (const char* p = name; *p; ++p) path[k++] = *p;
    for (const char* p = ".rel.o"; *p; ++p) path[k++] = *p;
    path[k] = 0;
    return read_file(path, g_img[i], sizeof g_img[i]);
}

// Load ONE module.
static reloc::module load_one(const char* name) {
    reloc::module m;
    std::size_t sz = read_mod(name, 0);
    if (!sz) { trace_puts("  read FAILED\n"); return m; }
    loader::symbol_source rs{ main_resolve, nullptr };
    auto r = reloc::load(std::span<const std::byte>(g_img[0], sz), std::span(g_arena), rs);
    if (!r) { trace_u32("  load FAILED: ", static_cast<std::uint32_t>(r.err())); return m; }
    return *r;
}

// Load a SET of modules (cross-module / cyclic) as one link unit.
static reloc::module_set load_set(const char* const* names, int n) {
    reloc::module_set set;
    for (int i = 0; i < n; ++i) {
        std::size_t sz = read_mod(names[i], i);
        if (!sz) { trace_puts("  read FAILED\n"); return set; }
        g_spans[i] = std::span<const std::byte>(g_img[i], sz);
    }
    loader::symbol_source rs{ main_resolve, nullptr };
    auto r = reloc::load_set(std::span<const std::span<const std::byte>>(g_spans, n), std::span(g_arena), rs);
    if (!r) { trace_u32("  load_set FAILED: ", static_cast<std::uint32_t>(r.err())); return set; }
    return *r;
}

template <class F> static F sym(const reloc::module& m, const char* name) {
    return reinterpret_cast<F>(static_cast<std::uintptr_t>(reloc::sym(m, name)));
}
template <class F> static F sym(const reloc::module_set& s, const char* name) {
    return reinterpret_cast<F>(static_cast<std::uintptr_t>(reloc::sym(s, name)));
}

using u32_1 = std::uint32_t (*)(std::uint32_t);
using u32_2 = std::uint32_t (*)(std::uint32_t, std::uint32_t);

int main() {
    board_init();
    trace_puts("scl-fw-reloc: ET_REL loader — simple demo\n");

    /* ---- relprov + relmod (relmod calls prov_add in relprov) ---- */
    static const char* pr[2] = { "relprov", "relmod" };
    reloc::module_set s_pr = load_set(pr, 2);
    if (s_pr) {
        printf("  prov_add(10,20) = %u\n", sym<u32_2>(s_pr, "prov_add")(10, 20));
        printf("  prov_state()    = %u\n", sym<u32_1>(s_pr, "prov_state")(0));
        printf("  rel_compute(7)  = %u\n", sym<u32_1>(s_pr, "rel_compute")(7));
    }

    /* ---- mylib1 / mylib2 ---- */
    reloc::module m1 = load_one("mylib1");
    if (m1) {
        printf("  mylib1 library_function(42) = %u\n", sym<u32_1>(m1, "library_function")(42));
        const char* (*rs)() = sym<const char* (*)()>(m1, "retstr");
        printf("  mylib1 retstr() = \"%s\"\n", rs ? rs() : "?");
    }
    reloc::module m2 = load_one("mylib2");
    if (m2)
        printf("  mylib2 lf(7) = %u\n", sym<u32_1>(m2, "lf")(7));

    /* ---- libprov + libcons (libcons calls prov_value in libprov) ---- */
    static const char* pc[2] = { "libprov", "libcons" };
    reloc::module_set s_pc = load_set(pc, 2);
    if (s_pc)
        printf("  libcons cons_call(5) = %u\n", sym<u32_1>(s_pc, "cons_call")(5));

    /* ---- c1/c2/cpp3 (cyclic cross-module cluster) ---- */
    static const char* ccc[3] = { "cpp3", "c2", "c1" };
    reloc::module_set s_ccc = load_set(ccc, 3);
    if (s_ccc) {
        printf("  cpp3_method(7,5) = %u\n", sym<u32_2>(s_ccc, "cpp3_method")(7, 5));
        printf("  c1_call_c2(7)    = %u\n", sym<u32_1>(s_ccc, "c1_call_c2")(7));
        printf("  c2_add(10,20)    = %u\n", sym<u32_2>(s_ccc, "c2_add")(10, 20));
    }

    trace_puts("RESULT: done\n");
    semihost_exit(0);
    for (;;) { }
}
