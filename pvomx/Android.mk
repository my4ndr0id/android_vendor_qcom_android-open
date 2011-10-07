ifneq ($(BUILD_WITHOUT_PV),true)
LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_SRC_FILES := 01_qcomm_omx.cfg
LOCAL_BUILT_MODULE_STEM := 01_qcomm_omx.cfg
LOCAL_MODULE_SUFFIX := $(suffix 01_qcomm_omx.cfg)
LOCAL_MODULE := $(basename 01_qcomm_omx.cfg)
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS :=  ETC
include $(BUILD_PREBUILT)

include vendor/qcom/android-open/pvomx/omx_core_plugin/Android.mk
endif
