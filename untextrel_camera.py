#!/usr/bin/env python3
"""Remove the text relocations from two HTC One M8 camera prebuilts.

Usage: $PYTHON untextrel_camera.py IN [OUT]

IN is m8-common vendor/lib/libmmjpeg.so or vendor/lib/libmmcamera_faceproc.so,
identified by sha256. OUT defaults to IN (in-place). An input that already
matches the patched sha256 is left unchanged, so the script is idempotent and
safe as an extract-files blob_fixup.

Each text relocation in these blobs is an R_ARM_RELATIVE on a literal-pool
word that an ARM-mode function loads as the absolute address of read-only
data in the same PT_LOAD segment. The patch stores the PC-relative distance in
that literal instead and adds PC after the load, so the address is computed at
run time and the segment needs no write. The removed relocations leave
.rel.dyn, DT_RELSZ and DT_RELCOUNT; DT_TEXTREL and DF_TEXTREL are cleared.

libmmjpeg.so, jpegd_engine_sw_idct_4x4 / _8x8 (NEON, section idct_func):
ten literals point into RODataArea (0x310c0..0x31158), whose tables the
functions read as consecutive 16-byte blocks. One literal per function now
yields the first table address; the rest derive from it with ADD, and pairs
of two-register VLD1 become one four-register VLD1 over the same bytes. Each
function keeps its size and every general-purpose and NEON register holds the
same value at return as in the original.

libmmcamera_faceproc.so, crtbegin_so __on_dlclose at .text start: the
literal &__dso_handle (.bss 0x10dc40) becomes its PC-relative distance.

Only the Python standard library is used.
"""
import hashlib
import struct
import sys
from typing import Any

R_ARM_RELATIVE = 23
DT_NULL = 0
DT_REL = 17
DT_RELSZ = 18
DT_RELENT = 19
DT_TEXTREL = 22
DT_FLAGS = 30
DT_RELCOUNT = 0x6FFFFFFA
DF_TEXTREL = 0x4
PT_LOAD = 1
PT_DYNAMIC = 2
PF_X = 1
PF_W = 2

NOP = 0xE320F000

# Each spec, keyed by input sha256: instruction rewrites as
# (vaddr, original word, new word), PC-relative literals as
# (literal vaddr, original target, vaddr of the "add rX, pc, rX" that
# consumes it), literals that become unused (set to 0), and the vaddrs whose
# R_ARM_RELATIVE entries are removed.
SPECS = {
    "6a4d2b03119adc2bfef46819742e6a557f13d9883f5e8a308b6dbe1a71a75157": {
        "name": "libmmjpeg.so",
        "insns": [
            # jpegd_engine_sw_idct_4x4
            (0x2D220, 0xE59FC35C, 0xE08F3003),  # add r3, pc, r3
            (0x2D224, 0xF4230A4F, 0xF423024D),  # vld1.16 {d0-d3}, [r3]!
            (0x2D228, 0xF42C2A4F, 0xF423424F),  # vld1.16 {d4-d7}, [r3]
            (0x2D22C, 0xE59F3354, 0xE283C010),  # add r12, r3, #0x10
            (0x2D230, 0xE59FC354, NOP),
            (0x2D234, 0xF4234A4F, NOP),
            (0x2D238, 0xF42C6A4F, NOP),
            # jpegd_engine_sw_idct_8x8
            (0x2D2E8, 0xE59F62A4, 0xE08F5005),  # add r5, pc, r5
            (0x2D2EC, 0xE59F72A4, 0xE2856010),  # add r6, r5, #0x10
            (0x2D2F0, 0xE59F82A4, 0xE2857020),  # add r7, r5, #0x20
            (0x2D2F4, 0xE59F92A4, 0xE2858030),  # add r8, r5, #0x30
            (0x2D2F8, 0xF4250A4F, 0xF425024F),  # vld1.16 {d0-d3}, [r5]
            (0x2D2FC, 0xF4262A4F, 0xE2859040),  # add r9, r5, #0x40
            (0x2D4E4, 0xE59F50B8, 0xE2855050),  # add r5, r5, #0x50
        ],
        # The loads that stay: 0x2d21c "ldr r3, [pc, #0x35c]" and
        # 0x2d2e4 "ldr r5, [pc, #0x2a4]".
        "keep": [(0x2D21C, 0xE59F335C), (0x2D2E4, 0xE59F52A4)],
        "pcrel": [(0x2D580, 0x31118, 0x2D220), (0x2D590, 0x310C0, 0x2D2E8)],
        "unused": [
            (0x2D584, 0x31128), (0x2D588, 0x31138), (0x2D58C, 0x31148),
            (0x2D594, 0x310D0), (0x2D598, 0x310E0), (0x2D59C, 0x310F0),
            (0x2D5A0, 0x31100), (0x2D5A4, 0x31110),
        ],
    },
    "10f933fda9a8883485b53c68136a26a4b302e5ee26982b38f3246374d2a40b34": {
        "name": "libmmcamera_faceproc.so",
        "insns": [
            (0x75F4, 0xE28F0004, 0xE59F0004),  # ldr r0, [pc, #4]
            (0x75F8, 0xE5900000, 0xE08F0000),  # add r0, pc, r0
        ],
        # 0x75fc "b __cxa_finalize" stays.
        "keep": [],
        "pcrel": [(0x7600, 0x10DC40, 0x75F8)],
        "unused": [],
    },
}
OUT_SHA256 = {
    "6a4d2b03119adc2bfef46819742e6a557f13d9883f5e8a308b6dbe1a71a75157":
        "a5231d87bd828046c7751232a24df23d7439828cf9663fb445f331b7cb11ec6c",
    "10f933fda9a8883485b53c68136a26a4b302e5ee26982b38f3246374d2a40b34":
        "3bb2d74fba0481402a8fdbccfc5c425850f5a784c859cf9ca6e442ac4ed11751",
}


