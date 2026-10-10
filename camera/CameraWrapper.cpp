/*
 * Copyright (C) 2012, The CyanogenMod Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
* @file CameraWrapper.cpp
*
* This file wraps a vendor camera module.
*
*/

#define LOG_NDEBUG 0

#define LOG_TAG "CameraWrapper"
#include <cutils/log.h>
#include <cutils/properties.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <utils/threads.h>
#include <utils/String8.h>
#include <hardware/hardware.h>
#include <hardware/camera.h>
#include <camera/Camera.h>
#include <camera/CameraParameters.h>

static android::Mutex gCameraWrapperLock;
static camera_module_t *gVendorModule = 0;
static const camera_module_callbacks_t *gModuleCallbacks = NULL;
static int gOpenCameraCount = 0;
static bool gTorchEnabled = false;

/* msm8974-m8-common.dtsi:160-168 names torch_0 and caps it at 200;
 * tps61310_flashlight.c:1436-1468 maps its brightness to torch current. */
static const char kTorchBrightness[] = "/sys/class/leds/torch_0/brightness";

static void camera_notify_torch_status(int status)
{
    if (gModuleCallbacks && gModuleCallbacks->torch_mode_status_change)
        gModuleCallbacks->torch_mode_status_change(gModuleCallbacks, "0", status);
}

static int camera_write_torch(bool enabled)
{
    const char *value = enabled ? "200\n" : "0\n";
    const size_t length = strlen(value);
    int fd = open(kTorchBrightness, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;

    ssize_t written;
    do {
        written = write(fd, value, length);
    } while (written < 0 && errno == EINTR);
    int result = written == static_cast<ssize_t>(length) ? 0 :
            (written < 0 ? -errno : -EIO);
    if (close(fd) < 0 && result == 0)
        result = -errno;
    return result;
}

/*
 * Each exposed camera id owns one set of callback trampolines, handed to its
 * vendor device in set_callbacks, and each trampoline forwards to the client
 * callbacks and client cookie recorded for that id, so cameras 0 and 2 stream
 * to their own clients at the same time. The vendor HAL's cookie never selects
 * the client: it passes an object of its own to get_memory, while the camera
 * provider's HAL1 get_memory (CameraDevice::sGetMemory) reads its cookie as
 * the CameraDevice that registers the buffer.
 */
struct client_callbacks {
    camera_notify_callback notify;
    camera_data_callback data;
    camera_data_timestamp_callback data_timestamp;
    camera_request_memory get_memory;
    void *user;
};

#define MAX_WRAPPED_CAMERAS 3
static client_callbacks gClients[MAX_WRAPPED_CAMERAS];

static char **fixed_set_params = NULL;
static int gFixedSetParamsCount = 0;

static int camera_device_open(const hw_module_t *module, const char *name,
        hw_device_t **device);
static int camera_get_number_of_cameras(void);
static int camera_get_camera_info(int camera_id, struct camera_info *info);
static int camera_module_set_callbacks(const camera_module_callbacks_t *callbacks);
static int camera_set_torch_mode(const char *camera_id, bool enabled);

static struct hw_module_methods_t camera_module_methods = {
    .open = camera_device_open
};

camera_module_t HAL_MODULE_INFO_SYM = {
    .common = {
         .tag = HARDWARE_MODULE_TAG,
         .module_api_version = CAMERA_MODULE_API_VERSION_2_4,
         .hal_api_version = HARDWARE_HAL_API_VERSION,
         .id = CAMERA_HARDWARE_MODULE_ID,
         .name = "M8 Camera Wrapper",
         .author = "The CyanogenMod Project",
         .methods = &camera_module_methods,
         .dso = NULL, /* remove compilation warnings */
         .reserved = {0}, /* remove compilation warnings */
    },
    .get_number_of_cameras = camera_get_number_of_cameras,
    .get_camera_info = camera_get_camera_info,
    .set_callbacks = camera_module_set_callbacks,
    .get_vendor_tag_ops = NULL, /* remove compilation warnings */
    .open_legacy = NULL, /* remove compilation warnings */
    .set_torch_mode = camera_set_torch_mode,
    .init = NULL, /* remove compilation warnings */
    .reserved = {0}, /* remove compilation warnings */
};

typedef struct wrapper_camera_device {
    camera_device_t base;
    int id;
    camera_device_t *vendor;
} wrapper_camera_device_t;

#define VENDOR_CALL(device, func, ...) ({ \
    wrapper_camera_device_t *__wrapper_dev = (wrapper_camera_device_t*) device; \
    __wrapper_dev->vendor->ops->func(__wrapper_dev->vendor, ##__VA_ARGS__); \
})

