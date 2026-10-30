/**
 * @file main.cpp  (test-xip)
 * @brief XIP test — modules are provisioned into on-chip flash (flat-fs) and
 *        loaded from there; then EVERY export of EVERY fixture is exercised by
 *        DIRECT, BY-NAME calls.
 *
 * The whole surface is right here in main: firmware ABI the fixtures import, the
 * exports declared with their real prototypes, a load-in-a-loop over the generated
 * module list, then variables / statics / constructors / structs / strings /
 * function pointers / weak symbols / C++ objects+methods / RTTI / hard-float /
 * heap — all called by name (a cast only for the two EH modules that can't be
 * -l:-linked). The RAM test (test-ram) is identical except for provide()/setup().
 */
#include "optimal/loader.hpp"
#include "firmware/semihost-file.hpp"
#include "fs-archive.hpp"
#include "scl/image-memory.hpp"
#include "diag/trace.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <span>

#ifndef DL_LIB_DIR
#define DL_LIB_DIR "."
#endif
#ifndef OPTIMAL_MODE
#define OPTIMAL_MODE "XIP"
#endif

// ---- The firmware surface the libraries import (their UND symbols) ----------
extern "C" {
void trace_event(const char* o, const char* f, std::uint32_t a) { trace_printf("[%s] %s(%u)\n", o, f, a); }
void trace_u32(const char* l, std::uint32_t v) { trace_printf("[trace] %s%u\n", l, v); }
std::uint32_t fm_var = 123;                       // NOT const: keep external linkage
struct FmInfo { std::uint32_t id; std::uint32_t flags; } fm_info = {0x00F1, 0x0007};
std::uint32_t fm_callback(std::uint32_t x) { return x + 1000; }
unsigned int HOST_K = 1000;
unsigned int host_f(unsigned int x) { return x * 2 + 1; }
unsigned int AA = 42, BB = 84;
char*        s1 = const_cast<char*>("main-s1");
unsigned int f(unsigned int x) { return x + 10; }
unsigned int w(unsigned int x) { return x + 20; }
}
#include "link-anchor.hpp"          // anchors borrowed libc/libm/libsupc++ into main's .dynsym

// ---- The library exports we call, declared with their real prototypes -------
struct Str_Lib { char* a; float f; char* bb; unsigned int b; unsigned int c; };
struct SInfo   { const char* tag; unsigned int x, y; };
typedef unsigned int (*ufn)(unsigned int);

extern "C" {
// slib — .data/.bss globals, ctor, string, struct, fn-pointer, host imports
unsigned int s_add(unsigned int, unsigned int);
unsigned int s_scale(unsigned int);
unsigned int s_via_helper(unsigned int);
unsigned int s_apply(unsigned int);
unsigned int s_use_host(unsigned int);
unsigned int s_mix(unsigned int);
unsigned int s_get_a(void);
unsigned int s_get_b(void);
const char*  s_text(void);
const char*  s_tag(void);
void         s_print(void);
extern unsigned int s_a, s_b;
extern const char* s_msg;
extern SInfo s_info;
// mylib1 — globals, statics, struct-by-value, strings, fn-pointer, weak symbols
unsigned int library_function(unsigned int);
unsigned int library_function2(unsigned int);
unsigned int library_function3(unsigned int);
unsigned int callback_f(unsigned int);
unsigned int g(unsigned int);
unsigned int gg(unsigned int);
unsigned int fff(unsigned int);
unsigned int ptrfunc(ufn, unsigned int);
char*        retstr(void);
char*        retstr_(void);
void         print_lib(void);
void         print_lib2(char*);
Str_Lib      ret_struct(Str_Lib);
Str_Lib      ret_struct_lib(void);
unsigned int foo(void);            // weak
unsigned int boo(void);            // weak
unsigned int boo2(void);           // weak
unsigned int lkj(void);
extern unsigned int aa, bb;
extern char *str, *str_;
extern Str_Lib s_lib;
// mylib2 — second load path, own globals, weak mm
unsigned int lf(unsigned int);
unsigned int lf2(unsigned int);
unsigned int lf3(unsigned int);
unsigned int xxx(void);
void         pl(void);
void         pl2(char*);
unsigned int mm(void);             // weak
extern unsigned int aaa, bbb, xx;
extern char* str2;
// libprov / libcons — cross-module
unsigned int prov_value(unsigned int);
unsigned int cons_call(unsigned int);
// libcppsh — C++ class, virtual method, instance counter (objects/methods)
void*        cppsh_make(unsigned int);
unsigned int cppsh_compute(void*, unsigned int);
unsigned int cppsh_instances(void);
void         cppsh_free(void*);
// cpp3 — C++ class method, global-object ctor, hard-float, heap, counters
unsigned int cpp3_method(unsigned int, unsigned int);
unsigned int cpp3_heap(unsigned int);
unsigned int cpp3_orchestrate(unsigned int);
unsigned int cpp3_step(unsigned int);
unsigned int cpp3_recurse(unsigned int);
unsigned int cpp3_float_cross(unsigned int);
unsigned int cpp3_calls(void);
unsigned int cpp3_made(void);
extern const unsigned int cpp3_magic;
extern unsigned int cpp3_var;
// c2 / c1 — static "class" struct + ctor, .data/.rodata/.bss, float, cyclic
unsigned int c2_add(unsigned int, unsigned int);
unsigned int c2_param(void);
unsigned int c2_chain(unsigned int);
unsigned int c2_relay(unsigned int);
unsigned int c2_step_down(unsigned int);
float        c2_scale(float);
unsigned int c2_state(void);
extern unsigned int c2_data, c2_bss;
extern const unsigned int c2_const;
extern char* c2_tag;
unsigned int c1_use_fm(void);
unsigned int c1_call_c2(unsigned int);
unsigned int c1_into_fm(unsigned int);
unsigned int c1_state(void);
extern unsigned int c1_data, c1_bss;
extern char* c1_tag;
}

