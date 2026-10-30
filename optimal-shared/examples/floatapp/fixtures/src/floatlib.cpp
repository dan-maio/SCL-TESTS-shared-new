/**
 * @file floatlib.cpp
 * @brief A tiny loadable C++ library: hard-float add and multiply of two floats.
 *
 * Built -fno-pic -mlong-calls (see fixtures/build.sh) so the firmware calls its
 * exports directly by name. A file-scope object with a constructor shows
 * .init_array runs at load; the exports are plain C ABI.
 */
#include "floatlib.h"

extern "C" unsigned calls = 0;         // exported library global (a call counter)

namespace {
struct Banner { Banner() noexcept { calls = 0; } };   // ctor runs at load (.init_array)
Banner g_banner;
}

extern "C" float float_add(float a, float b) { ++calls; return a + b; }
extern "C" float float_mul(float a, float b) { ++calls; return a * b; }
