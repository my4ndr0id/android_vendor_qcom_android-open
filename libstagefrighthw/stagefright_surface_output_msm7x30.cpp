/*
 * Copyright (C) 2009 The Android Open Source Project
 * Copyright (c) 2010, Code Aurora Forum. All rights reserved.
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

#include <media/stagefright/HardwareAPI.h>
#include "QComHardwareOverlayRenderer.h"
#include "QComHardwareRenderer.h"
#define LOG_TAG "StagefrightSurfaceOutput7630"
#include <utils/Log.h>

using android::sp;
using android::ISurface;
using android::VideoRenderer;

VideoRenderer *createRenderer(
        const sp<ISurface> &surface,
        const char *componentName,
        OMX_COLOR_FORMATTYPE colorFormat,
        size_t displayWidth, size_t displayHeight,
        size_t decodedWidth, size_t decodedHeight,
        size_t rotation , size_t flags ) {
    using android::QComHardwareOverlayRenderer;
    using android::QComHardwareRenderer;


    static const int OMX_QCOM_COLOR_FormatYVU420SemiPlanar = 0x7FA30C00;
    static const int QOMX_COLOR_FormatYUV420PackedSemiPlanar64x32Tile2m8ka = 0x7FA30C03;
    static const int QOMX_INTERLACE_FLAG = 0x49283654;

#ifndef SURF7x30
    if((colorFormat == OMX_COLOR_FormatYUV420SemiPlanar ||
        colorFormat == QOMX_COLOR_FormatYUV420PackedSemiPlanar64x32Tile2m8ka ||
        colorFormat == (OMX_QCOM_COLOR_FormatYVU420SemiPlanar ^ QOMX_INTERLACE_FLAG) ||
        colorFormat == (QOMX_COLOR_FormatYUV420PackedSemiPlanar64x32Tile2m8ka ^ QOMX_INTERLACE_FLAG) ||
        colorFormat == (OMX_COLOR_FormatYUV420SemiPlanar ^ QOMX_INTERLACE_FLAG))
        && !strncmp(componentName, "OMX.qcom.video.decoder.", 23)) {
            LOGV("StagefrightSurfaceOutput7x30::createRenderer");
            QComHardwareOverlayRenderer *videoRenderer =  new QComHardwareOverlayRenderer(
                surface, colorFormat,
                displayWidth, displayHeight,
                decodedWidth, decodedHeight, rotation);
            bool initSuccess = videoRenderer->InitOverlayRenderer();
            if (!initSuccess) {
                LOGE("Create Overlay Renderer failed");
                delete videoRenderer;
                return NULL;
            }
            LOGV("Create Overlay Renderer successfull");
            return  static_cast<VideoRenderer *> (videoRenderer);
    }
#else
    if((colorFormat == OMX_COLOR_FormatYUV420SemiPlanar ||
       colorFormat == (OMX_QCOM_COLOR_FormatYVU420SemiPlanar ^ QOMX_INTERLACE_FLAG) ||
       colorFormat == (QOMX_COLOR_FormatYUV420PackedSemiPlanar64x32Tile2m8ka ^ QOMX_INTERLACE_FLAG) ||
       colorFormat == (OMX_COLOR_FormatYUV420SemiPlanar ^ QOMX_INTERLACE_FLAG))
        && !strncmp(componentName, "OMX.qcom.video.decoder.", 23)) {
            LOGV("StagefrightSurfaceOutput7x30::createRenderer QComHardwareOverlayRenderer");
            QComHardwareOverlayRenderer *videoRenderer = new QComHardwareOverlayRenderer(
                surface, colorFormat,
                displayWidth, displayHeight,
                decodedWidth, decodedHeight, rotation );
            bool initSucess = videoRenderer->InitOverlayRenderer();
            if (!initSucess) {
                LOGE("Create Overlay Renderer failed");
                delete videoRenderer;
                return NULL;
            }
            LOGV("Create Overlay Renderer successfull");
            return static_cast<VideoRenderer *> (videoRenderer);
    }
    else if (colorFormat == QOMX_COLOR_FormatYUV420PackedSemiPlanar64x32Tile2m8ka
        && !strncmp(componentName, "OMX.qcom.video.decoder.", 23)) {
        LOGV("StagefrightSurfaceOutput7x30::createRenderer QComHardwareRenderer");
        return new QComHardwareRenderer(
                surface, colorFormat,
                displayWidth, displayHeight,
                decodedWidth, decodedHeight, rotation );
    }
#endif

    LOGE("error: StagefrightSurfaceOutput7x30::createRenderer returning NULL!");
    return NULL;
}
