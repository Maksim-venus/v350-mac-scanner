#!/bin/sh
# Build only the CLI against Homebrew libusb (brew install libusb pkg-config).
set -e
cd "$(dirname "$0")/.."
export PKG_CONFIG_PATH="/opt/homebrew/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
mkdir -p build
clang -O2 -Wall -Wextra -Isrc -o build/v350scan src/v350scan.c src/v350lib.c src/v350tiff.c src/v350stitch.c src/v350preview.c src/v350film.c \
  $(pkg-config --cflags --libs libusb-1.0) -framework CoreFoundation -lm -lz
echo "built $(pwd)/build/v350scan"
