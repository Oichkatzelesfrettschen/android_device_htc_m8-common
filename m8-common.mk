#
# Copyright (C) 2015-2016 The CyanogenMod Project
#               2017-2020 The LineageOS Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

$(call inherit-product, $(SRC_TARGET_DIR)/product/product_launched_with_k.mk)

# Release config: RELEASE_PACKAGE_NFC_STACK=NfcNci for products inheriting this
# file, so the platform NFC stack replaces com.android.nfcservices.
PRODUCT_RELEASE_CONFIG_MAPS += device/htc/m8-common/release/release_config_map.textproto
$(call inherit-product, vendor/htc/m8-common/m8-common-vendor.mk)

# Inherit from msm8974-common
$(call inherit-product, device/htc/msm8974-common/msm8974-common.mk)

# System properties
-include $(LOCAL_PATH)/system_prop.mk

# Overlay
DEVICE_PACKAGE_OVERLAYS += \
    $(LOCAL_PATH)/overlay \
    $(LOCAL_PATH)/overlay-lineage

# Permissions
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.consumerir.xml:system/vendor/etc/permissions/android.hardware.consumerir.xml \
    frameworks/native/data/etc/android.hardware.nfc.xml:system/vendor/etc/permissions/android.hardware.nfc.xml \
    frameworks/native/data/etc/android.hardware.telephony.cdma.xml:system/vendor/etc/permissions/android.hardware.telephony.cdma.xml \
    frameworks/native/data/etc/com.android.nfc_extras.xml:system/vendor/etc/permissions/com.android.nfc_extras.xml \
    frameworks/native/data/etc/com.nxp.mifare.xml:system/vendor/etc/permissions/com.nxp.mifare.xml \
    $(LOCAL_PATH)/configs/com.htc.software.market.xml:system/vendor/etc/permissions/com.htc.software.market.xml

# Camera
PRODUCT_PACKAGES += \
    camera.msm8974

# Filesystem management tools
PRODUCT_PACKAGES += \
    resize2fs_static

# Init
PRODUCT_PACKAGES += \
    init.target.rc

# IMS: PhhIms is a userspace SIP/IMS MmTel service, because the legacy
# Qualcomm RIL carries no IMS. It needs SIM AKA over SIM_AUTHENTICATION and an
# ims APN data call from the RIL, and IPsec transport mode from the kernel.
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.telephony.ims.xml:system/vendor/etc/permissions/android.hardware.telephony.ims.xml

PRODUCT_PACKAGES += \
    PhhIms

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/privapp-permissions-me.phh.ims.xml:$(TARGET_COPY_OUT_SYSTEM)/etc/permissions/privapp-permissions-me.phh.ims.xml

# IRDA
PRODUCT_PACKAGES += \
    android.hardware.ir@1.0-impl \
    android.hardware.ir@1.0-service

PRODUCT_BOOT_JARS += \
    htcirlibs

# NFC: NXP PN544 over ETSI HCI. The Nfc app (nxp DeviceHost) loads libnfc_jni,
# which drives libnfc and opens /dev/pn544 through the nfc.msm8974 hw_module_t.
PRODUCT_PACKAGES += \
    com.android.nfc_extras \
    libnfc \
    libnfc_jni \
    libnfc_ndef \
    Nfc \
    nfc.msm8974 \
    Tag

