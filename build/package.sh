#!/bin/sh
# package.sh - $DIST/AddrSpace-<version>.zip: !AddrSpace with !RunImage
# (from build.sh), !Sprites and !Sprites11, the licence and the source. The
# zip carries RISC OS filetypes (tools/mkrozip.py).
set -e
. "$(dirname "$0")/env.sh"
[ -f "$OUT/!RunImage,ff8" ] || { echo "run build/build.sh first" >&2; exit 1; }
T=$OUT/pkg
A=$T/!AddrSpace
rm -rf "$T"
mkdir -p "$A"
cp "$TOP/app/!AddrSpace/"* "$A/"
cp "$OUT/!RunImage,ff8" "$A/"
python3 "$TOP/tools/mksprites.py" "$A/!Sprites,ff9"
python3 "$TOP/tools/mksprites.py" --hi "$A/!Sprites11,ff9"   # IconSprites picks it in high-resolution modes
L=$A/docs/Licences
mkdir -p "$L" "$A/docs/source/c" "$A/docs/source/h"
cp "$TOP/COPYING" "$L/AddrSpace,fff"
for f in "$TOP"/src/*.c; do
  b=$(basename "$f" .c); cp "$f" "$A/docs/source/c/$b,fff"
done
for f in "$TOP"/src/*.h; do
  b=$(basename "$f" .h); cp "$f" "$A/docs/source/h/$b,fff"
done
cat > "$A/docs/source/ReadMe,fff" <<EOT
AddrSpace $VERSION's source (c and h). It is built by build/build.sh in
the riscos-addrspace repository, with GCCSDK's GCC 10.2 and UnixLib 5.0.3.3.
EOT
# RISC OS names ignore case: two files that differ only in case would
# overwrite each other when unzipped
dups=$(cd "$T" && find . | tr A-Z a-z | sed 's/,[0-9a-f][0-9a-f][0-9a-f]$//' | sort | uniq -d)
[ -z "$dups" ] || { echo "names that differ only in case: $dups" >&2; exit 1; }
mkdir -p "$DIST"
rm -f "$DIST/AddrSpace-$VERSION.zip"
( cd "$T" && python3 "$TOP/tools/mkrozip.py" "$DIST/AddrSpace-$VERSION.zip" '!AddrSpace' )
ls -l "$DIST/AddrSpace-$VERSION.zip"
