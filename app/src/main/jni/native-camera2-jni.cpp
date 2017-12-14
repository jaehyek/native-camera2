/*
 * Copyright (C) 2016-2017, Collabora Ltd.
 *   Author: Justin Kim <justin.kim@collabora.com>
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

#include <assert.h>
#include <jni.h>
#include <pthread.h>

#include <android/native_window_jni.h>

#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>

#include "messages-internal.h"

static ANativeWindow *theNativeWindow;
static ACameraDevice *cameraDevice;
static ACaptureRequest *captureRequest;
static ACameraOutputTarget *cameraOutputTarget;
static ACaptureSessionOutput *sessionOutput;
static ACaptureSessionOutputContainer *captureSessionOutputContainer;
static ACameraCaptureSession *captureSession;

static ACameraDevice_StateCallbacks deviceStateCallbacks;
static ACameraCaptureSession_stateCallbacks captureSessionStateCallbacks;

static void camera_device_on_disconnected(void *context, ACameraDevice *device)
{
    LOGI("Camera(id: %s) is diconnected.\n", ACameraDevice_getId(device));
}

static void camera_device_on_error(void *context, ACameraDevice *device, int error)
{
    LOGE("Error(code: %d) on Camera(id: %s).\n", error, ACameraDevice_getId(device));
}

static void capture_session_on_ready(void *context, ACameraCaptureSession *session)
{
    LOGI("Session is ready.\n");
}

static void capture_session_on_active(void *context, ACameraCaptureSession *session)
{
    LOGI("Session is activated.\n");
}


extern "C" {
JNIEXPORT void JNICALL Java_org_freedesktop_nativecamera2_NativeCamera2_stopPreview(JNIEnv *env,
                                                                                    jclass clazz);
JNIEXPORT void JNICALL Java_org_freedesktop_nativecamera2_NativeCamera2_startPreview(JNIEnv *env,
                                                                                     jclass clazz,
                                                                                     jobject surface);
}

static void openCamera(ACameraDevice_request_template templateId)
{
    ACameraIdList *cameraIdList = NULL;
    ACameraMetadata *cameraMetadata = NULL;

    const char *selectedCameraId = NULL;
    camera_status_t camera_status = ACAMERA_OK;
    ACameraManager *cameraManager = ACameraManager_create();

    camera_status = ACameraManager_getCameraIdList(cameraManager, &cameraIdList);
    if (camera_status != ACAMERA_OK)
    {
        LOGE("Failed to get camera id list (reason: %d)\n", camera_status);
        return;
    }

    if (cameraIdList->numCameras < 1)
    {
        LOGE("No camera device detected.\n");
        return;
    }

    // camera ID을 선택
    selectedCameraId = cameraIdList->cameraIds[0];

    LOGI("Trying to open Camera2 (id: %s, num of camera : %d)\n", selectedCameraId,
         cameraIdList->numCameras);

    camera_status = ACameraManager_getCameraCharacteristics(cameraManager, selectedCameraId,
                                                            &cameraMetadata);

    if (camera_status != ACAMERA_OK)
    {
        LOGE("Failed to get camera meta data of ID:%s\n", selectedCameraId);
    }

    deviceStateCallbacks.onDisconnected = camera_device_on_disconnected;
    deviceStateCallbacks.onError = camera_device_on_error;

    // cameraDevice을 생성.
    camera_status = ACameraManager_openCamera(cameraManager, selectedCameraId,
                                              &deviceStateCallbacks, &cameraDevice);

    if (camera_status != ACAMERA_OK)
    {
        LOGE("Failed to open camera device (id: %s)\n", selectedCameraId);
    }

    // captureRequest 을 생성.
    camera_status = ACameraDevice_createCaptureRequest(cameraDevice, templateId,
                                                       &captureRequest);

    if (camera_status != ACAMERA_OK)
    {
        LOGE("Failed to create preview capture request (id: %s)\n", selectedCameraId);
    }

    ACameraMetadata_free(cameraMetadata);
    ACameraManager_deleteCameraIdList(cameraIdList);
    ACameraManager_delete(cameraManager);
}

static void closeCamera(void)
{
    camera_status_t camera_status = ACAMERA_OK;

    if (captureRequest != NULL)
    {
        ACaptureRequest_free(captureRequest);
        captureRequest = NULL;
    }

    if (cameraOutputTarget != NULL)
    {
        ACameraOutputTarget_free(cameraOutputTarget);
        cameraOutputTarget = NULL;
    }

    if (cameraDevice != NULL)
    {
        camera_status = ACameraDevice_close(cameraDevice);

        if (camera_status != ACAMERA_OK)
        {
            LOGE("Failed to close CameraDevice.\n");
        }
        cameraDevice = NULL;
    }

    if (sessionOutput != NULL)
    {
        ACaptureSessionOutput_free(sessionOutput);
        sessionOutput = NULL;
    }

    if (captureSessionOutputContainer != NULL)
    {
        ACaptureSessionOutputContainer_free(captureSessionOutputContainer);
        captureSessionOutputContainer = NULL;
    }

    LOGI("Close Camera\n");
}

JNIEXPORT void JNICALL Java_org_freedesktop_nativecamera2_NativeCamera2_startPreview(JNIEnv *env,
                                                                                     jclass clazz,
                                                                                     jobject surface)
{
    theNativeWindow = ANativeWindow_fromSurface(env, surface);

    openCamera(TEMPLATE_PREVIEW);

    LOGI("Surface is prepared in %p.\n", surface);

    // captureRequest 에 cameraOutputTarget 을 추가
    ACameraOutputTarget_create(theNativeWindow, &cameraOutputTarget);

    // capture을 output target와 연결한다.
    ACaptureRequest_addTarget(captureRequest, cameraOutputTarget);

    // capture 출력이미지는  destination인 navtiveWindows와 연결된다..
    ACaptureSessionOutput_create(theNativeWindow, &sessionOutput);

    // capture 출력이미지를 모아두는 container 을 생성.
    ACaptureSessionOutputContainer_create(&captureSessionOutputContainer);
    ACaptureSessionOutputContainer_add(captureSessionOutputContainer, sessionOutput);

    captureSessionStateCallbacks.onReady = capture_session_on_ready;
    captureSessionStateCallbacks.onActive = capture_session_on_active;

    // camera device 마다  CaptureSession을 생성하고, output container와 연결한다.
    ACameraDevice_createCaptureSession(cameraDevice, captureSessionOutputContainer,
                                       &captureSessionStateCallbacks, &captureSession);

    // 방금 생성한 CaptureSession에  repeat 속성을 만든다.
    // captureSession, callback, numRequests, &captureRequest, captureSequenceId
    ACameraCaptureSession_setRepeatingRequest(captureSession, NULL, 1, &captureRequest, NULL);
}

JNIEXPORT void JNICALL Java_org_freedesktop_nativecamera2_NativeCamera2_stopPreview(JNIEnv *env,
                                                                                    jclass clazz)
{
    closeCamera();
    if (theNativeWindow != NULL)
    {
        ANativeWindow_release(theNativeWindow);
        theNativeWindow = NULL;
    }
}