class Elf32:
    """Little-endian ELF32 view with vaddr access through PT_LOAD."""

    def __init__(self, data: bytearray) -> None:
        if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
            raise SystemExit("not a little-endian ELF32 file")
        self.d = data
        (self.phoff, self.shoff) = struct.unpack_from("<II", data, 0x1C)
        (self.phentsize, self.phnum, self.shentsize, self.shnum) = \
            struct.unpack_from("<HHHH", data, 0x2A)
        self.phdrs = []
        for i in range(self.phnum):
            self.phdrs.append(struct.unpack_from(
                "<8I", data, self.phoff + i * self.phentsize))

    def off(self, vaddr: int) -> int:
        for p_type, p_off, p_vaddr, _pa, p_filesz, _m, _f, _a in self.phdrs:
            if p_type == PT_LOAD and p_vaddr <= vaddr < p_vaddr + p_filesz:
                return int(vaddr - p_vaddr + p_off)
        raise SystemExit(f"vaddr {vaddr:#x} is outside every PT_LOAD")

    def word(self, vaddr: int) -> int:
        return int(struct.unpack_from("<I", self.d, self.off(vaddr))[0])

    def put(self, vaddr: int, value: int) -> None:
        struct.pack_into("<I", self.d, self.off(vaddr), value & 0xFFFFFFFF)

    def text_ranges(self) -> list[tuple[int, int]]:
        return [(p[2], p[2] + p[5]) for p in self.phdrs
                if p[0] == PT_LOAD and p[6] & PF_X and not p[6] & PF_W]

    def dynamic(self) -> tuple[int, int]:
        for p in self.phdrs:
            if p[0] == PT_DYNAMIC:
                return p[1], p[4] // 8
        raise SystemExit("no PT_DYNAMIC")

    def dyn_entries(self) -> list[list[int]]:
        off, count = self.dynamic()
        out = []
        for i in range(count):
            tag, val = struct.unpack_from("<iI", self.d, off + 8 * i)
            out.append([tag, val])
            if tag == DT_NULL:
                break
        return out

    def write_dyn(self, entries: list[list[int]]) -> None:
        off, count = self.dynamic()
        if len(entries) > count:
            raise SystemExit("dynamic array grew")
        for i in range(count):
            tag, val = entries[i] if i < len(entries) else (DT_NULL, 0)
            struct.pack_into("<iI", self.d, off + 8 * i, tag, val)

    def section_by_addr(self, addr: int) -> int:
        for i in range(self.shnum):
            base = self.shoff + i * self.shentsize
            sh_addr = struct.unpack_from("<I", self.d, base + 12)[0]
            sh_type = struct.unpack_from("<I", self.d, base + 4)[0]
            if sh_addr == addr and sh_type == 9:  # SHT_REL
                return int(base)
        raise SystemExit(f"no SHT_REL section at {addr:#x}")


def dyn_get(entries: list[list[int]], tag: int) -> int | None:
    for t, v in entries:
        if t == tag:
            return v
    return None


