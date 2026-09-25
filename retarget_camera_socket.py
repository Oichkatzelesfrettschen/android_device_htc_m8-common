#!/usr/bin/env python3
"""Move the m8 camera daemon's socket path from /data to /dev/socket/cam.

mm-qcamera-daemon binds, and libmmcamera_interface.so connects to, a datagram
socket named by the format string /data/cam_socket%d. AOSP policy forbids a
vendor domain from creating files in the /data root, so both prebuilts are
rewritten to /dev/socket/cam/%d, which init creates for the camera user. The
two strings are 18 bytes long, so no offset in either ELF moves.

The rewrite is idempotent: a file that already holds the new path and none of
the old one is left unchanged. Any other count of either string is an error.

Usage: retarget_camera_socket.py FILE...
"""

import sys
from pathlib import Path

OLD = b"/data/cam_socket%d"
NEW = b"/dev/socket/cam/%d"
assert len(OLD) == len(NEW)


def retarget(path: Path) -> str:
    data = path.read_bytes()
    old, new = data.count(OLD), data.count(NEW)
    if (old, new) == (0, 1):
        return "already retargeted"
    if (old, new) != (1, 0):
        raise SystemExit(f"{path}: expected one {OLD!r}, found {old} old and {new} new")
    path.write_bytes(data.replace(OLD, NEW))
    return "retargeted"


def main(argv: list[str]) -> int:
    if not argv:
        raise SystemExit(__doc__)
    for name in argv:
        print(f"{name}: {retarget(Path(name))}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
