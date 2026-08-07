
#include "ImGui.h"
#include "KittyMemory/KittyMemory.h"
#include "dobby.h"
#include "Includes/Utils.h"
#include "Includes/obfuscate.h"
#include "Includes/Logger.h"
#include "imgui/imgui.h"
#include "Includes/Roboto-Regular.h"
#include "imgui/backends/imgui_impl_opengl3.h"
#include "imgui/backends/imgui_impl_android.h"
#include <GLES3/gl3.h>
#include <unistd.h>
#include "EGL/egl.h"
#include "imgui/OPPOSans-H.h"
using swapbuffers_orig = EGLBoolean (*)(EGLDisplay dpy, EGLSurface surf);
EGLBoolean swapbuffers_hook(EGLDisplay dpy, EGLSurface surf);
swapbuffers_orig o_swapbuffers = nullptr;

void (*menuAddress)();
void (*onInitAddr)();

bool isInitialized = false;
int glWidth = 0;
int glHeight = 0;

int getGlWidth()
{
    return glWidth;
}
int getGlHeight()
{
    return glHeight;
}

// Taken from https://github.com/fedes1to/Zygisk-ImGui-Menu/blob/main/module/src/main/cpp/hook.cpp
#define HOOKINPUT(ret, func, ...)                                                                                      \
    ret (*orig##func)(__VA_ARGS__);                                                                                    \
    ret my##func(__VA_ARGS__)

HOOKINPUT(void, Input, void *thiz, void *ex_ab, void *ex_ac)
{
    origInput(thiz, ex_ab, ex_ac);
    if (isInitialized)
        ImGui_ImplAndroid_HandleInputEvent((AInputEvent *)thiz);
    return;
}

ImVec2 initialScreenSize;
// This menu_addr is used to allow for multiple game support in the future
bool needClear = true;
void *initModMenu(void *menu_addr, void *on_init_addr, bool isJni)
{
    menuAddress = (void (*)())menu_addr;
    onInitAddr = (void (*)())on_init_addr;
    // do
    // {
    //     sleep(1);
    // } while (!isLibraryLoaded(OBFUSCATE("libEGL.so")));
    if (!isJni)
    {
        needClear = false;
        while (!isLibraryLoaded(OBFUSCATE("libEGL.so")))
        {
            sleep(1);
        }
        auto swapBuffers = ((uintptr_t)DobbySymbolResolver(OBFUSCATE("libEGL.so"), OBFUSCATE("eglSwapBuffers")));
        KittyMemory::ProtectAddr((void *)swapBuffers, sizeof(swapBuffers), PROT_READ | PROT_WRITE | PROT_EXEC);
        DobbyHook((void *)swapBuffers, (void *)swapbuffers_hook, (void **)&o_swapbuffers);

// // Taken from https://github.com/fedes1to/Zygisk-ImGui-Menu/blob/main/module/src/main/cpp/hook.cpp
#ifdef LIB_INPUT
        void *sym_input = DobbySymbolResolver(
            OBFUSCATE("/system/lib/libinput.so"),
            OBFUSCATE("_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE"));
        if (sym_input != nullptr)
        {
            DobbyHook((void *)sym_input, (void *)myInput, (void **)&origInput);
        }
#endif
    }
    LOGI("%s", (char *)OBFUSCATE("ImGUI Hooks initialized"));
    return nullptr;
}

void setupMenu()
{
    if (isInitialized)
        return;

    auto ctx = ImGui::CreateContext();
    if (!ctx)
    {
        LOGI("%s", (char *)OBFUSCATE("Failed to create context"));
        return;
    }

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)glWidth, (float)glHeight);
    // io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.IniFilename = nullptr;
    // enable docking
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Setup Platform/Renderer backends
    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");

ImFontConfig font_cfg;
        font_cfg.SizePixels = 22.0f;
        io.Fonts->AddFontFromMemoryTTF((void *)OPPOSans_H, OPPOSans_H_size, 28.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
        io.Fonts->AddFontDefault(&font_cfg);

    ImGui::GetStyle().ScaleAllSizes(2);
    ImGuiStyle &style = ImGui::GetStyle();
    style.ScrollbarSize *= 2.5f;

    if (onInitAddr)
        onInitAddr();

    isInitialized = true;
    LOGI("setup done.");
}
void internalDrawMenu(int width, int height)
{
    if (!isInitialized)
        return;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(width, height);
    ImGui::NewFrame();

    ImGui::SetNextWindowSize(ImVec2((float)width / 2, (float)height / 2), ImGuiCond_Once);
    menuAddress();

    ImGui::Render();

    if (needClear)
    {
        glViewport(0, 0, width, height);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

EGLBoolean swapbuffers_hook(EGLDisplay dpy, EGLSurface surf)
{
    EGLint w, h;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    glWidth = w;
    glHeight = h;
    static bool initialScreenSet = false;
    if (!initialScreenSet)
    {
        initialScreenSize.x = w;
        initialScreenSize.y = h;
        initialScreenSet = true;
    }
    setupMenu();
    internalDrawMenu(w, h);

    return o_swapbuffers(dpy, surf);
}
