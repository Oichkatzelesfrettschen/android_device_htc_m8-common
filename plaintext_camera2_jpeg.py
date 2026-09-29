#!/usr/bin/env python3
"""Deliver camera 2 JPEGs from camera.vendor.msm8974.so as plaintext.

QCameraPostProcessor::processJpegEvt compares the owning
QCamera2HardwareInterface's camera id (the byte at this+0x22c) with 2. On
equality, and when CheckMFG() finds ro.bootmode is not factory2, it rounds
the encoded length up to 16, copies the JPEG into an uninitialized heap
buffer of that size, and runs af_aes_cbc_encrypt from libhtc_depthmap.so
over it before the CAMERA_MSG_COMPRESSED_IMAGE callback. The branch after
the id compare is rewritten from bne to b with the same 16-bit Thumb
immediate, so every camera takes the path camera 0 and factory2 already
take: the unpadded JPEG goes to the callback and the encrypt block is
unreachable. The window below is unique in the module; the evidence for
the gate is htc-workbench evidence/camera2-jpeg-encryption-gate-107-20260928.

The rewrite is idempotent: a file that already holds the patched window
and none of the original is left unchanged. Any other count of either
window is an error.

Usage: plaintext_camera2_jpeg.py FILE...
"""

import sys
from pathlib import Path

# ldrb.w r6, [r0, 0x22c]; cmp r6, 2; bne +8; blx CheckMFG
OLD = bytes.fromhex("90f82c62022e04d1e8f724ed")
# ldrb.w r6, [r0, 0x22c]; cmp r6, 2; b +8; blx CheckMFG
NEW = bytes.fromhex("90f82c62022e04e0e8f724ed")
assert len(OLD) == len(NEW)


def ungate(path: Path) -> str:
    data = path.read_bytes()
    old, new = data.count(OLD), data.count(NEW)
    if (old, new) == (0, 1):
        return "already plaintext"
    if (old, new) != (1, 0):
        raise SystemExit(f"{path}: expected one gate window, found {old} old and {new} new")
    path.write_bytes(data.replace(OLD, NEW))
    return "patched"


def main(argv: list[str]) -> int:
    if not argv:
        raise SystemExit(__doc__)
    for name in argv:
        print(f"{name}: {ungate(Path(name))}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
