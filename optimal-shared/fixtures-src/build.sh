#!/usr/bin/env bash
# build.sh — build ALL fixtures with ONE generic recipe. No per-library hand files.
#
# Every module is compiled `-fno-pic -mlong-calls` and linked at a FIXED address
# so `main` can call its exports by name with a plain call — no r10, no GOT/PLT
# plumbing in the app. The fixed addresses are NOT hand-picked: this script
#   pass 1: builds each module at a placeholder base and measures its two PT_LOAD
#           sizes (readelf),
#   pass 2: PACKS every module contiguously inside the app's reserved windows,
#   pass 3: re-links each module at its packed address.
# Overlap is impossible by construction; overflow of the windows fails the build.
#
# To add a library: append one line to the `MODULES` table below (name + sources).
# Nothing else — no linker script, no address, no edit to main.cpp. The script
# also emits fixtures/manifest.txt, which the firmware loads in order.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/../fixtures"
SCL="$HERE/.."                             # project root (self-contained SCL)
# Fixture sources live in this project (fixtures-src/src) — build from here.
FSRC="${FIXTURES_SRC:-$HERE/src}"
SLIB_C="$HERE/src/slib.c"
LD_TMPL="$HERE/ldscripts/nopic-split.ld"
APP_LD="$HERE/../test/common/ldscripts/sections-flatfs-hosted-nopic.ld"   # single source of truth for windows
mkdir -p "$OUT"

source "$SCL/tools-config.sh" 2>/dev/null || true
TC="${ARM_TOOLCHAIN_BIN:-}"
if [[ ! -x "$TC/arm-none-eabi-gcc" ]]; then
    TC=/home/dan/.local/xPacks/@xpack-dev-tools/arm-none-eabi-gcc/15.2.1-1.1.1/.content/bin
fi
[[ -x "$TC/arm-none-eabi-gcc" ]] || { echo "arm-none-eabi-gcc not found; set ARM_TOOLCHAIN_BIN" >&2; exit 1; }
CC="$TC/arm-none-eabi-gcc"; CXX="$TC/arm-none-eabi-g++"; READELF="$TC/arm-none-eabi-readelf"

MCU="-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16"
NOPIC="-fno-pic -mlong-calls"
OPT="-O0 -ffunction-sections -fdata-sections"
LDFLAGS="-Wl,-E -Wl,-z,max-page-size=0x100 -Wl,--unresolved-symbols=ignore-all"
FMINC="-I $FSRC/include"

# ---- the ONE table you edit to add a library --------------------------------
#   name | compiler-kind (c|cpp|eh) | source(s) | extra include flags
#   c  = plain C          cpp = C++ no-exceptions/no-rtti   eh = C++ EH+RTTI on
declare -a MODULES=(
  "slib      c    $SLIB_C"
  "mylib1    c    $FSRC/mylib1/mylib1.c              -I$FSRC/mylib1 $FMINC"
  "mylib2    c    $FSRC/mylib2/mylib2.c              -I$FSRC/mylib2 $FMINC"
  "libprov   c    $FSRC/libprov/libprov.c            -I$FSRC/libprov $FMINC"
  "libcons   c    $FSRC/libcons/libcons.c            -I$FSRC/libcons $FMINC"
  "libcppsh  cpp  $FSRC/libcppsh/src/cppsh-api.cpp $FSRC/libcppsh/src/widget.cpp -I$FSRC/libcppsh/include"
  "libcppeh  eh   $FSRC/libcppeh/src/cppeh.cpp        -I$FSRC/libcppeh/include"
  "ehx_catch eh   $FSRC/ehx/ehx-catch.cpp            -I$FSRC/libcppeh/include"
  "cpp3      cpp  $FSRC/cpp3/cpp3.cpp                -I$FSRC/cpp3 $FMINC"
  "c2        c    $FSRC/c2/c2.c                      -I$FSRC/c2 $FMINC"
  "c1        c    $FSRC/c1/c1.c                      -I$FSRC/c1 $FMINC"
)
# Modules NOT linked into main with -l: (their EH/RTTI runtime pulls libgcc's
# hidden __aeabi_unwind_cpp_pr* under -Wl,-E). They are still loaded at runtime
# and called through a typed function pointer (Module::sym) — see main.cpp.
NOLINK="libcppeh ehx_catch"
names() { for m in "${MODULES[@]}"; do echo "${m%% *}"; done; }

