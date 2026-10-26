# Protocol and AFL notes (Epson Perfection V350 / GT-F700, USB 04b8:012f)

These are our own notes from using the scanner, written in our own words. They contain no Epson code.

## Architecture

The V350 speaks Epson's ESC/I command language, but on macOS the host does not send ESC/I to the
USB endpoint directly. Epson's ICA driver contains a per-model *interpreter* plug-in
(`Interpreter 68.bundle`, which also uploads the scanner firmware `Esfw68.bin` at power-up). The host:

1. opens the USB device with libusb, claims interface 0 and finds the bulk IN/OUT endpoints;
2. loads the interpreter with CoreFoundation (`CFBundle`) and calls its init function, passing two
   callbacks that do the raw bulk reads/writes;
3. sends ESC/I commands to the interpreter's write function and reads replies from its read function.
   The interpreter translates them into the scanner's native protocol.

The interpreter bundle is universal (arm64 + x86_64), so this works natively on Apple Silicon.
v350scan looks for it at
`/Library/Image Capture/Devices/EPSON Scanner.app/Contents/PlugIns/Interpreter 68.bundle`
(override: `--interpreter PATH` or `V350_INTERPRETER`).

## Commands used

| Command | Meaning (as used here) |
|---|---|
| `ESC @` | reset / initialise; answered with ACK when the scanner is ready |
| `ESC F` | short status (STX, status byte, 2 bytes) |
| `ESC f` | extended status: warming-up bit, option unit present / enabled, option area |
| `ESC e n` | select the source: 0 flatbed, 1 transparency unit, 5 auto film loader |
| `ESC R` | resolution (x, y; 16-bit little-endian each) |
| `ESC C` | colour mode (0x13 = RGB line sequence, 0x10 = grey) |
| `ESC D` | bit depth (8 or 16) |
| `ESC N` | film type: 0 = positive, 1 = negative (only accepted with TPU/AFL selected) |
| `ESC A` | scan area x, y, w, h in pixels at the `ESC R` resolution |
| `ESC G` | start; image arrives in blocks (4-byte header with status and size), host ACKs each block |
| `CAN` | sent instead of a block ACK to cancel a transfer |

16-bit samples are little-endian. Width for the AFL is the full 1.45" (rounded down to a multiple of 8 pixels).

## Auto film loader behaviour (observed)

- AFL area: 1.45" × 9.30". 135 strips are up to 6 frames (≈ 9").
- **300 dpi** runs as one 9.30" window. The feeder moves the film past the parked carriage
  ("preview path"). The film moves about 3 % further than nominal, so a 300 dpi preview is
  vertically about 3 % short. This is measured and compensated (`F_PREV`).
- **600–4800 dpi**: the feeder positions the film once per window (the `ESC A` y offset becomes a
  feeder advance), and the carriage then scans the window. Long windows fail on this path
  (USB I/O error at 5.5" at 600 dpi, stall at 6.8" at 1200 dpi; a too-long window once made the belt
  grind). v350scan therefore scans one window per frame (about 1.5–1.8") and refuses windows over 4.5".
- The per-window advance is also slightly long (about 2.7 % at 600 dpi, `F_WIN`), with a constant
  offset (`K`). The gap positions found in each window measure the remaining error, and that feeds
  forward into the next window.
- The film sits about 0.35° skewed to the carriage (≈ 5 px x shift across a frame at 600 dpi).
- Eject: `ESC e 0` after `ESC w` did not move the strip on the test unit and once left the scanner
  unresponsive; use the eject button.

## Film type

Epson's own software sends `ESC N 1` for negatives, and the scanner then uses a different exposure /
gain calibration (more exposure, especially for the blue channel behind the orange mask). v350scan
always scans with `ESC N 0` and saves the raw transmission data; negative inversion and mask removal
are left to the user's software. Whether `ESC N 1` gives better shadow detail for dense negatives has
not been tested on hardware yet.

## Frame-gap detection (300 dpi preview)

Per image row (central 60 % of the width, green channel): 10th / 50th / 90th percentiles.

- No film: row median ≥ 0.9 of full scale (lamp shines straight through), at the strip ends.
- Positives: a gap row has its 90th percentile below 1 % (black).
- Negatives: estimate the film-base level B as a high percentile of the row 10th percentiles.
  A gap row has p10 ≥ 0.85·B and p90 ≤ 1.12·B, i.e. it is uniformly at base level. Dense
  (over-exposed) frames are darker than base and thin frames still have texture, so neither matches.
  Unexposed leader is base too and is treated like a long gap.
- Runs of 0.01–0.35" become gaps (cut in the middle); longer runs are leader / blank frames (cuts
  0.04" inside their edges). Over-long segments are split only inside gap-like rows.
