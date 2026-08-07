LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE            := libdobby
LOCAL_SRC_FILES         := Dobby/libraries/$(TARGET_ARCH_ABI)/libdobby.a
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/dobby/
include $(PREBUILT_STATIC_LIBRARY)
LOCAL_LDFLAGS := $(LOCAL_PATH)/Dobby/armeabi-v7a/libdobby.a
include $(CLEAR_VARS)
LOCAL_MODULE    := HitMargin

LOCAL_CFLAGS := -w -s -Wno-error=format-security -fvisibility=hidden -fpermissive -fexceptions
LOCAL_CPPFLAGS := -w -s -Wno-error=format-security -fvisibility=hidden -Werror -std=c++20
LOCAL_CPPFLAGS += -Wno-error=c++11-narrowing -fpermissive -Wall -fexceptions
LOCAL_LDFLAGS += -Wl,--gc-sections,--strip-all,-llog
LOCAL_LDLIBS += -llog -landroid -lEGL -lGLESv2 -lGLESv3 -lGLESv1_CM -lz -lm
LOCAL_ARM_MODE := arm

LOCAL_STATIC_LIBRARIES := libdobby frida asmjit

LOCAL_C_INCLUDES += $(LOCAL_PATH)
LOCAL_C_INCLUDES += $(LOCAL_PATH)/imgui
LOCAL_C_INCLUDES += $(LOCAL_PATH)/imgui/backends
FILE_LIST += $(wildcard $(LOCAL_PATH)/imgui/*.c*)
LOCAL_C_INCLUDES += $(LOCAL_PATH)/Frida/$(TARGET_ARCH_ABI)/
LOCAL_C_INCLUDES += $(LOCAL_PATH)/asmjit/include/

LOCAL_C_INCLUDES += $(LOCAL_PATH)

LOCAL_SRC_FILES := Main.cpp \
    Il2cpp/xdl/xdl.c \
    Il2cpp/xdl/xdl_iterate.c \
    Il2cpp/xdl/xdl_linker.c \
    Il2cpp/xdl/xdl_lzma.c \
    Il2cpp/xdl/xdl_util.c \
    Menu/ImGui.cpp \
    Tool/Keyboard.cpp \
    Tool/Tool.cpp \
    Tool/Util.cpp \
    Tool/Patcher.cpp \
    Tool/PopUpSelector.cpp \
    Tool/ClassesTab.cpp \
    Tool/Unity.cpp \
    Tool/ObjectDrawManager.cpp \
    Includes/Utils.cpp \
    Includes/Logger.cpp \
    KittyMemory/KittyMemory.cpp \
    KittyMemory/MemoryPatch.cpp \
    KittyMemory/MemoryBackup.cpp \
    KittyMemory/KittyUtils.cpp \
    Il2cpp/Il2cpp.cpp \
    Il2cpp/il2cpp-class.cpp \
    Frida/gumpp/runtime.cpp \
    Frida/gumpp/backtracer.cpp \
    Frida/gumpp/interceptor.cpp \
    Frida/gumpp/invocationlistener.cpp \
    Frida/gumpp/returnaddress.cpp \
    Tool/Frida.cpp \
    imgui/backends/imgui_impl_opengl3.cpp \
    imgui/backends/imgui_impl_android.cpp \
    imgui/imgui.cpp \
    imgui/imgui_draw.cpp \
    imgui/imgui_demo.cpp \
    imgui/imgui_tables.cpp \
    imgui/imgui_widgets.cpp 

include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := frida
LOCAL_SRC_FILES := Frida/$(TARGET_ARCH_ABI)/libfrida-gum.a
include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)
LOCAL_MODULE := asmjit
LOCAL_SRC_FILES := asmjit/lib/libasmjit.a
include $(PREBUILT_STATIC_LIBRARY)