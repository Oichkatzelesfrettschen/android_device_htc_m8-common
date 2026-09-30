#!/bin/sh
# Apply the m8 build's changes to a vendor/htc checkout of
# TARKZiM/proprietary_vendor_htc (see README.md).
#
# libmmjpeg.so and libmmcamera_faceproc.so ship with text relocations, which
# need SELinux execmod in the camera HAL and mm-qcamera-daemon; the build's
# policy grants none. untextrel_camera.py rewrites both prebuilts so they load
# without text relocations. It keys each input by sha256 and leaves an already
# rewritten file unchanged. mm-qcamera-daemon and libmmcamera_interface.so
# name the daemon's socket /data/cam_socket%d, a path in the /data root that
# vendor domains may not create; retarget_camera_socket.py moves it to
# /dev/socket/cam/%d in both and leaves a retargeted file unchanged.
# camera.vendor.msm8974.so AES-encrypts every camera 2 JPEG through
# libhtc_depthmap.so's af_aes_cbc_encrypt before the compressed-image
# callback; plaintext_camera2_jpeg.py turns the camera id branch in
# QCameraPostProcessor::processJpegEvt into the path camera 0 takes and
# leaves a patched file unchanged. The OV2722 subcamera AEC curve runs out to
# 3201 or 6421 lines, so camera 2 stretches its frames to 10.5 fps or less in
# dim light; cap_subcam_aec_curve.py caps it at 1096 lines, moving the rest of
# the exposure into gain, so camera 2 holds 30 fps and the kernel frame lock
# can shorten its frame to camera 0's period. Every rewrite leaves an already
# rewritten file unchanged, so this script is idempotent.
#
# Usage: sh apply-vendor-fixups.sh [VENDOR_HTC_DIR]
#   VENDOR_HTC_DIR defaults to vendor/htc of the tree holding this directory.
set -eu
PYTHON=${PYTHON:-python3}

here=$(cd "$(dirname "$0")" && pwd)
vendor=${1:-$here/../../../vendor/htc}
lib=$vendor/m8-common/proprietary/vendor/lib
bin=$vendor/m8-common/proprietary/vendor/bin

check() {
	sum=$(sha256sum "$1" | cut -d' ' -f1)
	[ "$sum" = "$2" ] || { echo "$1: sha256 $sum, expected $2" >&2; exit 1; }
}

"$PYTHON" "$here/untextrel_camera.py" "$lib/libmmjpeg.so"
"$PYTHON" "$here/untextrel_camera.py" "$lib/libmmcamera_faceproc.so"
check "$lib/libmmjpeg.so" a5231d87bd828046c7751232a24df23d7439828cf9663fb445f331b7cb11ec6c
check "$lib/libmmcamera_faceproc.so" 3bb2d74fba0481402a8fdbccfc5c425850f5a784c859cf9ca6e442ac4ed11751
"$PYTHON" "$here/retarget_camera_socket.py" "$bin/mm-qcamera-daemon" "$lib/libmmcamera_interface.so"
check "$bin/mm-qcamera-daemon" 18f70fef503c6eedbe4eb7cd5cd80d3e7edf1bfe54af1bd40a5f860a4c365ed0
check "$lib/libmmcamera_interface.so" 161139c41436a8b008a204e0deeab4e37787d635b7f72c96a9d8e0d4c26404e1
"$PYTHON" "$here/plaintext_camera2_jpeg.py" "$lib/hw/camera.vendor.msm8974.so"
check "$lib/hw/camera.vendor.msm8974.so" b40a977ddef751b2be08fa7ed924ae5743764ab6d9cb33cd2fedc30caedee3cb
"$PYTHON" "$here/cap_subcam_aec_curve.py" "$lib/libtuning_aec_ov2722_subcam_zsl.so"
check "$lib/libtuning_aec_ov2722_subcam_zsl.so" 2c849901f387136d1bd4d012ec16d31bb70b648e07b87eb98abcfd1357e152fc
echo "vendor/htc camera prebuilts rewritten"
