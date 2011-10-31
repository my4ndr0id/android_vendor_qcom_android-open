ifneq ($(BUILD_WITHOUT_PV), true)

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

# Set up the OpenCore variables.
include external/opencore/Config.mk
LOCAL_C_INCLUDES := $(PV_INCLUDES)\
                    hardware/msm7k/libgralloc-qsd8k

ifeq ($(call is-board-platform-in-list,msm7627a msm7627_surf msm7627_6x),true)
  LOCAL_SRC_FILES := android_surface_output_msm72xx.cpp
endif
ifeq ($(call is-board-platform-in-list,msm7630_surf msm7630_fusion msm8660),true)
  LOCAL_SRC_FILES := android_surface_output_msm7x30.cpp
endif


LOCAL_CFLAGS := $(PV_CFLAGS_MINUS_VISIBILITY)

LOCAL_C_INCLUDES += hardware/msm7k/libgralloc-qsd8k

LOCAL_SHARED_LIBRARIES := \
    libutils \
    libbinder \
    libcutils \
    libui \
    libhardware\
    libandroid_runtime \
    libmedia \
    libskia \
    libopencore_common \
    libicuuc \
    libsurfaceflinger_client \
    libopencore_player

LOCAL_MODULE := libopencorehw

LOCAL_MODULE_TAGS := optional

LOCAL_LDLIBS += 

include $(BUILD_SHARED_LIBRARY)
endif
