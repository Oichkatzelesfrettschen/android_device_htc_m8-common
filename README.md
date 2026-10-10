# HTC One M8 common tree, LineageOS 22.2 (Android 15)

Branch `lineage-22.2-m8` of this fork builds `lineage_m8-bp1a-userdebug`
together with the `lineage-22.2-m8` branches of the sibling forks (system,
frameworks, bionic, sepolicy, kernel and hardware projects) on the same account.

## Vendor blobs

HTC's proprietary files are not published here. Use TARKZiM's repository at
the commit this build was made with, then apply the camera fixups:

    <project path="vendor/htc" name="TARKZiM/proprietary_vendor_htc"
             remote="github" revision="0b4a15c042f8af8c7509906ec92a0c49f291b4ee" />

    repo sync vendor/htc
    sh device/htc/m8-common/apply-vendor-fixups.sh

`apply-vendor-fixups.sh` rewrites `libmmjpeg.so` and
`libmmcamera_faceproc.so` so they load without text relocations: this build's
SELinux policy gives the camera HAL and `mm-qcamera-daemon` no `execmod`, and
without the rewrite JPEG capture and face processing fail to load. The script
checks both results against the sha256 values pinned in it and in
`common-proprietary-files.txt`; `untextrel_camera.py` documents each patched
instruction. It also moves the camera daemon's socket: `mm-qcamera-daemon` and
`libmmcamera_interface.so` name it `/data/cam_socket%d`, a path in the `/data`
root that vendor domains may not create, and `retarget_camera_socket.py`
rewrites both to the same-length `/dev/socket/cam/%d`, which msm8974-common's
init creates and labels. `camera.vendor.msm8974.so` AES-encrypts every
camera 2 (depth subcam) JPEG through `libhtc_depthmap.so` before the
compressed-image callback, gated on camera id 2 and `ro.bootmode` other
than `factory2`; `plaintext_camera2_jpeg.py` rewrites the one-byte branch
condition after the camera id compare so camera 2 stills take camera 0's
unencrypted path. The OV2722 subcamera AEC curve in
`libtuning_aec_ov2722_subcam_zsl.so` runs out to 3201 or 6421 lines, so in dim
light camera 2 stretches its frames to 10.5 fps or less whatever its
`preview-fps-range`; `cap_subcam_aec_curve.py` caps every row at 1096 lines
with the gain scaled up to keep the exposure, and flattens the stock gain
field where it wraps at the curve's end, so camera 2 holds 30 fps and the
kernel's OV2722 frame lock can shorten its frame to camera 0's period. The
same rewrites run as a `blob_fixup` when blobs are extracted with
`extract-files.sh`.
