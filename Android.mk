LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)


LOCAL_SE_FILE :=$(LOCAL_PATH)/cmd_services.te
LOCAL_DEVICE_SEPOLICY_DIR :=$(PLATDIR)/common/sepolicy/
$(shell cp -rf $(LOCAL_SE_FILE) $(LOCAL_DEVICE_SEPOLICY_DIR))
$(warning "PLATDIR is $(PLATDIR)")
$(warning "$(LOCAL_SE_FILE) copied to $(LOCAL_DEVICE_SEPOLICY_DIR)")



LOCAL_SRC_FILES:= cmd_services.c

LOCAL_SHARED_LIBRARIES := libcutils libc liblog

LOCAL_INIT_RC := cmd_services.rc

LOCAL_MODULE := cmd_services

LOCAL_MODULE_TAGS := optional

#LOCAL_PROPRIETARY_MODULE := true

include $(BUILD_EXECUTABLE)

CUSTOM_MODULES += cmd_services