#define CAMERA_ID(device) (((wrapper_camera_device_t *)(device))->id)

static int check_vendor_module()
{
    int rv = 0;
    ALOGV("%s", __FUNCTION__);

    if (gVendorModule)
        return 0;

    rv = hw_get_module_by_class("camera", "vendor",
            (const hw_module_t**)&gVendorModule);
    if (rv)
        ALOGE("failed to open vendor camera module");
    return rv;
}

static char *camera_fixup_getparams(const char *settings)
{
    int rotation = 0;

    android::CameraParameters params;
    params.unflatten(android::String8(settings));

#ifdef LOG_PARAMETERS
    ALOGV("%s: original parameters:", __FUNCTION__);
    params.dump();
#endif

    if (params.get(android::CameraParameters::KEY_ROTATION)) {
        rotation = atoi(params.get(android::CameraParameters::KEY_ROTATION));
    }

    params.set("preview-frame-rate-mode", "frame-rate-fixed");

    /* Fix rotation missmatch */
    switch (rotation) {
        case 90:
            params.set(android::CameraParameters::KEY_ROTATION, "0");
            break;
        case 180:
            params.set(android::CameraParameters::KEY_ROTATION, "90");
            break;
        case 270:
            params.set(android::CameraParameters::KEY_ROTATION, "180");
            break;
        default:
            break;
    }

#ifdef LOG_PARAMETERS
    ALOGV("%s: fixed parameters:", __FUNCTION__);
    params.dump();
#endif

    android::String8 strParams = params.flatten();
    char *ret = strdup(strParams.c_str());

    return ret;
}

/*
 * camera.vendor.msm8974.so routes every preview fps range update through
 * QCameraParameters::setPreviewFpsRange(int, int), which replaces the range
 * with [N, N] fps when persist.debug.set.fixedfps holds a nonzero N. The HAL
 * capability tables lack the fixed ranges a recording asks for: in
 * video-mode 2 (1080p60) the OV4688 runs its 60 fps mode but the HAL commits
 * the [30, 30] entry, and under recording-hint the HAL offers only
 * [15, 15] and [20, 30], so a [30, 30] request runs [20, 30] and auto
 * exposure lengthens frames to 41 ms in dim light. The wrapper holds the
 * property at 60 while a camera's parameters carry video-mode 2, at N while
 * a recording-hint camera requests a fixed [N, N] range, and clears it
 * otherwise. The fixup runs before the vendor set_parameters call, so the HAL
 * reads the held value in the same update. The property persists across
 * provider restarts, so the first open clears it before the vendor HAL reads
 * it.
 */
static const char kFixedFpsProperty[] = "persist.debug.set.fixedfps";
static const char kHtcVideoMode[] = "video-mode";
static const char kHtcVideoMode60Fps[] = "2";
static const int kHtcVideoMode60FpsRate = 60;
static android::Mutex gFixedFpsLock;
static int gFixedFpsOwner = -1;
static int gFixedFpsValue = 0;

static void camera_write_fixed_fps(int fps)
{
    char value[12];
    snprintf(value, sizeof(value), "%d", fps);
    if (property_set(kFixedFpsProperty, value))
        ALOGE("%s: setting %s to %s failed", __FUNCTION__, kFixedFpsProperty, value);
}

/* The fixed rate these parameters need, or 0 for none. */
static int camera_fixed_fps_for(const android::CameraParameters &params, bool isVideo)
{
    const char *videoMode = params.get(kHtcVideoMode);
    if (videoMode && !strcmp(videoMode, kHtcVideoMode60Fps))
        return kHtcVideoMode60FpsRate;
    if (!isVideo)
        return 0;

    int minFps = 0, maxFps = 0;
    params.getPreviewFpsRange(&minFps, &maxFps);
    if (minFps > 0 && minFps == maxFps && maxFps % 1000 == 0)
        return maxFps / 1000;
    return 0;
}