# Chip RAM geometry (NOT window addresses) read from the app linker script:
# the module windows are derived from the top of RAM downward, so they never
# need to be typed by hand — see windows.generated.ld emitted below.
tonum() { local v="${1//K/*1024}"; v="${v//M/*1024*1024}"; echo $(( v )); }
RAM_LINE=$(grep -E 'RAM[[:space:]]*\(rwx\)[[:space:]]*:' "$APP_LD" | head -1)
RAM_ORIGIN=$(tonum "$(sed -nE 's/.*ORIGIN[[:space:]]*=[[:space:]]*(0x[0-9A-Fa-f]+).*/\1/p' <<<"$RAM_LINE")")
RAM_LEN=$(tonum "$(sed -nE 's/.*LENGTH[[:space:]]*=[[:space:]]*([0-9]+[KM]?).*/\1/p' <<<"$RAM_LINE")")
STACK=$(tonum "$(sed -nE 's/.*__Main_Stack_Size[[:space:]]*=[[:space:]]*([0-9]+([[:space:]]*\*[[:space:]]*[0-9]+)*).*/\1/p' "$APP_LD" | head -1 | tr -d ' ')")
: "${STACK:=$((1024*3))}"
if [ -z "$RAM_ORIGIN" ] || [ "${RAM_ORIGIN:-0}" -le 0 ] || [ "${RAM_LEN:-0}" -le 0 ]; then
  echo "  ERROR: could not read RAM ORIGIN/LENGTH from $APP_LD" >&2; exit 1
fi
RAM_TOP=$(( RAM_ORIGIN + RAM_LEN ))
printf '  RAM 0x%x..0x%x  stack %u  (windows derived top-down)\n' "$RAM_ORIGIN" "$RAM_TOP" "$STACK"

SLOT_ALIGN=0x100
declare -A TADDR DADDR
PH_T=$(( RAM_ORIGIN + RAM_LEN/4 ))          # distinct placeholder VAs for pass-1
PH_D=$(( RAM_ORIGIN + RAM_LEN/2 ))          # (non-overlapping; sizes are placement-independent)

link_mod() { # name kind textbase database source...
  local name="$1" kind="$2" tb="$3" db="$4"; shift 4
  local ld="$OUT/$name.ld"
  sed -e "s/__TEXT_BASE__/$tb/g" -e "s/__DATA_BASE__/$db/g" "$LD_TMPL" > "$ld"
  local cc="$CC" flags=""
  if [ "$kind" = "cpp" ]; then
    cc="$CXX"; flags="-std=c++20 -nostdlib -nodefaultlibs -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit"
  elif [ "$kind" = "eh" ]; then
    cc="$CXX"; flags="-std=c++20 -nostdlib -nodefaultlibs -fno-threadsafe-statics -fno-use-cxa-atexit"
  fi
  $cc $MCU $NOPIC $OPT $flags -shared -nostartfiles $LDFLAGS -Wl,--gc-sections -T "$ld" -o "$OUT/$name.so" "$@"
}
build_all() {
  for m in "${MODULES[@]}"; do
    read -r name kind rest <<<"$m"
    link_mod "$name" "$kind" "${TADDR[$name]}" "${DADDR[$name]}" $rest
  done
}
measure() { # name -> TXT_SZ DAT_SZ
  local f="$OUT/$1.so"
  TXT_SZ=$("$READELF" -l "$f" | awk '$1=="LOAD" && $7=="R" {print $6; exit}')
  DAT_SZ=$("$READELF" -l "$f" | awk '$1=="LOAD" && $7 ~ /W/ {print $6; exit}')
}
align_up()   { echo $(( ($1 + $2 - 1) & ~($2 - 1) )); }
align_down() { echo $(( $1 & ~($2 - 1) )); }

