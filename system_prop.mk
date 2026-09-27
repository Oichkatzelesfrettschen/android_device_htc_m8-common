#
# Common system properties for m8 and m8d
#

# Camera
PRODUCT_PROPERTY_OVERRIDES += \
    persist.camera.maxopen=3

# The 2014 HAL1 camera delivers recording frames through
# CameraSource::dataCallbackTimestamp as metadata whose pHandle is a pointer
# valid only in the camera process; the out-of-process encoder faults
# dereferencing it. This property (frameworks/av, PR #4) makes
# initWithCameraAccess skip the buffer-queue and metadata probes and use
# VIDEO_BUFFER_MODE_DATA_CALLBACK_YUV instead, shipping real pixels through
# shared memory.
PRODUCT_PROPERTY_OVERRIDES += \
    ro.camera.record_force_yuv=true

# Perf
PRODUCT_PROPERTY_OVERRIDES += \
    ro.vendor.extension_library=/vendor/lib/libqti-perfd-client.so

# Vendor security patch level
PRODUCT_PROPERTY_OVERRIDES += \
    ro.lineage.build.vendor_security_patch=2016-07-01
