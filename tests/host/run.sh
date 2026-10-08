#!/bin/sh
# run.sh - AddrSpace's host tests.
#
# 1. areas_test: src/areas.c against tests/host/fake_os.c (dynamic areas,
#    tasks, the caps, and free address space that areas are made from).
# 2. ui_test: the front end (src/main.c) against a scripted fake Wimp and
#    the same fake areas.
# Both are built for arm-linux and run under qemu-arm, because the Task
# Manager's and the Wimp's blocks hold 32-bit pointers. QEMU=<riscos-ffmpeg's
# qemu-arm-aligntrap> also traps unaligned accesses, as RISC OS does.
# 3. The version in src/version.h and !Help agree.
# Needs: gcc-arm-linux-gnueabihf, qemu-user.
set -e
TOP=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-/tmp/addrspace-tests}
mkdir -p "$OUT"
CC="arm-linux-gnueabihf-gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -marm -mno-unaligned-access -no-pie"
$CC -I"$TOP/src" -I"$TOP/tests/host" -o "$OUT/areas_test" \
  "$TOP/tests/host/areas_test.c" "$TOP/tests/host/fake_os.c" "$TOP/src/areas.c"
$CC -DADDRSPACE_TEST -DADDRSPACE_NO_MAIN -I"$TOP/tests/host/fake" -I"$TOP/src" -I"$TOP/tests/host" -o "$OUT/ui_test" \
  "$TOP/tests/host/ui_test.c" "$TOP/tests/host/fake_os.c" "$TOP/src/areas.c" "$TOP/src/sys.c" "$TOP/src/main.c"

bad=0
for t in areas_test ui_test; do
  end=$(arm-linux-gnueabihf-readelf -lW "$OUT/$t" | awk '$1=="LOAD" && / R E / {print $3, $6; exit}')
  lo=$(( ${end% *} )); hi=$(( ${end% *} + ${end#* } ))
  QEMU_ARM_ALIGN_TRAP=1 QEMU_ARM_ALIGN_IGNORE=$(printf '0-%x,%x-ffffffff' $lo $hi) \
  QEMU_LD_PREFIX=${QEMU_LD_PREFIX:-/usr/arm-linux-gnueabihf} \
    timeout 120 "${QEMU:-qemu-arm}" "$OUT/$t" "$OUT" || bad=1
done

v=$(sed -n 's/^#define ADDRSPACE_VERSION *"\(.*\)"/\1/p' "$TOP/src/version.h")
if head -1 "$TOP/app/!AddrSpace/!Help,fff" | grep -q " $v\$"; then
  echo "versions agree: $v"
else
  echo "FAIL: !Help's first line doesn't end with $v" >&2
  bad=1
fi
exit $bad
