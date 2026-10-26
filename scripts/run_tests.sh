#!/bin/sh
# Offline tests (no scanner): simulator build, split-mode check, synthetic strips for all film types.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-cc}
$CC -O2 -Wall -Wextra -Isrc -o test/v350scan_mock src/v350scan.c test/mocklib.c src/v350tiff.c src/v350stitch.c src/v350preview.c src/v350film.c -lm -lz
T=$(mktemp -d)
for f in cpos bpos cneg bneg; do
  M_FILM=$f ./test/v350scan_mock --preview --film $f --out "$T/p_$f.tif" > "$T/p_$f.txt" 2>/dev/null
  echo "$f: $(grep 'suggested' "$T/p_$f.txt")"
done
M_FILM=cneg ./test/v350scan_mock --dpi 600 --bits 8 --cuts-from "$T/p_cneg.json" --only 2,3 --outdir "$T/out" 2>&1 | grep saved
ls "$T/out"
# 「输出成片」 end to end (split scan, B&W -> greyscale)
M_FILM=bneg ./test/v350scan_mock --preview --film bneg --out "$T/p_bn.tif" > /dev/null 2>&1
M_FILM=bneg ./test/v350scan_mock --dpi 600 --bits 16 --film bneg --cuts-from "$T/p_bn.json" --finish-from "$T/p_bn.json" --only 2 --outdir "$T/fin" 2>&1 | grep "finish:"
python3 test/synth_finish.py test/v350scan_mock - "$T/finish"
python3 test/synth_film.py test/v350scan_mock
rm -rf "$T"
echo "all tests passed"