static void camera_update_fixed_fps(int id, int fps)
{
    android::Mutex::Autolock lock(gFixedFpsLock);

    if (fps > 0 && (gFixedFpsOwner != id || gFixedFpsValue != fps)) {
        camera_write_fixed_fps(fps);
        gFixedFpsOwner = id;
        gFixedFpsValue = fps;
    } else if (fps == 0 && gFixedFpsOwner == id) {
        camera_write_fixed_fps(0);
        gFixedFpsOwner = -1;
        gFixedFpsValue = 0;
    }
}

static void camera_reset_fixed_fps(void)
{
    android::Mutex::Autolock lock(gFixedFpsLock);

    if (property_get_int32(kFixedFpsProperty, 0) != 0)
        camera_write_fixed_fps(0);
    gFixedFpsOwner = -1;
    gFixedFpsValue = 0;
}

static char *camera_fixup_setparams(int id, const char *settings)
{
    bool isVideo = false;

    android::CameraParameters params;
    params.unflatten(android::String8(settings));

#ifdef LOG_PARAMETERS
    ALOGV("%s: original parameters:", __FUNCTION__);
    params.dump();
#endif

    if (params.get(android::CameraParameters::KEY_RECORDING_HINT)) {
        isVideo = !strcmp(params.get(android::CameraParameters::KEY_RECORDING_HINT), "true");
    }

    camera_update_fixed_fps(id, camera_fixed_fps_for(params, isVideo));

    /* Enable fixed fps mode */
    params.set("preview-frame-rate-mode", "frame-rate-fixed");

    if (isVideo && id == 1) {
        /* Front camera only supports infinity */
        params.set(android::CameraParameters::KEY_FOCUS_MODE, "infinity");
    }

#ifdef LOG_PARAMETERS
    ALOGV("%s: fixed parameters:", __FUNCTION__);
    params.dump();
#endif

    android::String8 strParams = params.flatten();
    if (fixed_set_params[id])
        free(fixed_set_params[id]);
    fixed_set_params[id] = strdup(strParams.c_str());
    char *ret = fixed_set_params[id];

    return ret;
}

/*******************************************************************
 * implementation of camera_device_ops functions
 *******************************************************************/

static int camera_set_preview_window(struct camera_device *device,
        struct preview_stream_ops *window)
{
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    return VENDOR_CALL(device, set_preview_window, window);
}

template <int N>
static void camera_notify_cb(int32_t msg_type, int32_t ext1, int32_t ext2, void *user __unused) {
    const client_callbacks &c = gClients[N];
    c.notify(msg_type, ext1, ext2, c.user);
}

/*
 * camera.vendor.msm8974.so and libcameraface.so pass frame metadata in HTC's
 * camera.h layout: a 16-byte header with the face array pointer at offset 8,
 * and 0x1b0-byte face records whose first 48 bytes hold the AOSP fields in
 * AOSP order (rect, score, id, left_eye, right_eye, mouth). The framework
 * reads this tree's camera_face_t, so every metadata pointer is rebuilt with
 * the AOSP fields copied and the vendor extension fields zeroed.
 */
struct htc_camera_frame_metadata {
    int32_t number_of_faces;
    int32_t reserved0;
    const uint8_t *faces;
    void *reserved1;
};

struct htc_camera_face_head {
    int32_t rect[4];
    int32_t score;
    int32_t id;
    int32_t left_eye[2];
    int32_t right_eye[2];
    int32_t mouth[2];
};

static const size_t kHtcCameraFaceSize = 0x1b0;

/* The HAL advertises max-num-detected-faces-hw=10, and libcameraface sizes
 * its face arrays for ten records. */
static const int kMaxFaces = 10;

