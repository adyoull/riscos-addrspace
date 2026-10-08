#!/bin/sh
# check-unixlib.sh ELF... - each program was linked with UnixLib 5.0.3.3.
#
# UnixLib 5.0.3.1 to 5.0.3.3's start-up code claims a 640-byte pthread
# block in the RMA (the ticker code and its counters; earlier ones 472,
# 248 or 120). The test: the "mov r3, #<size>" before OS_Module 6 in
# no_dynamic_area (the start-up path of a program without a heap dynamic
# area, as AddrSpace is). And (REQUIRE) symbols only the wanted UnixLib
# has: __exit_status came with 5.0.3.1-rc7 (_exit(n) exits with n);
# __pthread_held_wait with 5.0.3.3 (stdio pulls it in), which also tells
# 5.0.3.3 from 5.0.3.2. Run by build/build.sh.
CROSS=${CROSS:-/root/gccsdk/env/bin/arm-riscos-gnueabihf-}
EXPECTED=${EXPECTED:-640}
REQUIRE=${REQUIRE:-__exit_status __pthread_held_wait}
WANT=${WANT:-UnixLib 5.0.3.3}
bad=0
for f in "$@"; do
  if ! ${CROSS}nm "$f" 2>/dev/null | grep -q ' __pthread_call_every_code$'; then
    echo "$f: not linked with a UnixLib that has the ticker fix" >&2; bad=1; continue
  fi
  size=$(${CROSS}objdump -d "$f" | awk '/<no_dynamic_area>:$/{p=1;next} p&&/^$/{exit}
    p&&/mov\tr3, #/{s=$0} p&&/svc\t0x0002001e/{sub(/.*#/,"",s); sub(/[ \t;].*/,"",s); print s; exit}')
  syms=$(${CROSS}nm "$f" 2>/dev/null)
  missing=
  for r in $REQUIRE; do
    echo "$syms" | grep -q " $r\$" || missing="$missing $r"
  done
  if [ -n "$missing" ]; then
    echo "$f: no$missing: relink with $WANT" >&2; bad=1; continue
  fi
  if [ "$size" = "$EXPECTED" ]; then
    echo "  $(basename "$f"): $WANT (the $EXPECTED-byte pthread block)"
  else
    echo "$f: claims a ${size:-?}-byte pthread block, not $EXPECTED: relink with $WANT" >&2; bad=1
  fi
done
exit $bad
