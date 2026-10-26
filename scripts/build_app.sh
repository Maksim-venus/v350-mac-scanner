#!/bin/bash
# Build "V350 Scanner.app" (Apple Silicon) + zip + dmg into dist/.
#   scripts/build_app.sh            needs: Xcode or Command Line Tools, Homebrew libusb (brew install libusb)
# The app bundles: the v350scan CLI, the simulator build (v350scan-sim), the HTML UI and libusb.
# It does NOT bundle anything from Epson: the Epson ICA driver must be installed separately.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${VERSION:-0.1.0}"
MINOS="${MINOS:-13.0}"
ARCH="${ARCH:-arm64}"
LIBUSB_VER="${LIBUSB_VER:-1.0.30}"
BUILD="$ROOT/build"; DIST="$ROOT/dist"
APP="$DIST/V350 Scanner.app"
rm -rf "$BUILD" "$APP"; mkdir -p "$BUILD" "$DIST"

# libusb: by default built from the official release tarball for $MINOS (LGPL-2.1, shipped as a
# separate dylib so it can be replaced). Set LIBUSB_PREFIX=/opt/homebrew/opt/libusb to use Homebrew's
# (then the app needs the macOS version Homebrew built for).
if [ -z "${LIBUSB_PREFIX:-}" ]; then
  LIBUSB="$ROOT/.cache/libusb-$LIBUSB_VER-$ARCH-$MINOS"
  if [ ! -f "$LIBUSB/lib/libusb-1.0.0.dylib" ]; then
    echo "== libusb $LIBUSB_VER from source"
    mkdir -p "$ROOT/.cache"; T="$ROOT/.cache/libusb-$LIBUSB_VER.tar.bz2"
    [ -f "$T" ] || curl -fL -o "$T" "https://github.com/libusb/libusb/releases/download/v$LIBUSB_VER/libusb-$LIBUSB_VER.tar.bz2"
    rm -rf "$BUILD/libusb-src"; mkdir -p "$BUILD/libusb-src"; tar xjf "$T" -C "$BUILD/libusb-src" --strip-components 1
    ( cd "$BUILD/libusb-src" && CFLAGS="-O2 -arch $ARCH -mmacosx-version-min=$MINOS" LDFLAGS="-arch $ARCH -mmacosx-version-min=$MINOS" \
        ac_cv_func_pipe2=no ./configure --prefix="$LIBUSB" --disable-static >/dev/null && make -j8 >/dev/null && make install >/dev/null )
  fi
else
  LIBUSB="$LIBUSB_PREFIX"
fi
[ -f "$LIBUSB/lib/libusb-1.0.0.dylib" ] || { echo "libusb not found at $LIBUSB"; exit 1; }

CFLAGS="-O2 -arch $ARCH -mmacosx-version-min=$MINOS -Wall -Wextra"
SRC="$ROOT/src"
echo "== CLI"
clang $CFLAGS -I"$SRC" -I"$LIBUSB/include/libusb-1.0" -o "$BUILD/v350scan" \
  "$SRC/v350scan.c" "$SRC/v350lib.c" "$SRC/v350tiff.c" "$SRC/v350stitch.c" "$SRC/v350preview.c" "$SRC/v350film.c" \
  -L"$LIBUSB/lib" -lusb-1.0 -framework CoreFoundation -lm -lz -Wl,-rpath,@executable_path/../Frameworks
OLD=$(otool -L "$BUILD/v350scan" | awk '/libusb/{print $1}')
install_name_tool -change "$OLD" @rpath/libusb-1.0.0.dylib "$BUILD/v350scan"
echo "== simulator CLI"
clang $CFLAGS -I"$SRC" -o "$BUILD/v350scan-sim" \
  "$SRC/v350scan.c" "$ROOT/test/mocklib.c" "$SRC/v350tiff.c" "$SRC/v350stitch.c" "$SRC/v350preview.c" "$SRC/v350film.c" -lm -lz
echo "== app"
swiftc -O -swift-version 5 -target "$ARCH-apple-macos$MINOS" -framework AppKit -framework WebKit \
  -o "$BUILD/V350Scanner" "$ROOT/app/V350App.swift"

mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/Frameworks"
cp "$BUILD/V350Scanner" "$BUILD/v350scan" "$BUILD/v350scan-sim" "$APP/Contents/MacOS/"
cp -L "$LIBUSB/lib/libusb-1.0.0.dylib" "$APP/Contents/Frameworks/"
chmod u+w "$APP/Contents/Frameworks/libusb-1.0.0.dylib"
install_name_tool -id @rpath/libusb-1.0.0.dylib "$APP/Contents/Frameworks/libusb-1.0.0.dylib"
cp "$ROOT/gui/index.html" "$APP/Contents/Resources/"
cp "$ROOT/LICENSE" "$ROOT/THIRD_PARTY_NOTICES.md" "$APP/Contents/Resources/"
cp "$ROOT/third_party/libusb/COPYING" "$APP/Contents/Resources/libusb-COPYING.txt"
sed -e "s/__VERSION__/$VERSION/g" -e "s/__MINOS__/$MINOS/g" "$ROOT/app/Info.plist" > "$APP/Contents/Info.plist"
# icon
if [ -f "$ROOT/app/icon_1024.png" ]; then
  IS="$BUILD/AppIcon.iconset"; mkdir -p "$IS"
  for s in 16 32 128 256 512; do
    sips -z $s $s "$ROOT/app/icon_1024.png" --out "$IS/icon_${s}x${s}.png" >/dev/null
    sips -z $((s*2)) $((s*2)) "$ROOT/app/icon_1024.png" --out "$IS/icon_${s}x${s}@2x.png" >/dev/null
  done
  iconutil -c icns "$IS" -o "$APP/Contents/Resources/AppIcon.icns"
fi
echo "== ad-hoc codesign"
codesign --force -s - "$APP/Contents/Frameworks/libusb-1.0.0.dylib"
codesign --force -s - "$APP/Contents/MacOS/v350scan" "$APP/Contents/MacOS/v350scan-sim"
codesign --force -s - "$APP"
codesign --verify --strict --verbose=1 "$APP"
otool -L "$APP/Contents/MacOS/v350scan" | grep -E "libusb|homebrew" || true
for f in "$APP/Contents/MacOS/"* "$APP/Contents/Frameworks/libusb-1.0.0.dylib"; do
  if otool -L "$f" | tail -n +2 | grep -qE "/opt/homebrew|/usr/local|$ROOT"; then
    echo "ERROR: $f still links to something outside the app"; exit 1; fi
done

echo "== zip / dmg"
ZIP="$DIST/V350-Scanner-$VERSION-macOS-$ARCH.zip"; DMG="$DIST/V350-Scanner-$VERSION-macOS-$ARCH.dmg"
rm -f "$ZIP" "$DMG"
( cd "$DIST" && ditto -c -k --sequesterRsrc --keepParent "V350 Scanner.app" "$ZIP" )
ST="$BUILD/dmg"; mkdir -p "$ST"; cp -R "$APP" "$ST/"; ln -s /Applications "$ST/Applications"
hdiutil create -quiet -volname "V350 Scanner" -srcfolder "$ST" -ov -format UDZO "$DMG"
ls -la "$DIST"
echo "built: $APP"