static void camera_forward_data(const client_callbacks &c, int32_t msg_type,
        const camera_memory_t *data, unsigned int index, camera_frame_metadata_t *metadata) {
    if (metadata == NULL) {
        c.data(msg_type, data, index, NULL, c.user);
        return;
    }

    const htc_camera_frame_metadata *htc =
            reinterpret_cast<const htc_camera_frame_metadata *>(metadata);
    camera_face_t faces[kMaxFaces];
    int count = htc->faces != NULL ? htc->number_of_faces : 0;
    if (count < 0) {
        count = 0;
    } else if (count > kMaxFaces) {
        ALOGW("%s: %d faces from the HAL, forwarding %d", __FUNCTION__, count, kMaxFaces);
        count = kMaxFaces;
    }
    memset(faces, 0, sizeof(faces));
    for (int i = 0; i < count; i++) {
        htc_camera_face_head head;
        memcpy(&head, htc->faces + i * kHtcCameraFaceSize, sizeof(head));
        camera_face_t &face = faces[i];
        for (int k = 0; k < 4; k++) {
            face.rect[k] = head.rect[k];
        }
        face.score = head.score;
        face.id = head.id;
        for (int k = 0; k < 2; k++) {
            face.left_eye[k] = head.left_eye[k];
            face.right_eye[k] = head.right_eye[k];
            face.mouth[k] = head.mouth[k];
        }
    }

    /* The framework copies the faces before this returns. */
    camera_frame_metadata_t aosp;
    aosp.number_of_faces = count;
    aosp.faces = faces;
    c.data(msg_type, data, index, &aosp, c.user);
}

template <int N>
static void camera_data_cb(int32_t msg_type, const camera_memory_t *data, unsigned int index,
        camera_frame_metadata_t *metadata, void *user __unused) {
    camera_forward_data(gClients[N], msg_type, data, index, metadata);
}

template <int N>
static void camera_data_cb_timestamp(nsecs_t timestamp, int32_t msg_type,
        const camera_memory_t *data, unsigned index, void *user __unused) {
    const client_callbacks &c = gClients[N];
    c.data_timestamp(timestamp, msg_type, data, index, c.user);
}

template <int N>
static camera_memory_t *camera_get_memory(int fd, size_t buf_size,
        uint_t num_bufs, void *user __unused) {
    const client_callbacks &c = gClients[N];
    return c.get_memory(fd, buf_size, num_bufs, c.user);
}

static const camera_notify_callback kNotifyCbs[MAX_WRAPPED_CAMERAS] = {
    camera_notify_cb<0>, camera_notify_cb<1>, camera_notify_cb<2> };
static const camera_data_callback kDataCbs[MAX_WRAPPED_CAMERAS] = {
    camera_data_cb<0>, camera_data_cb<1>, camera_data_cb<2> };
static const camera_data_timestamp_callback kDataTimestampCbs[MAX_WRAPPED_CAMERAS] = {
    camera_data_cb_timestamp<0>, camera_data_cb_timestamp<1>, camera_data_cb_timestamp<2> };
static const camera_request_memory kGetMemoryCbs[MAX_WRAPPED_CAMERAS] = {
    camera_get_memory<0>, camera_get_memory<1>, camera_get_memory<2> };

static void camera_set_callbacks(struct camera_device *device,
        camera_notify_callback notify_cb,
        camera_data_callback data_cb,
        camera_data_timestamp_callback data_cb_timestamp,
        camera_request_memory get_memory,
        void *user)
{
    if (!device)
        return;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    int id = CAMERA_ID(device);
    if (id < 0 || id >= MAX_WRAPPED_CAMERAS) {
        ALOGE("%s: camera %d has no callback slot", __FUNCTION__, id);
        return;
    }
    gClients[id] = { notify_cb, data_cb, data_cb_timestamp, get_memory, user };

    VENDOR_CALL(device, set_callbacks, kNotifyCbs[id], kDataCbs[id],
            kDataTimestampCbs[id], kGetMemoryCbs[id], user);
}

static void camera_enable_msg_type(struct camera_device *device,
        int32_t msg_type)
{
    if (!device)
        return;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    VENDOR_CALL(device, enable_msg_type, msg_type);
}

static void camera_disable_msg_type(struct camera_device *device,
        int32_t msg_type)
{
    if (!device)
        return;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    VENDOR_CALL(device, disable_msg_type, msg_type);
}

static int camera_msg_type_enabled(struct camera_device *device,
        int32_t msg_type)
{
    if (!device)
        return 0;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    return VENDOR_CALL(device, msg_type_enabled, msg_type);
}

static int camera_start_preview(struct camera_device *device)
{
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    return VENDOR_CALL(device, start_preview);
}

static void camera_stop_preview(struct camera_device *device)
{
    if (!device)
        return;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    VENDOR_CALL(device, stop_preview);
}

static int camera_preview_enabled(struct camera_device *device)
{
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    return VENDOR_CALL(device, preview_enabled);
}

static int camera_store_meta_data_in_buffers(struct camera_device *device,
        int enable)
{
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    return VENDOR_CALL(device, store_meta_data_in_buffers, enable);
}

