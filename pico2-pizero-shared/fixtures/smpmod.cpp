/**
 * @file smpmod.cpp
 * @brief The module both cores call. All its writes go through main's lock, and
 *        the read-only table and FPU use run concurrently on both cores.
 *
 * Exports use one signature so the harness can enter any model the same way.
 */
#include <stdint.h>

extern "C" {
void app_lock(void);
void app_unlock(void);
unsigned app_cpu_id(void);
}

namespace {

const uint32_t kTable[8] = {0x9E3779B1u, 0x85EBCA77u, 0xC2B2AE3Du, 0x27D4EB2Fu,
                            0x165667B1u, 0xD3A2646Cu, 0xFD7046C5u, 0xB55A4F09u};
uint32_t g_magic;
uint32_t g_shared;
uint32_t g_per_core[2];

struct Init {
    Init() noexcept { g_magic = 0x5CA1AB1Eu; }
};
Init g_init;

}  // namespace

extern "C" {

uint32_t mod_magic(uint32_t, uint32_t) { return g_magic; }

uint32_t mod_compute(uint32_t a, uint32_t b) {
    uint32_t h = a ^ b;
    for (unsigned i = 0; i < 8; ++i) h = (h ^ kTable[i]) * 31u + i;
    return h;
}

uint32_t mod_fmix(uint32_t a, uint32_t b) {
    volatile float x = (float)(a & 0xFFFFu), y = (float)(b & 0xFFFFu);
    return (uint32_t)(x * 0.5f + y * 0.25f);
}

/* Increment under main's lock. Interrupts are off there, so the core cannot change. */
uint32_t mod_note(uint32_t, uint32_t) {
    app_lock();
    const uint32_t cpu = app_cpu_id() & 1u;
    ++g_per_core[cpu];
    const uint32_t v = ++g_shared;
    app_unlock();
    return v;
}

uint32_t mod_shared(uint32_t, uint32_t) { return g_shared; }
uint32_t mod_core_count(uint32_t cpu, uint32_t) { return g_per_core[cpu & 1u]; }

}  // extern "C"
