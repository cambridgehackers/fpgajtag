
include $(CLEAR_VARS)
LOCAL_PATH := $(PWD)
LOCAL_ARM_MODE := arm
LOCAL_SRC_FILES := cortex.c fpgajtag.c process.c util.c

LOCAL_MODULE := fpgajtag
LOCAL_MODULE_TAGS := optional
LOCAL_LDLIBS := -llog -lz
LOCAL_CPPFLAGS := "-march=armv7-a"

include $(BUILD_EXECUTABLE)
