#!/bin/sh
# build.sh - builds AddrSpace for RISC OS and checks it:
#   $OUT/addrspace_g   the ELF (with debug information: keep it for reading
#                      crash addresses with addr2line; don't ship it)
#   $OUT/!RunImage,ff8 stripped, as AIF
# Checks: every big stack frame is probed, the program was linked with
# UnixLib 5.0.3.3, it has no heap dynamic area (no __dynamic_da_name: it
# mustn't add to what it measures), and no build path or unwanted name is
# left in the shipped image.
# Needs the GCCSDK GCC 10.2 toolchain with UnixLib 5.0.3.3 (docs/TOOLCHAIN.md).
set -e
. "$(dirname "$0")/env.sh"
mkdir -p "$OUT"
CFLAGS="-O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -march=armv7-a -mfpu=vfpv3 -mfloat-abi=hard -fstack-clash-protection"
"${CROSS}gcc" $CFLAGS -I"$TOP/src" "$TOP"/src/*.c -static -o "$OUT/addrspace_g"
"${CROSS}strip" -o "$OUT/addrspace" "$OUT/addrspace_g"
rm -f "$OUT/!RunImage,ff8"
"$ELF2AIF" -e "$OUT/addrspace" "$OUT/!RunImage,ff8" >/dev/null
"${CROSS}size" "$OUT/addrspace_g"

echo "checks:"
python3 "$TOP/tools/check-stack-probes.py" "$OUT/addrspace_g" "${CROSS}objdump" | tail -1 | tee "$OUT/probes.txt"
grep -q '; 0 without stack probes' "$OUT/probes.txt" || { echo "unprobed stack frames" >&2; exit 1; }
CROSS=$CROSS sh "$TOP/tools/check-unixlib.sh" "$OUT/addrspace_g"
if "${CROSS}nm" "$OUT/addrspace_g" | grep -q ' __dynamic_da_name$'; then
  echo "the program has a heap dynamic area (__dynamic_da_name): it would change what it measures" >&2
  exit 1
fi
echo "  no heap dynamic area"
if strings -a "$OUT/!RunImage,ff8" | grep -i -E 'claude|anthropic|(^|[ :="])/(home|root|build)/'; then
  echo "unwanted strings in !RunImage" >&2
  exit 1
fi
echo "  !RunImage: no build paths ($(wc -c < "$OUT/!RunImage,ff8") bytes)"