def rel_entries(elf: Elf32, entries: list[list[int]]) -> list[tuple[int, int]]:
    rel = dyn_get(entries, DT_REL)
    relsz = dyn_get(entries, DT_RELSZ)
    if rel is None or relsz is None or dyn_get(entries, DT_RELENT) != 8:
        raise SystemExit("unexpected DT_REL layout")
    base = elf.off(rel)
    return [struct.unpack_from("<II", elf.d, base + 8 * i)
            for i in range(relsz // 8)]


def patch(data: bytearray, spec: dict[str, Any]) -> None:
    elf = Elf32(data)
    for vaddr, old, new in spec["insns"]:
        got = elf.word(vaddr)
        if got != old:
            raise SystemExit(f"{vaddr:#x}: {got:#010x}, expected {old:#010x}")
        elf.put(vaddr, new)
    for vaddr, old in spec["keep"]:
        if elf.word(vaddr) != old:
            raise SystemExit(f"{vaddr:#x}: kept instruction differs")
    drop = set()
    for lit, target, add_vaddr in spec["pcrel"]:
        if elf.word(lit) != target:
            raise SystemExit(f"literal {lit:#x} does not hold {target:#x}")
        # ARM-mode PC reads as the instruction address plus 8.
        elf.put(lit, target - (add_vaddr + 8))
        drop.add(lit)
    for lit, target in spec["unused"]:
        if elf.word(lit) != target:
            raise SystemExit(f"literal {lit:#x} does not hold {target:#x}")
        elf.put(lit, 0)
        drop.add(lit)

    dyn = elf.dyn_entries()
    rels = rel_entries(elf, dyn)
    kept = [r for r in rels if r[0] not in drop]
    removed = [r for r in rels if r[0] in drop]
    if sorted(r[0] for r in removed) != sorted(drop):
        raise SystemExit("relocation set differs from the literal set")
    if any(r[1] & 0xFF != R_ARM_RELATIVE or r[1] >> 8 for r in removed):
        raise SystemExit("a removed relocation is not a local R_ARM_RELATIVE")
    relcount = dyn_get(dyn, DT_RELCOUNT)
    if relcount is not None:
        lead = rels[:relcount]
        if any(r[1] & 0xFF != R_ARM_RELATIVE for r in lead):
            raise SystemExit("DT_RELCOUNT prefix is not all R_ARM_RELATIVE")
        if any(rels.index(r) >= relcount for r in removed):
            raise SystemExit("a removed relocation lies past DT_RELCOUNT")
    rel_addr = dyn_get(dyn, DT_REL)
    assert rel_addr is not None
    base = elf.off(rel_addr)
    for i in range(len(rels)):
        r_off, r_info = kept[i] if i < len(kept) else (0, 0)
        struct.pack_into("<II", elf.d, base + 8 * i, r_off, r_info)
    shdr = elf.section_by_addr(rel_addr)
    struct.pack_into("<I", elf.d, shdr + 20, 8 * len(kept))

    new_dyn = []
    for tag, val in dyn:
        if tag == DT_TEXTREL:
            continue
        if tag == DT_RELSZ:
            val = 8 * len(kept)
        elif tag == DT_RELCOUNT:
            val -= len(removed)
        elif tag == DT_FLAGS:
            val &= ~DF_TEXTREL
        new_dyn.append([tag, val])
    elf.write_dyn(new_dyn)
    check(elf)


def check(elf: Elf32) -> None:
    """Refuse output that still carries a text relocation or TEXTREL flag."""
    dyn = elf.dyn_entries()
    if dyn_get(dyn, DT_TEXTREL) is not None:
        raise SystemExit("DT_TEXTREL still present")
    if (dyn_get(dyn, DT_FLAGS) or 0) & DF_TEXTREL:
        raise SystemExit("DF_TEXTREL still set")
    ranges = elf.text_ranges()
    tables = [rel_entries(elf, dyn)]
    jmprel, pltsz = dyn_get(dyn, 23), dyn_get(dyn, 2)
    if jmprel is not None and pltsz:
        o = elf.off(jmprel)
        tables.append([struct.unpack_from("<II", elf.d, o + 8 * i)
                       for i in range(pltsz // 8)])
    for table in tables:
        for r_off, _info in table:
            if any(lo <= r_off < hi for lo, hi in ranges):
                raise SystemExit(f"relocation at {r_off:#x} targets text")


def main(argv: list[str]) -> int:
    if len(argv) not in (2, 3):
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 2
    src = argv[1]
    dst = argv[2] if len(argv) == 3 else src
    with open(src, "rb") as fh:
        data = bytearray(fh.read())
    digest = hashlib.sha256(data).hexdigest()
    if digest in OUT_SHA256.values():
        if dst != src:
            with open(dst, "wb") as fh:
                fh.write(data)
        return 0
    spec = SPECS.get(digest)
    if spec is None:
        print(f"{src}: unknown input sha256 {digest}", file=sys.stderr)
        return 1
    patch(data, spec)
    out = hashlib.sha256(data).hexdigest()
    want = OUT_SHA256[digest]
    if out != want:
        print(f"{src}: output sha256 {out}, expected {want}", file=sys.stderr)
        return 1
    with open(dst, "wb") as fh:
        fh.write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
