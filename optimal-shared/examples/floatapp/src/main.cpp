/**
 * @file main.cpp
 * @brief WeAct F411CE app — load floatlib.so over SEMIHOSTING and use its
 *        hard-float add/multiply BY NAME, printing the results to UART (USART1).
 *
 * Follows optimal-shared/docs/import_in_project.md:
 *   - the SCL loader is pulled in as an INTERFACE library (see CMakeLists.txt);
 *   - the .so is read from the host over semihosting into the on-chip flat-fs,
 *     then loaded; xip::nopic::patch_main() fills main's GOT so float_add/float_mul
 *     are ordinary direct calls;
 *   - results go to the on-board UART (PA9/PA10, 115200 8N1).
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

extern "C" { void uart_init(void); void uart_puts(const char* s); }

#ifndef DL_LIB_DIR
#define DL_LIB_DIR "."
#endif

// ---- floatlib's exports, declared by hand (its API — no gen-imports needed) ----
extern "C" {
float float_add(float, float);
float float_mul(float, float);
extern unsigned calls;
}

namespace {

// Minimal float -> "int.frac" (3 decimals), enough for a UART print without %f.
void ftoa(float v, char* out) {
    if (v < 0) { *out++ = '-'; v = -v; }
    unsigned ip = (unsigned)v;
    unsigned fp = (unsigned)((v - (float)ip) * 1000.0f + 0.5f);
    std::sprintf(out, "%u.%03u", ip, fp);
}

// Read floatlib.so from the host over semihosting into the on-chip flat-fs (flash).
std::span<const std::byte> provide(const char* name) {
    char path[160];
    std::snprintf(path, sizeof(path), "%s/%s", DL_LIB_DIR, name);
    firmware::semihost_file src(path);
    if (!src.is_open()) { trace_printf("absent: %s\n", name); return {}; }
    const std::size_t sz = src.size();
    auto memp = fw::image_arena().alloc(sz);
    auto* buf = static_cast<std::byte*>(*memp);
    bool ok = src.read_at(0, std::span<std::byte>(buf, sz)).has_value() &&
              fw::fs_add(name, buf, (std::uint32_t)sz).has_value();
    fw::image_arena().free(buf);
    if (!ok) return {};
    const fw::FsEntry* e = fw::fs_find(name);
    return e ? std::span<const std::byte>(fw::fs_blob(*e), e->size) : std::span<const std::byte>{};
}

}  // namespace

extern "C" int main(void) {
    uart_init();
    uart_puts("floatapp: loading floatlib.so over semihosting...");
    trace_printf("floatapp: start\n");

    fw::fs_format();
    const char* kModules[] = { "floatlib.so" };           // hand-written load list (one lib)
    optimal::Module mods[1];
    std::span<const std::byte> blobs[1];
    blobs[0] = provide(kModules[0]);
    int n = optimal::load_and_link(blobs, 1, mods);
    trace_printf("floatapp: %d module(s) loaded + linked\n", n);
    if (n != 1) { uart_puts("floatapp: LOAD FAILED"); std::exit(1); }

    // ---- use the library BY NAME (hard-float) ----
    const float a = 2.5f, b = 3.5f;
    float sum = float_add(a, b);        // 6.0
    float prod = float_mul(a, b);       // 8.75

    char line[96], sa[24], sb[24], sr[24];
    ftoa(a, sa); ftoa(b, sb);
    ftoa(sum, sr);
    std::sprintf(line, "float_add(%s, %s) = %s", sa, sb, sr); uart_puts(line);
    trace_printf("%s\n", line);
    ftoa(prod, sr);
    std::sprintf(line, "float_mul(%s, %s) = %s", sa, sb, sr); uart_puts(line);
    trace_printf("%s\n", line);
    std::sprintf(line, "floatlib calls = %u", calls);        uart_puts(line);

    uart_puts("floatapp: done");
    trace_printf("floatapp: done\n");
    std::exit(0);
    for (;;) {}
}
