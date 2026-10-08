# env.sh - where the build finds its tools (sourced by build.sh and
# package.sh). Each can be set from outside.
TOP=$(cd "$(dirname "$0")/.." && pwd)
GCCSDK_ENV=${GCCSDK_ENV:-$HOME/gccsdk/env}
CROSS=${CROSS:-$GCCSDK_ENV/bin/arm-riscos-gnueabihf-}
ELF2AIF=${ELF2AIF:-elf2aif}
OUT=${OUT:-$TOP/build/out}
DIST=${DIST:-$TOP/dist}
VERSION=$(sed -n 's/^#define ADDRSPACE_VERSION *"\(.*\)"/\1/p' "$TOP/src/version.h")
