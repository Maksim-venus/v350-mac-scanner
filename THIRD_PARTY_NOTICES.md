# Third-party notices

## libusb 1.0.30 (bundled in the app as `Contents/Frameworks/libusb-1.0.0.dylib`)

- Project: https://libusb.info / https://github.com/libusb/libusb
- License: GNU Lesser General Public License v2.1 or later (full text: `third_party/libusb/COPYING`,
  and `Contents/Resources/libusb-COPYING.txt` inside the app).
- Source: the unmodified release tarball
  https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2
  (`scripts/build_app.sh` downloads and builds exactly this).
- libusb is linked dynamically and shipped as a separate file. You may replace
  `V350 Scanner.app/Contents/Frameworks/libusb-1.0.0.dylib` with your own build of libusb
  (re-sign the app afterwards with `codesign --force -s - "V350 Scanner.app"`).

## Epson software (NOT included)

This project does not contain, redistribute or modify any Epson software or firmware.
At run time the CLI loads `Interpreter 68.bundle` (and through it `Esfw68.bin`) from Epson's own
ICA Scanner Driver, which the user installs from Epson:
https://epson.com/Support/Scanners/Perfection-Series/Epson-Perfection-V350-Photo/s/SPT_B11B185011
Epson, Perfection and Image Capture-related names are trademarks of their respective owners.
This project is not affiliated with or endorsed by Seiko Epson Corporation.