namespace {

// The generated module list (basenames, load order) — no hard-coded names here.
//
// WITHOUT gen-imports: this project needs the tool for exactly two things, and
// both have a plain manual replacement —
//   (1) this load list. Drop the #include and write the array by hand instead:
//         const char* const kModules[] = {
//             "slib.so","mylib1.so","mylib2.so","libprov.so","libcons.so",
//             "libcppsh.so","libcppeh.so","ehx_catch.so","cpp3.so","c2.so","c1.so" };
//       (keep it in the same load order build.sh lists in fixtures/manifest.txt.)
//   (2) the export DECLARATIONS. This file already does that the manual way: the
//       `extern "C"` prototype block above IS the fixtures' API, written by hand
//       (or you would #include each library's own header). The generated
//       imports.generated.h is only the zero-header fallback and is NOT used here.
// Do those two and gen-imports — and its python/readelf needs — drop out entirely.
const char* const kModules[] = {
#include "modules.generated.inc"
};
constexpr int kNumModules = int(sizeof(kModules) / sizeof(kModules[0]));

optimal::Module            g_mods[kNumModules];
std::span<const std::byte> g_blobs[kNumModules];

// ---- XIP blob source: provision each .so into the on-chip flat-fs (flash) ----
void setup() {
    trace_printf("[xip] formatting on-chip flat-fs\n");
    if (!fw::fs_format()) trace_printf("[xip] fs_format FAILED\n");
}

std::span<const std::byte> provide(const char* name) {
    char path[160];
    std::snprintf(path, sizeof(path), "%s/%s", DL_LIB_DIR, name);
    firmware::semihost_file src(path);
    if (!src.is_open()) { trace_printf("  [xip] absent: %s\n", name); return {}; }
    const std::size_t sz = src.size();
    auto memp = fw::image_arena().alloc(sz);
    if (!memp) { trace_printf("  [xip] OOM: %s\n", name); return {}; }
    auto* buf = static_cast<std::byte*>(*memp);
    bool ok = src.read_at(0, std::span<std::byte>(buf, sz)).has_value() &&
              fw::fs_add(name, buf, static_cast<std::uint32_t>(sz)).has_value();
    fw::image_arena().free(buf);
    if (!ok) { trace_printf("  [xip] provision FAILED: %s\n", name); return {}; }
    const fw::FsEntry* e = fw::fs_find(name);      // hand back the FLASH-resident copy
    if (!e) return {};
    return std::span<const std::byte>(fw::fs_blob(*e), e->size);
}

}  // namespace

