// gen-imports.cpp — C++ host tool, the native twin of tools/gen-imports.py.
//
// Reads every shared library (.so) produced by the fixture build and emits, with
// NO hand editing and NO external `readelf`:
//
//   1. imports.generated.h   — one `extern "C"` declaration per exported symbol,
//      so an app can name each function/global directly (call by NAME + a cast to
//      the real prototype: argument types are not recorded in an ELF symbol table).
//   2. modules.generated.inc — the ordered `.so` basenames for the loader loop.
//
// It parses the ELF32 little-endian `.dynsym`/`.dynstr` itself (Cortex-M .so's),
// keeping GLOBAL/WEAK, defined (st_shndx != UNDEF), FUNC/OBJECT symbols.
//
// Usage:  gen-imports <out_header> <out_modules_inc> <lib1.so> [lib2.so ...]
// (Same output as gen-imports.py, which takes an extra <readelf> arg this tool
//  does not need.)  Build with tools/build.sh.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

// ---- minimal ELF32 layout ---------------------------------------------------
constexpr unsigned SHT_DYNSYM = 11;
constexpr unsigned STB_GLOBAL = 1, STB_WEAK = 2;
constexpr unsigned STT_OBJECT = 1, STT_FUNC = 2;

template <class T> T rd(const std::vector<char>& b, std::size_t off) {
    T v{};
    if (off + sizeof(T) <= b.size()) std::memcpy(&v, b.data() + off, sizeof(T));
    return v;
}

struct Sym { std::string name; bool is_func; };

// Read the exported (defined GLOBAL/WEAK FUNC/OBJECT) symbols of one .so.
bool exported_symbols(const char* path, std::vector<Sym>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::fprintf(stderr, "gen-imports: cannot open %s\n", path); return false; }
    std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 52 || std::memcmp(b.data(), "\177ELF", 4) != 0) {
        std::fprintf(stderr, "gen-imports: not an ELF: %s\n", path); return false;
    }
    const std::uint32_t shoff    = rd<std::uint32_t>(b, 0x20);
    const std::uint16_t shentsz  = rd<std::uint16_t>(b, 0x2e);
    const std::uint16_t shnum    = rd<std::uint16_t>(b, 0x30);

    // locate .dynsym and its linked .dynstr
    std::uint32_t sym_off = 0, sym_sz = 0, sym_ent = 0, str_off = 0, str_sz = 0;
    for (std::uint16_t i = 0; i < shnum; ++i) {
        const std::size_t sh = shoff + std::size_t(i) * shentsz;
        if (rd<std::uint32_t>(b, sh + 4) != SHT_DYNSYM) continue;   // sh_type
        sym_off = rd<std::uint32_t>(b, sh + 16);                    // sh_offset
        sym_sz  = rd<std::uint32_t>(b, sh + 20);                    // sh_size
        const std::uint32_t link = rd<std::uint32_t>(b, sh + 24);   // sh_link -> dynstr
        sym_ent = rd<std::uint32_t>(b, sh + 36);                    // sh_entsize
        const std::size_t st = shoff + std::size_t(link) * shentsz;
        str_off = rd<std::uint32_t>(b, st + 16);
        str_sz  = rd<std::uint32_t>(b, st + 20);
        break;
    }
    if (!sym_off || sym_ent < 16) { std::fprintf(stderr, "gen-imports: no .dynsym in %s\n", path); return false; }

    for (std::uint32_t o = 0; o + 16 <= sym_sz; o += sym_ent) {
        const std::size_t s = sym_off + o;
        const std::uint32_t st_name  = rd<std::uint32_t>(b, s + 0);
        const std::uint8_t  st_info  = rd<std::uint8_t>(b, s + 12);
        const std::uint16_t st_shndx = rd<std::uint16_t>(b, s + 14);
        const unsigned bind = st_info >> 4, type = st_info & 0xf;
        if (st_shndx == 0) continue;                               // UNDEF -> import, skip
        if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
        if (type != STT_FUNC && type != STT_OBJECT) continue;
        if (st_name == 0 || str_off + st_name >= str_off + str_sz) continue;
        std::string name(b.data() + str_off + st_name);
        if (auto at = name.find('@'); at != std::string::npos) name.resize(at);  // strip version
        if (name.empty() || !(std::isalpha((unsigned char)name[0]) || name[0] == '_')) continue;
        out.push_back({name, type == STT_FUNC});
    }
    return true;
}

const char* basename(const char* p) {
    const char* s = std::strrchr(p, '/');
    return s ? s + 1 : p;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <out_header> <out_modules_inc> <lib.so>...\n", argv[0]);
        return 2;
    }
    const char* out_h = argv[1];
    const char* out_inc = argv[2];

    std::vector<Sym> order;                 // first definition wins (dups across libs OK)
    std::vector<std::string> seen;
    std::vector<std::string> modules;
    auto known = [&](const std::string& n) {
        for (auto& s : seen) if (s == n) return true; return false;
    };
    for (int i = 3; i < argc; ++i) {
        modules.push_back(basename(argv[i]));
        std::vector<Sym> syms;
        if (!exported_symbols(argv[i], syms)) return 1;
        for (auto& s : syms) if (!known(s.name)) { seen.push_back(s.name); order.push_back(s); }
    }

    std::ofstream h(out_h);
    h << "// AUTO-GENERATED by tools/gen-imports (cpp) — DO NOT EDIT.\n"
         "// One declaration per exported library symbol. Call functions by\n"
         "// NAME with a cast to the real prototype; read globals by NAME.\n"
         "#pragma once\n\n"
         "extern \"C\" {\n";
    for (auto& s : order) {
        if (s.is_func) h << "void " << s.name << "(void);            // function: cast at the call site\n";
        else           h << "extern unsigned char " << s.name << ";   // global: take &" << s.name << ", cast as needed\n";
    }
    h << "}\n";

    std::ofstream inc(out_inc);
    inc << "// AUTO-GENERATED by tools/gen-imports (cpp) — DO NOT EDIT.\n"
           "// Ordered list of module basenames for the loader loop in main.\n";
    for (auto& m : modules) inc << '"' << m << "\",\n";

    std::fprintf(stderr, "gen-imports: %zu symbols from %zu modules\n", order.size(), modules.size());
    return 0;
}
