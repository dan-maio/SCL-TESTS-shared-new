/**
 * @file main.cpp — minimal POSIX dynamic-loading test.
 *
 * Only the standard POSIX API: dlopen / dlsym / dlclose / dlerror. Load a .so,
 * resolve an export, call it, unload. main also provides the small symbol
 * surface the fixtures import.
 */
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "firmware/dl-impl.hpp"

/* the standard POSIX API (backed by the SCL facade) */
extern "C" {
void* dlopen(const char* p, int)              { return dl::dlopen(p); }
void* dlsym(void* h, const char* n)           { return dl::dlsym((dl::handle)h, n); }
int   dlclose(void* h)                        { return dl::dlclose((dl::handle)h); }
const char* dlerror(void)                     { return dl::dlerror(); }
}

/* symbols the fixtures import from the firmware */
extern "C" {
struct FmInfo { std::uint32_t id, flags; };
std::uint32_t fm_var = 123;   struct FmInfo fm_info = {0xF1, 7};
void trace_event(const char* o, const char* f, std::uint32_t a) { printf("[%s] %s(%u)\n", o, f, a); }
void trace_u32(const char* l, std::uint32_t v)                   { printf("[trace] %s%u\n", l, v); }
std::uint32_t fm_callback(std::uint32_t x) { return x + 1000; }
unsigned int AA = 42, BB = 84;   char* s1 = (char*)"main-s1";
unsigned int f(unsigned int x) { return x + 10; }
unsigned int w(unsigned int x) { return x + 20; }
}

#ifndef SCL_SO_DIR
#define SCL_SO_DIR "/tmp"
#endif

static int g_fail = 0;

/* dlopen a fixture; returns the handle (or 0 + error on failure). */
static void* open_so(const char* so) {
    char path[160];
    snprintf(path, sizeof path, SCL_SO_DIR "/%s", so);
    void* h = dlopen(path, 0);
    if (!h) { printf("  !dlopen %-10s: %s\n", so, dlerror()); g_fail++; }
    else printf("  dlopen %-10s ok\n", so);
    return h;
}

/* dlsym a `unsigned(unsigned)` export and call it. */
static unsigned call1(void* h, const char* sym, unsigned arg) {
    unsigned (*fn)(unsigned) = (unsigned (*)(unsigned))dlsym(h, sym);
    if (!fn) { printf("  !dlsym %s: %s\n", sym, dlerror()); g_fail++; return 0; }
    return fn(arg);
}

int main(void) {
    puts("scl-fw-posix-dl: dlopen/dlsym/dlclose — all fixtures");

    /* libprov <-> libcons (libcons calls prov_value in libprov) */
    void* a = open_so("libprov.so");
    printf("  libprov: prov_value(5) = %u\n", call1(a, "prov_value", 5));
    void* b = open_so("libcons.so");
    printf("  libcons: cons_call(5)  = %u\n", call1(b, "cons_call", 5));
    dlclose(a); dlclose(b);

    /* independent C / C++ fixtures */
    void* h = open_so("cpp3.so");
    printf("  cpp3:   cpp3_heap(8) = %u\n", call1(h, "cpp3_heap", 8));
    dlclose(h);
    h = open_so("mylib1.so");
    printf("  mylib1: library_function(42) = %u\n", call1(h, "library_function", 42));
    dlclose(h);
    h = open_so("mylib2.so");
    printf("  mylib2: lf(7) = %u\n", call1(h, "lf", 7));
    dlclose(h);
    h = open_so("libsh1.so");
    printf("  libsh1: library_function(7) = %u\n", call1(h, "library_function", 7));
    dlclose(h);
    h = open_so("libsh2.so");
    printf("  libsh2: lf(7) = %u\n", call1(h, "lf", 7));
    dlclose(h);
    h = open_so("libcppsh.so");
    if (h) {
        void* w = ((void* (*)(unsigned))dlsym(h, "cppsh_make"))(7);
        printf("  libcppsh: cppsh_compute(w,5) = %u\n",
               ((unsigned (*)(void*, unsigned))dlsym(h, "cppsh_compute"))(w, 5));
        dlclose(h);
    }

    /* libcppeh <-> ehx_catch (ehx calls eh_throw in libcppeh) */
    a = open_so("libcppeh.so");
    printf("  libcppeh: eh_dyncast(0) = %u\n", call1(a, "eh_dyncast", 0));
    b = open_so("ehx_catch.so");
    printf("  ehx_catch: ehx_catch_cross(0) = %u\n", call1(b, "ehx_catch_cross", 0));
    dlclose(a); dlclose(b);

    /* c1 -> c2 (c1 calls c2_add in c2) */
    a = open_so("c2.so");
    b = open_so("c1.so");
    printf("  c1:   c1_call_c2(7) = %u\n", call1(b, "c1_call_c2", 7));
    dlclose(a); dlclose(b);

    printf("RESULT: %s\n", g_fail ? "FAILURE" : "SUCCESS");
    return g_fail ? 1 : 0;
}
