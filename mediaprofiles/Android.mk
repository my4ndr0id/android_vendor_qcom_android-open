LOCAL_PATH := $(my-dir)

########################
include $(CLEAR_VARS)

ifeq ($(call is-board-platform,msm8960),true)

LOCAL_MODULE := media_profiles.xml

LOCAL_MODULE_TAGS := optional

# This will install the file in /system/etc
#
LOCAL_MODULE_CLASS := ETC

LOCAL_SRC_FILES := $(LOCAL_MODULE)

include $(BUILD_PREBUILT)
endif

ifeq ($(call is-board-platform,msm8660),true)

LOCAL_MODULE := media_profiles.xml

LOCAL_MODULE_TAGS := optional

# This will install the file in /system/etc
#
LOCAL_MODULE_CLASS := ETC

LOCAL_SRC_FILES := $(LOCAL_MODULE)

include $(BUILD_PREBUILT)
endif

ifeq ($(call is-board-platform,msm7627a),true)

LOCAL_MODULE := media_profiles.xml

LOCAL_MODULE_TAGS := optional

# This will install the file in /system/etc
#
LOCAL_MODULE_CLASS := ETC

LOCAL_SRC_FILES := $(LOCAL_MODULE)

include $(BUILD_PREBUILT)
endif