echo "=== pass 1: build at a placeholder VA + measure sizes"
for n in $(names); do TADDR[$n]=$(printf '0x%x' "$PH_T"); DADDR[$n]=$(printf '0x%x' "$PH_D"); done
build_all

echo "=== pass 2: total sizes -> place windows top-down (below the stack)"
declare -A TSZ DSZ; tot_t=0; tot_d=0; max_file=0
for n in $(names); do
  measure "$n"; TSZ[$n]=$TXT_SZ; DSZ[$n]=$DAT_SZ
  tot_t=$(( tot_t + $(align_up $TXT_SZ $SLOT_ALIGN) ))
  tot_d=$(( tot_d + $(align_up $DAT_SZ $SLOT_ALIGN) ))
  fsz=$(stat -c%s "$OUT/$n.so"); (( fsz > max_file )) && max_file=$fsz
done
ARENA=$(align_up "$max_file" 0x400)                       # provisioning staging = largest .so
DATA_END=$(align_down $(( RAM_TOP - STACK )) 8)
DATA_START=$(align_down $(( DATA_END - tot_d )) 8)
TEXT_END=$DATA_START
TEXT_START=$(align_down $(( TEXT_END - tot_t )) 0x20)
if (( TEXT_START <= RAM_ORIGIN )); then
  echo "  ERROR: modules ($(printf 0x%x $((tot_t+tot_d)))) + stack do not fit in RAM" >&2; exit 1
fi
cur_t=$TEXT_START; cur_d=$DATA_START
for n in $(names); do
  TADDR[$n]=$(printf '0x%x' "$cur_t"); DADDR[$n]=$(printf '0x%x' "$cur_d")
  printf '  %-9s text %-10s (0x%x)  data %-10s (0x%x)\n' "$n" "${TADDR[$n]}" "${TSZ[$n]}" "${DADDR[$n]}" "${DSZ[$n]}"
  cur_t=$(( cur_t + $(align_up ${TSZ[$n]} $SLOT_ALIGN) ))
  cur_d=$(( cur_d + $(align_up ${DSZ[$n]} $SLOT_ALIGN) ))
done

echo "=== pass 3: re-link at the derived (packed) addresses"
build_all

# Emit the generated linker fragment the app script INCLUDEs — no hand addresses.
cat > "$OUT/windows.generated.ld" <<LDEOF
/* AUTO-GENERATED by fixtures-src/build.sh — DO NOT EDIT.
 * Module windows derived from RAM geometry (0x$(printf %x $RAM_ORIGIN)..0x$(printf %x $RAM_TOP),
 * stack ${STACK}B) and the measured module sizes; packed top-down below the stack.
 */
_simage_arena_size = $(printf '0x%x' $ARENA) ;
_sxip_text_start   = $(printf '0x%x' $TEXT_START) ;
_sxip_text_end     = $(printf '0x%x' $TEXT_END) ;
_sxip_data_start   = $(printf '0x%x' $DATA_START) ;
_sxip_data_end     = $(printf '0x%x' $DATA_END) ;
LDEOF
printf '  windows: text 0x%x..0x%x  data 0x%x..0x%x  arena 0x%x  -> windows.generated.ld\n' \
    "$TEXT_START" "$TEXT_END" "$DATA_START" "$DATA_END" "$ARENA"

# manifest.txt = every module, in load order (the firmware loads all of these).
# link.txt     = the subset linked into main with -l: for by-name calls (all but
#                the EH/RTTI libs, which main reaches via a typed function pointer).
: > "$OUT/manifest.txt"; : > "$OUT/link.txt"
for n in $(names); do
  echo "$n.so" >> "$OUT/manifest.txt"
  case " $NOLINK " in *" $n "*) : ;; *) echo "$n.so" >> "$OUT/link.txt" ;; esac
done
echo "done -> $OUT ($(names | wc -l) modules; $(wc -l < "$OUT/link.txt") linked by name); manifest.txt + link.txt written"
