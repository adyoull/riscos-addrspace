# env.sh - where the build finds its tools (sourced by build.sh and
# package.sh). Each can be set from outside.
TOP=$(cd "$(dirname "$0")/.." && pwd)
# The toolchain: riscos-crossdev's (RISCOS_TOOLCHAIN, as its container
# image and `make prebuilt` set it: GCCSDK GCC 10.2 with riscos-unixlib),
# else a GCCSDK environment in ~/gccsdk/env.
GCCSDK_ENV=${GCCSDK_ENV:-${RISCOS_TOOLCHAIN:-$HOME/gccsdk/env}}
CROSS=${CROSS:-$GCCSDK_ENV/bin/arm-riscos-gnueabihf-}
if [ -z "$ELF2AIF" ] && [ -x "$GCCSDK_ENV/bin/elf2aif" ]; then
  ELF2AIF=$GCCSDK_ENV/bin/elf2aif
fi
ELF2AIF=${ELF2AIF:-elf2aif}
OUT=${OUT:-$TOP/build/out}
DIST=${DIST:-$TOP/dist}
VERSION=$(sed -n 's/^#define ADDRSPACE_VERSION *"\(.*\)"/\1/p' "$TOP/src/version.h")
