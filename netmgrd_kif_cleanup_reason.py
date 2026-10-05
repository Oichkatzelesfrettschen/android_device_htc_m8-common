#!/usr/bin/env python3
"""Let netmgrd's exit cleanup pass kif links that have no kernel interface.

netmgrd registers netmgr_kif_cleanup (Thumb, file offset 0xf020) with
crtbegin's atexit (0x3bc0, which calls __cxa_atexit) at 0x148fc, and its
SIGTERM handler calls exit(), so every SIGTERM runs the cleanup. The cleanup
calls netmgr_kif_reset_link (0xee4c) for links 0 through 0x21 with reset
reason 0x11; the loop bound is a constant and reads no property.
netmgr_kif_reset_link keeps the reason in r8 and reads it once, at 0xeeaa
(`cmp.w r8, #4`), after the link's interface init returns nonzero: reason 4,
NETMGR_MODEM_IS_EV, logs `Ignoring KIF init failure during
NETMGR_MODEM_IS_EV on link[%d]` and returns -1, and every other reason logs
`cannot init iface[%d]` and calls abort() at 0xef16. A link whose init
succeeds takes the same path under either reason.

The 34 links name rmnet0-7, rmnet_sdio0-7, rev_rmnet0-8 and
rev_rmnet_usb0-8. The msm8974 kernel registers rmnet0-7 and rev_rmnet0-8
only, so links 8-15 and 25-33 fail init and stock netmgrd turns every
SIGTERM into SIGABRT before its QMI clients release. File byte 0xf064 is the
immediate of the cleanup's `movs r1, #0x11`; writing 4 there makes the
cleanup log the absent links and run to its end. The evidence is
htc-workbench evidence/m8-netmgrd-sigterm-abort-126-20261004 and
evidence/m8-netmgrd-kif-cleanup-disasm.

The rewrite keys on sha256: the stock file is rewritten, a file already
holding the output is left unchanged, and any other file is an error.

Usage: netmgrd_kif_cleanup_reason.py FILE...
"""

import hashlib
import sys
from pathlib import Path

STOCK = "f1192bc3246eca85a956a49b1389d72c95f6522d16276fa633227fd7b83036fc"
PATCHED = "a7494eb814c57586a2859b4a0fd6ca3aa7d81ea7eae80d4795a95d9b6bf52beb"
# Thumb `movs r1, #imm8` encodes as imm8 then 0x21; the PIE's text LOAD
# segment maps file offset 0 at vaddr 0, so file and virtual offsets agree.
REASON_IMM = 0xF064
STOCK_INSN = b"\x11\x21"
NETMGR_MODEM_IS_EV = 4


def rewrite(path: Path) -> str:
    data = path.read_bytes()
    sha = hashlib.sha256(data).hexdigest()
    if sha == STOCK:
        out = bytearray(data)
        if out[REASON_IMM : REASON_IMM + 2] != STOCK_INSN:
            raise SystemExit(f"{path}: unexpected cleanup instruction")
        out[REASON_IMM] = NETMGR_MODEM_IS_EV
        if hashlib.sha256(out).hexdigest() != PATCHED:
            raise SystemExit(f"{path}: rewrite does not produce {PATCHED}")
        path.write_bytes(bytes(out))
        return "patched"
    if sha == PATCHED:
        return "already patched"
    raise SystemExit(
        f"{path}: sha256 {sha} is neither the stock nor the patched netmgrd"
    )


def main(argv: list[str]) -> int:
    if not argv:
        raise SystemExit(__doc__)
    for name in argv:
        print(f"{name}: {rewrite(Path(name))}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
