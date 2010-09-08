#include <media/stagefright/HardwareAPI.h>

#include "QComHardwareRenderer.h"

using android::sp;
using android::ISurface;
using android::VideoRenderer;

VideoRenderer *createRenderer(
        const sp<ISurface> &surface,
        const char *componentName,
        OMX_COLOR_FORMATTYPE colorFormat,
        size_t displayWidth, size_t displayHeight,
        size_t decodedWidth, size_t decodedHeight,
        size_t rotation, size_t flags) { //rotation unused for now
    using android::QComHardwareRenderer;

    static const int OMX_QCOM_COLOR_FormatYVU420SemiPlanar = 0x7FA30C00;
    static const int OMX_QCOM_COLOR_FormatYVU420SemiPlanarInterlace = 0x7FA30C04;

    if ((colorFormat == OMX_QCOM_COLOR_FormatYVU420SemiPlanar ||
         colorFormat == OMX_QCOM_COLOR_FormatYVU420SemiPlanarInterlace)
        && !strncmp(componentName, "OMX.qcom.video.decoder.", 23)) {
        return new QComHardwareRenderer(
                surface, colorFormat,
                displayWidth, displayHeight,
                decodedWidth, decodedHeight, rotation);
    }

    return NULL;
}