static int camera_start_recording(struct camera_device *device)
{
    if (!device)
        return EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    return VENDOR_CALL(device, start_recording);
}

static void camera_stop_recording(struct camera_device *device)
{
    if (!device)
        return;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    VENDOR_CALL(device, stop_recording);
}

static int camera_recording_enabled(struct camera_device *device)
{
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    return VENDOR_CALL(device, recording_enabled);
}

static void camera_release_recording_frame(struct camera_device *device,
        const void *opaque)
{
    if (!device)
        return;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    VENDOR_CALL(device, release_recording_frame, opaque);
}

static int camera_auto_focus(struct camera_device *device)
{
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    return VENDOR_CALL(device, auto_focus);
}

static int camera_cancel_auto_focus(struct camera_device *device)
{
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    return VENDOR_CALL(device, cancel_auto_focus);
}

static int camera_take_picture(struct camera_device *device)
{
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    return VENDOR_CALL(device, take_picture);
}

static int camera_cancel_picture(struct camera_device *device)
{
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    return VENDOR_CALL(device, cancel_picture);
}

static int camera_set_parameters(struct camera_device *device,
        const char *params)
{
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    char *tmp = NULL;
    tmp = camera_fixup_setparams(CAMERA_ID(device), params);

    int ret = VENDOR_CALL(device, set_parameters, tmp);
    return ret;
}

static char *camera_get_parameters(struct camera_device *device)
{
    if (!device)
        return NULL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    char *params = VENDOR_CALL(device, get_parameters);

    char *tmp = camera_fixup_getparams(params);
    VENDOR_CALL(device, put_parameters, params);
    params = tmp;

    return params;
}

static void camera_put_parameters(struct camera_device *device __unused, char *params)
{
    if (params)
        free(params);
}

static int camera_send_command(struct camera_device *device,
        int32_t cmd, int32_t arg1, int32_t arg2)
{
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    return VENDOR_CALL(device, send_command, cmd, arg1, arg2);
}

static void camera_release(struct camera_device *device)
{
    if (!device)
        return;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    VENDOR_CALL(device, release);
}

static int camera_dump(struct camera_device *device, int fd)
{
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    return VENDOR_CALL(device, dump, fd);
}

extern "C" void heaptracker_free_leaked_memory(void);

static int camera_device_close(hw_device_t *device)
{
    int ret = 0;
    wrapper_camera_device_t *wrapper_dev = NULL;

    ALOGV("%s", __FUNCTION__);

    android::Mutex::Autolock lock(gCameraWrapperLock);

    if (!device) {
        ret = -EINVAL;
        goto done;
    }

    wrapper_dev = (wrapper_camera_device_t*) device;

    ret = wrapper_dev->vendor->common.close((hw_device_t*)wrapper_dev->vendor);
    camera_update_fixed_fps(wrapper_dev->id, 0);
    if (gOpenCameraCount > 0 && --gOpenCameraCount == 0) {
        for (int i = 0; i < gFixedSetParamsCount; i++)
            free(fixed_set_params[i]);
        free(fixed_set_params);
        fixed_set_params = NULL;
        gFixedSetParamsCount = 0;
        camera_notify_torch_status(TORCH_MODE_STATUS_AVAILABLE_OFF);
    }
    if (wrapper_dev->base.ops)
        free(wrapper_dev->base.ops);
    free(wrapper_dev);
done:
#ifdef HEAPTRACKER
    heaptracker_free_leaked_memory();
#endif
    return ret;
}

/*******************************************************************
 * implementation of camera_module functions
 *******************************************************************/

/* open device handle to one of the cameras
 *
 * assume camera service will keep singleton of each camera
 * so this function will always only be called once per camera instance
 */

