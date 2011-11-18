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

ifeq "$(findstring msm7627a,$(QCOM_TARGET_PRODUCT))" "msm7627a"

LOCAL_MODULE := media_profiles.xml

LOCAL_MODULE_TAGS := optional

# This will install the file in /system/etc
#
LOCAL_MODULE_CLASS := ETC

LOCAL_SRC_FILES := $(LOCAL_MODULE)

include $(BUILD_PREBUILT)
endif