extern "C" int main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    trace_printf("\noptimal-shared [%s]: load-in-a-loop, exercise EVERY fixture (no r10, no dlsym)\n",
                 OPTIMAL_MODE);

    // ---- provision + load + link, all driven by the generated list -------------
    setup();
    for (int i = 0; i < kNumModules; ++i) g_blobs[i] = provide(kModules[i]);
    int loaded = optimal::load_and_link(g_blobs, kNumModules, g_mods);
    trace_printf("[boot] %d/%d modules loaded + linked, main GOT patched\n", loaded, kNumModules);

    // ===================== slib — globals, ctor, string, struct, fn-ptr, imports =====
    trace_printf("\n-- slib --\n");
    trace_printf("  vars: s_a=%u (ctor 10->15)  s_b=%u (ctor->30)  s_msg=\"%s\"\n", s_a, s_b, s_msg);
    trace_printf("  struct s_info: tag=\"%s\" x=%u y=%u\n", s_info.tag, s_info.x, s_info.y);
    trace_printf("  funcs: s_add(10,5)=%u  s_scale(3)=%u  s_via_helper(5)=%u  s_apply(3)=%u\n",
                 s_add(10, 5), s_scale(3), s_via_helper(5), s_apply(3));
    trace_printf("  s_text()=\"%s\"  s_tag()=\"%s\"  s_get_a()=%u  s_get_b()=%u\n",
                 s_text(), s_tag(), s_get_a(), s_get_b());
    trace_printf("  imports: s_use_host(5)=%u  s_mix(4)=%u  s_print(): ", s_use_host(5), s_mix(4)); s_print();

    // ===================== mylib1 — globals, statics, struct-by-value, strings, weak ===
    trace_printf("\n-- mylib1 --\n");
    trace_printf("  vars: aa=%u (ctor 10->20)  bb=%u  str=\"%s\"  str_=\"%s\"\n", aa, bb, str, str_);
    trace_printf("  funcs: library_function(39)=%u  library_function2(10)=%u (statics)  g(5)=%u  gg(5)=%u\n",
                 library_function(39), library_function2(10), g(5), gg(5));
    trace_printf("  fff(5)=%u  callback_f(10)=%u  ptrfunc(g,5)=%u (fn-pointer)\n",
                 fff(5), callback_f(10), ptrfunc(g, 5));
    trace_printf("  retstr()=\"%s\"  retstr_()=\"%s\"\n", retstr(), retstr_());
    Str_Lib in{const_cast<char*>("in"), 1.0f, const_cast<char*>("bb"), 5, 6};
    Str_Lib rs = ret_struct(in), sl = ret_struct_lib();
    trace_printf("  ret_struct(): a=\"%s\" b=%u c=%u   ret_struct_lib(): a=\"%s\" b=%u c=%u\n",
                 rs.a, rs.b, rs.c, sl.a, sl.b, sl.c);
    trace_printf("  global s_lib: a=\"%s\" b=%u c=%u\n", s_lib.a, s_lib.b, s_lib.c);
    trace_printf("  weak: foo()=%u  boo()=%u  boo2()=%u  lkj()=%u\n", foo(), boo(), boo2(), lkj());
    trace_printf("  print_lib(): "); print_lib();
    trace_printf("  print_lib2(): "); print_lib2(const_cast<char*>("hi"));
    trace_printf("  library_function3(10)=%u  bb after=%u\n", library_function3(10), bb);

    // ===================== mylib2 — second load path, own globals, weak mm ==========
    trace_printf("\n-- mylib2 --\n");
    trace_printf("  vars: aaa=%u  bbb=%u  xx=%u  str2=\"%s\"\n", aaa, bbb, xx, str2);
    trace_printf("  funcs: lf(39)=%u  lf2(10)=%u  xxx()=%u  weak mm()=%u\n", lf(39), lf2(10), xxx(), mm());
    trace_printf("  pl(): "); pl();
    trace_printf("  pl2(): "); pl2(const_cast<char*>("hi"));
    trace_printf("  lf3(10)=%u  bbb after=%u\n", lf3(10), bbb);

    // ===================== libprov / libcons — cross-module =====================
    trace_printf("\n-- libprov / libcons (cross-module) --\n");
    trace_printf("  prov_value(5)=%u  cons_call(5)=%u (calls prov_value+1)\n", prov_value(5), cons_call(5));

    // ===================== libcppsh — objects, virtual method, static-local table ===
    trace_printf("\n-- libcppsh (C++ objects/methods) --\n");
    trace_printf("  instances before=%u (ctor Banner+=1000)\n", cppsh_instances());
    void* wdg = cppsh_make(7);                          // object ctor (operator new from main)
    trace_printf("  cppsh_make(7)=%p  cppsh_compute(w,5)=%u (virtual)  instances=%u\n",
                 wdg, cppsh_compute(wdg, 5), cppsh_instances());
    cppsh_free(wdg);                                    // object dtor (operator delete from main)
    trace_printf("  after free: instances=%u\n", cppsh_instances());

    // ===================== libcppeh / ehx — C++ exceptions + RTTI (pointer-called) ==
    trace_printf("\n-- libcppeh / ehx (C++ exceptions + RTTI) --\n");
    auto eh_selftest = reinterpret_cast<int (*)(int)>(xip::nopic::resolve_any("eh_selftest"));
    auto eh_dyncast  = reinterpret_cast<int (*)(int)>(xip::nopic::resolve_any("eh_dyncast"));
    auto ehx_cross   = reinterpret_cast<unsigned (*)(unsigned)>(xip::nopic::resolve_any("ehx_catch_cross"));
    trace_printf("  eh_selftest(0)=%d (no-throw path)  eh_dyncast(0)=%d (RTTI dynamic_cast)\n",
                 eh_selftest ? eh_selftest(0) : -1, eh_dyncast ? eh_dyncast(0) : -1);
    trace_printf("  ehx_catch_cross(0)=%u (cross-lib throw wiring, non-throwing arg)\n",
                 ehx_cross ? ehx_cross(0) : 0);

    // ===================== cpp3 — class method, global-object ctor, hard-float, heap =
    trace_printf("\n-- cpp3 (C++ class + hard-float + heap) --\n");
    trace_printf("  vars: cpp3_magic=0x%08x (.rodata)  cpp3_var=0x%08x (.data)\n", cpp3_magic, cpp3_var);
    trace_printf("  ctor: cpp3_made()=%u (Banner+=0x300)  calls before=%u\n", cpp3_made(), cpp3_calls());
    trace_printf("  cpp3_method(7,5)=%u (class method, sinf/cosf/logf/sqrtf from main)\n", cpp3_method(7, 5));
    trace_printf("  cpp3_heap(8)=%u (malloc + new from main's heap)\n", cpp3_heap(8));
    trace_printf("  cpp3_step(3)=%u  cpp3_orchestrate(5)=%u  cpp3_recurse(5)=%u\n",
                 cpp3_step(3), cpp3_orchestrate(5), cpp3_recurse(5));
    trace_printf("  cpp3_float_cross(10)=%u (float cross-lib)  calls after=%u\n",
                 cpp3_float_cross(10), cpp3_calls());

    // ===================== c2 / c1 — static "class" struct, ctor, cyclic cluster =====
    trace_printf("\n-- c2 / c1 (static class + cyclic cluster) --\n");
    trace_printf("  c2 vars: c2_data=0x%08x c2_const=0x%08x c2_bss=0x%08x c2_tag=\"%s\"  state=%u\n",
                 c2_data, c2_const, c2_bss, c2_tag, c2_state());
    trace_printf("  c2_add(10,20)=%u  c2_param()=%u  c2_chain(3)=%u  c2_relay(4)=%u\n",
                 c2_add(10, 20), c2_param(), c2_chain(3), c2_relay(4));
    trace_printf("  c2_step_down(4)=%u (mutual recursion)  c2_scale(1.5f)=%d (float)\n",
                 c2_step_down(4), (int)c2_scale(1.5f));
    trace_printf("  c1 vars: c1_data=0x%08x c1_bss=0x%08x c1_tag=\"%s\"  state=%u\n",
                 c1_data, c1_bss, c1_tag, c1_state());
    trace_printf("  c1_use_fm()=%u  c1_call_c2(7)=%u  c1_into_fm(2)=%u\n",
                 c1_use_fm(), c1_call_c2(7), c1_into_fm(2));

    trace_printf("\noptimal-shared: done\n");
    std::exit(0);
    for (;;) { }
}