static int camera_device_open(const hw_module_t *module, const char *name,
        hw_device_t **device)
{
    int rv = 0;
    int num_cameras = 0;
    int cameraid;
    wrapper_camera_device_t *camera_device = NULL;
    camera_device_ops_t *camera_ops = NULL;

    android::Mutex::Autolock lock(gCameraWrapperLock);

    ALOGV("%s", __FUNCTION__);

    if (name != NULL) {
        if (check_vendor_module())
            return -EINVAL;

        cameraid = atoi(name);
        num_cameras = gVendorModule->get_number_of_cameras();

        if (cameraid < 0 || cameraid >= camera_get_number_of_cameras() ||
                cameraid >= num_cameras) {
            ALOGE("camera service provided cameraid out of bounds, "
                    "cameraid = %d, num supported = %d",
                    cameraid, num_cameras);
            rv = -EINVAL;
            goto fail;
        }

        if (!fixed_set_params) {
            fixed_set_params = (char **)calloc(num_cameras, sizeof(char *));
            if (!fixed_set_params) {
                ALOGE("parameter memory allocation fail");
                rv = -ENOMEM;
                goto fail;
            }
            gFixedSetParamsCount = num_cameras;
        }

        if (gOpenCameraCount == 0)
            camera_reset_fixed_fps();

        if (gTorchEnabled) {
            rv = camera_write_torch(false);
            if (rv)
                goto fail;
            gTorchEnabled = false;
            camera_notify_torch_status(TORCH_MODE_STATUS_AVAILABLE_OFF);
        }

        camera_device = (wrapper_camera_device_t*)malloc(sizeof(*camera_device));
        if (!camera_device) {
            ALOGE("camera_device allocation fail");
            rv = -ENOMEM;
            goto fail;
        }
        memset(camera_device, 0, sizeof(*camera_device));
        camera_device->id = cameraid;

        rv = gVendorModule->common.methods->open(
                (const hw_module_t*)gVendorModule, name,
                (hw_device_t**)&(camera_device->vendor));
        if (rv) {
            ALOGE("vendor camera open fail");
            goto fail;
        }
        ALOGV("%s: got vendor camera device 0x%08X",
                __FUNCTION__, (uintptr_t)(camera_device->vendor));

        camera_ops = (camera_device_ops_t*)malloc(sizeof(*camera_ops));
        if (!camera_ops) {
            ALOGE("camera_ops allocation fail");
            rv = -ENOMEM;
            goto fail;
        }

        memset(camera_ops, 0, sizeof(*camera_ops));

        camera_device->base.common.tag = HARDWARE_DEVICE_TAG;
        camera_device->base.common.version = CAMERA_DEVICE_API_VERSION_1_0;
        camera_device->base.common.module = (hw_module_t *)(module);
        camera_device->base.common.close = camera_device_close;
        camera_device->base.ops = camera_ops;

        camera_ops->set_preview_window = camera_set_preview_window;
        camera_ops->set_callbacks = camera_set_callbacks;
        camera_ops->enable_msg_type = camera_enable_msg_type;
        camera_ops->disable_msg_type = camera_disable_msg_type;
        camera_ops->msg_type_enabled = camera_msg_type_enabled;
        camera_ops->start_preview = camera_start_preview;
        camera_ops->stop_preview = camera_stop_preview;
        camera_ops->preview_enabled = camera_preview_enabled;
        camera_ops->store_meta_data_in_buffers = camera_store_meta_data_in_buffers;
        camera_ops->start_recording = camera_start_recording;
        camera_ops->stop_recording = camera_stop_recording;
        camera_ops->recording_enabled = camera_recording_enabled;
        camera_ops->release_recording_frame = camera_release_recording_frame;
        camera_ops->auto_focus = camera_auto_focus;
        camera_ops->cancel_auto_focus = camera_cancel_auto_focus;
        camera_ops->take_picture = camera_take_picture;
        camera_ops->cancel_picture = camera_cancel_picture;
        camera_ops->set_parameters = camera_set_parameters;
        camera_ops->get_parameters = camera_get_parameters;
        camera_ops->put_parameters = camera_put_parameters;
        camera_ops->send_command = camera_send_command;
        camera_ops->release = camera_release;
        camera_ops->dump = camera_dump;

        *device = &camera_device->base.common;
        if (gOpenCameraCount++ == 0)
            camera_notify_torch_status(TORCH_MODE_STATUS_NOT_AVAILABLE);
    }

    return rv;

fail:
    if (camera_device) {
        if (camera_device->vendor)
            camera_device->vendor->common.close((hw_device_t*)camera_device->vendor);
        free(camera_device);
        camera_device = NULL;
    }
    if (camera_ops) {
        free(camera_ops);
        camera_ops = NULL;
    }
    if (gOpenCameraCount == 0) {
        free(fixed_set_params);
        fixed_set_params = NULL;
        gFixedSetParamsCount = 0;
    }
    *device = NULL;
    return rv;
}

