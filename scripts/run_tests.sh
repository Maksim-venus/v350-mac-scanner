#!/bin/sh
# Offline tests (no scanner): simulator build, split-mode check, synthetic strips for all film types.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-cc}
$CC -O2 -Wall -Wextra -Isrc -o test/v350scan_mock src/v350scan.c test/mocklib.c src/v350tiff.c src/v350stitch.c src/v350preview.c -lm -lz
T=$(mktemp -d)
for f in cpos bpos cneg bneg; do
  M_FILM=$f ./test/v350scan_mock --preview --film $f --out "$T/p_$f.tif" > "$T/p_$f.txt" 2>/dev/null
  echo "$f: $(grep 'suggested' "$T/p_$f.txt")"
done
M_FILM=cneg ./test/v350scan_mock --dpi 600 --bits 8 --cuts-from "$T/p_cneg.json" --only 2,3 --outdir "$T/out" 2>&1 | grep saved
ls "$T/out"
python3 test/synth_film.py test/v350scan_mock
rm -rf "$T"
echo "all tests passed"
