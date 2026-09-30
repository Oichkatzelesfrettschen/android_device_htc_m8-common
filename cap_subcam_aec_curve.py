#!/usr/bin/env python3
"""Cap the OV2722 subcamera AEC exposure curve so camera 2 holds 30 fps.

libtuning_aec_ov2722_subcam_zsl.so carries one 0x281c-byte AEC tuning block
per main sensor (CheckSensor on /sys/android_camera/sensor). Each block's
exposure curve is rows of two little-endian u32 words from block offset
0x4a0 to 0x1340: the line count, then the gain in Q8 (256 = 1x) in a 16-bit
field. The rows run out to 3201 lines, so in dim light the subcamera AEC
stretches the frame to 10.5 fps and preview-fps-range never shortens it.

Two edits per block:

- Across the rows at the block's longest line count, the gain is made
  non-decreasing. The stock OV4688 block's gain field wraps at row 457
  (249x falls to 0.65x), so the darkest scenes would otherwise drop about
  eight stops.
- Every row longer than CAP lines gets CAP lines and its gain scaled by
  lines / CAP, clamped to 0xffff (256x), so the row keeps its exposure up
  to that clamp. A row whose gain is 0 keeps gain 0.

CAP is 1096 lines: the OV2722 exposure ends 4 lines before the frame does,
so the frame length can drop to 1100 lines (about 30.3 fps at the 29.79 us
line time) and msm_duo_sync can shorten camera 2's frame to camera 0's
period. The evidence is htc-workbench
evidence/camera-duo-camera2-aec-curve-cap-111-20260929.

The rewrite keys on sha256: the stock file is rewritten, a file already
holding the output is left unchanged, and any other file is an error.

Usage: cap_subcam_aec_curve.py FILE...
"""

import hashlib
import struct
import sys
from pathlib import Path

STOCK = "c0c76fdb59d91262b2b0a84bf5c3189bce6d0e9e69a491096fc63d5e8c84b580"
CAPPED = "2c849901f387136d1bd4d012ec16d31bb70b648e07b87eb98abcfd1357e152fc"
CAP = 1096
GAIN_MAX = 0xFFFF
ROWS_LO = 0x4A0
ROWS_HI = 0x1340
# .data starts at vaddr 0x4000, file offset 0x3000; block vaddrs are
# vd6869, ov4688, imx214, ov13850_1140m, ov13850 and the default block.
DATA_DELTA = 0x1000
BLOCKS = (0x6824, 0xE078, 0xB85C, 0x4004, 0x10894, 0x9040)


def cap_curve(data: bytearray) -> None:
    for block in BLOCKS:
        base = block - DATA_DELTA
        offs = range(base + ROWS_LO, base + ROWS_HI, 8)
        rows = [list(struct.unpack_from("<II", data, o)) for o in offs]
        top = max(lines for lines, _ in rows)
        prev = 0
        for row in rows:
            if row[0] == top and row[1]:
                row[1] = max(row[1] & 0xFFFF, prev)
                prev = row[1]
        for o, (lines, gain) in zip(offs, rows):
            if lines > CAP:
                gain = min(GAIN_MAX, round((gain & 0xFFFF) * lines / CAP))
                lines = CAP
            struct.pack_into("<II", data, o, lines, gain)


def rewrite(path: Path) -> str:
    data = path.read_bytes()
    sha = hashlib.sha256(data).hexdigest()
    if sha == STOCK:
        out = bytearray(data)
        cap_curve(out)
        path.write_bytes(bytes(out))
        return "capped"
    if sha == CAPPED:
        return "already capped"
    raise SystemExit(f"{path}: sha256 {sha} is neither the stock nor the capped library")


def main(argv: list[str]) -> int:
    if not argv:
        raise SystemExit(__doc__)
    for name in argv:
        print(f"{name}: {rewrite(Path(name))}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
