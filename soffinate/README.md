# soffinate

MIT-licensed clean-room HTC M8 S-OFF probe and attempt tool. Written
from the Ghidra decompilation of `/system/bin/dmagent` and from
disassembly of the hboot and tz partitions on a Sprint M8 running
HBOOT 3.19.0.0000 SHIP.

## Pathways

| Pathway | What it does | HBOOT 3.19 outcome |
|---|---|---|
| `-p dmagent` | Opens `/dev/socket/dmagent`, completes the 'w' challenge-response with zero-key AES and session key `"9900049945620100"`, sends encrypted `:DMD:CONFIG:EPST_ON`. The server-side handler does only `chmod(/data/data/com.google.android.epst/http_proxy, 0777)` and `chmod(.../rtsp_proxy, 0777)`. | Returns "OK" when the EPST directory and symlinks exist (soffinate.rc sets them up). The chmod is not an S-OFF write; HBOOT 3.19 reads from pg1fs and TZ, not from the uevent_helper permissions. |
| `-p misc` | Writes 0xFF to `/dev/block/platform/msm_sdcc.1/by-name/misc` offset 0x101C. | Byte persists but HBOOT 3.19 does not read S-OFF from misc:0x101C; the Sunshine note "flag is at misc:0x101C" was correct for HBOOT 2.x and earlier. No security_level change. |
| `-p pg1fs` | Writes `00 00 00 00 'H' 'T' 'C' 'S' 01 00 00 00 00 00 00 00` to `/dev/block/platform/msm_sdcc.1/by-name/pg1fs` offset 0x8400 (the PGFS "security" data block). | dd reports success and the open-fd readback shows the new bytes, but a fresh dd read after `drop_caches` shows the original `03 00 00 00 48 54 43 55 ...`. TrustZone eats the eMMC write at the hardware WP layer. No security_level change. |

The probe mode `-r` (default) reads the current state from
`/sys/module/scm/parameters/security_level`, pg1fs:0x8400 and
misc:0x101C and prints the three.

## Where the S-OFF flag actually lives

The strings in `hboot.img` (image_id=3, load 0x0F500000, 4 MB) show
HBOOT 3.19.0.0000 reads `pg1fs_security` (the PGFS "security" file in
pg1fs/mmcblk0p2) and calls `secure_get_security_level()`
(`scm_call(SCM_SVC_OEM=0xFE, TZ_HTC_SVC_GET_SECURITY_LEVEL=0x10)`).
The data block at pg1fs:0x8400 is 16 bytes:

    03 00 00 00  48 54 43 55  01 00 00 00  00 00 00 00
    ^level=3     ^"HTCU"      ^flag=1
    (S-ON)       (bootloader unlocked)

A level=0 "HTCS" value is the S-OFF state. The 32-bit value at
pg1fs:0x408 is the directory-entry checksum (`0xda2e2e41` on this
device) and must match the data block; the exact PGFS checksum is
not CRC-32 (ethernet poly), so writing a different data block without
updating a matching checksum would be rejected by HBOOT.

## Build

Lives under `device/htc/m8-common/soffinate`; the top-level `Android.bp`
and `m8-common.mk` include it as a `cc_binary` named `soffinate` in
`PRODUCT_PACKAGES`. The `soffinate.rc` runs at `post-fs-data` and sets
up the EPST directory so the dmagent 'w' handshake completes.

## Verifying

    adb shell /vendor/bin/soffinate -r
    adb shell /vendor/bin/soffinate -p all

The `-p all` run exits non-zero if no pathway persists a byte that
should flip the HBOOT S-OFF reading, which on HBOOT 3.19 is always.
