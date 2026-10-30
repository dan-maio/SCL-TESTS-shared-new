/**
 * @file floatlib.cpp
 * @brief Loadable hard-float add/multiply library, built as an ET_REL module
 *        (relocatable object) for scl::reloc — see fixtures/Makefile.
 *
 * A file-scope object with a constructor proves .init_array runs at load; the
 * exports are plain C ABI so main can reach them by name.
 */
#include "floatlib.h"

extern "C" unsigned calls = 0;                 // exported library global (call counter)

namespace {
struct Banner { Banner() noexcept { calls = 0; } };   // ctor runs at load (.init_array)
Banner g_banner;
}

extern "C" float float_add(float a, float b) { ++calls; return a + b; }
extern "C" float float_mul(float a, float b) { ++calls; return a * b; }

// The module imports this from main, so loading only works if main's table is used.
extern "C" float app_gain(void);
extern "C" float float_scale(float a, float b) { ++calls; return a * b * app_gain(); }