/*
 * The vendor HAL enumerates three sensors: rear (OV4688), front (S5K5E or
 * OV2722) and the rear depth subcam (OV2722, sensor position 2), which runs
 * on its own CSIPHY1/CSID1/CCI pipeline. Apps that pick the first back-facing
 * camera by index misbehave with a second one, so the subcam is exposed only
 * when persist.camera.expose_depth_subcam is set.
 */
#define NUM_USER_CAMERAS 2

static int camera_get_number_of_cameras(void)
{
    ALOGV("%s", __FUNCTION__);
    if (check_vendor_module())
        return 0;
    int vendor_cameras = gVendorModule->get_number_of_cameras();
    if (property_get_bool("persist.camera.expose_depth_subcam", false))
        return vendor_cameras;
    return vendor_cameras < NUM_USER_CAMERAS ? vendor_cameras : NUM_USER_CAMERAS;
}

/*
 * The subcam's device-tree node carries qcom,sensor-position <2> (aux), which
 * the vendor HAL reports as CAMERA_FACING_FRONT, although the sensor sits on
 * the back beside the main camera. Its orientation is the node's
 * qcom,mount-angle and passes through unchanged.
 */
static int camera_get_camera_info(int camera_id, struct camera_info *info)
{
    ALOGV("%s", __FUNCTION__);
    if (check_vendor_module())
        return -ENODEV;
    if (!info || camera_id < 0 || camera_id >= camera_get_number_of_cameras())
        return -EINVAL;

    int result = gVendorModule->get_camera_info(camera_id, info);
    if (result)
        return result;
    if (camera_id >= NUM_USER_CAMERAS)
        info->facing = CAMERA_FACING_BACK;

    /*
     * The main camera (0) and the subcam (2) sit on separate CSIPHY/CSID/VFE
     * paths and form the Duo stereo pair, so each costs 50 of camera service's
     * budget of 100 and they open together. The front camera (1) costs 100
     * and conflicts with both.
     */
    static char id0[] = "0";
    static char id1[] = "1";
    static char id2[] = "2";
    static char *front_conflicts[] = { id0, id2 };
    static char *pair_conflicts[] = { id1 };
    if (camera_id >= MAX_WRAPPED_CAMERAS)
        return -EINVAL;
    info->device_version = CAMERA_DEVICE_API_VERSION_1_0;
    if (camera_id == 1) {
        info->resource_cost = 100;
        info->conflicting_devices = front_conflicts;
        info->conflicting_devices_length = camera_get_number_of_cameras() > 2 ? 2 : 1;
    } else {
        info->resource_cost = 50;
        info->conflicting_devices = pair_conflicts;
        info->conflicting_devices_length = 1;
    }
    return 0;
}

static int camera_module_set_callbacks(const camera_module_callbacks_t *callbacks)
{
    android::Mutex::Autolock lock(gCameraWrapperLock);
    gModuleCallbacks = callbacks;
    if (gOpenCameraCount > 0)
        camera_notify_torch_status(TORCH_MODE_STATUS_NOT_AVAILABLE);
    else if (gTorchEnabled)
        camera_notify_torch_status(TORCH_MODE_STATUS_AVAILABLE_ON);
    return 0;
}

/*
 * The TPS61310 torch_0 LED sits beside the main sensor, so camera 0 owns the
 * torch; every other exposed camera, the rear depth subcam included, has no
 * flash unit of its own.
 */
static int camera_set_torch_mode(const char *camera_id, bool enabled)
{
    if (!camera_id || !*camera_id)
        return -EINVAL;
    char *end;
    long id = strtol(camera_id, &end, 10);
    if (*end || id < 0 || id >= camera_get_number_of_cameras())
        return -EINVAL;
    if (id != 0)
        return -ENOSYS;

    android::Mutex::Autolock lock(gCameraWrapperLock);
    if (gOpenCameraCount > 0)
        return -EBUSY;

    int result = camera_write_torch(enabled);
    if (result) {
        ALOGE("torch brightness write failed: %d", result);
        return result;
    }
    gTorchEnabled = enabled;
    camera_notify_torch_status(enabled ? TORCH_MODE_STATUS_AVAILABLE_ON :
            TORCH_MODE_STATUS_AVAILABLE_OFF);
    return 0;
}
