#!/usr/bin/env bash
# Build ALL the available PIC fixtures for the POSIX dlopen demo.
#
# The .so files under test/fixtures/ are built for the r10 static/XIP loaders
# (single-pic-base). The POSIX dynamic loader (scl::dynamic-newlib) expects
# PLAIN -fPIC images (no r10), so every fixture is rebuilt here from its source
# with the standard `-fPIC -shared` recipe and placed in test-dynamic/fixtures/.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/../fixtures"
FSRC="$HERE/../../test/fixtures-src"
mkdir -p "$OUT"

TC="${TOOLCHAIN_BIN:-/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin}"
CC="$TC/arm-none-eabi-gcc"
CXX="$TC/arm-none-eabi-g++"

MCU="-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16"
PIC="-fPIC -shared -nostartfiles -Wl,-E"
OPT="-O0 -ffunction-sections -fdata-sections"
FMINC="-I $FSRC/include"

# $1=name $2.. = compile args (C)
build_c() {
    local name="$1"; shift
    echo "  $name.so"
    $CC $MCU $PIC $OPT -o "$OUT/$name.so" "$@"
}
# $1=name; rest = args (C++, no exceptions)
build_cxx_noeh() {
    local name="$1"; shift
    echo "  $name.so (no-exceptions)"
    $CXX $MCU $PIC $OPT -std=c++20 -nostdlib -nodefaultlibs \
        -fno-exceptions -fno-rtti -fno-threadsafe-statics \
        -Wl,-E -o "$OUT/$name.so" "$@"
}
# $1=name; rest = args (C++, exceptions ON)
build_cxx_eh() {
    local name="$1"; shift
    echo "  $name.so (exceptions)"
    $CXX $MCU $PIC $OPT -std=c++20 -nostdlib -nodefaultlibs \
        -fno-threadsafe-statics \
        -Wl,-E -o "$OUT/$name.so" "$@"
}

echo "=== building all POSIX-dl fixtures -> $OUT ==="
build_c      "c1"         -I "$FSRC/c1" $FMINC "$FSRC/c1/c1.c"
build_c      "c2"         -I "$FSRC/c2" $FMINC "$FSRC/c2/c2.c"
build_cxx_noeh "cpp3"     -I "$FSRC/cpp3" $FMINC "$FSRC/cpp3/cpp3.cpp"
build_c      "libprov"    -I "$FSRC/libprov" "$FSRC/libprov/libprov.c"
build_c      "libcons"    -I "$FSRC/libcons" "$FSRC/libcons/libcons.c"
build_cxx_noeh "libcppsh" -I "$FSRC/libcppsh/include" \
                 "$FSRC/libcppsh/src/cppsh-api.cpp" "$FSRC/libcppsh/src/widget.cpp"
build_cxx_eh "libcppeh"   -I "$FSRC/libcppeh/include" "$FSRC/libcppeh/src/cppeh.cpp"
build_cxx_eh "ehx_catch"  -I "$FSRC/libcppeh/include" "$FSRC/ehx/ehx-catch.cpp"
build_c      "mylib1"     -I "$FSRC/mylib1" "$FSRC/mylib1/mylib1.c"
build_c      "mylib2"     -I "$FSRC/mylib2" "$FSRC/mylib2/mylib2.c"
build_c      "libsh1"     -I "$HERE/libsh1" "$HERE/libsh1/mylib1.c"
build_c      "libsh2"     -I "$HERE/libsh2" "$HERE/libsh2/mylib2.c"

echo "--- verify: ET_DYN, no r10 ---"
for f in c1 c2 cpp3 libprov libcons libcppsh libcppeh ehx_catch mylib1 mylib2 libsh1 libsh2; do
    t=$("$TC/arm-none-eabi-readelf" -h "$OUT/$f.so" | awk '/Type:/{print $2}')
    echo "  $f.so: $t"
done
echo "done -> $OUT"
