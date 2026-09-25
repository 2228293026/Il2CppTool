
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
#include "Tool/Unity.h"
using swapbuffers_orig = EGLBoolean (*)(EGLDisplay dpy, EGLSurface surf);
EGLBoolean swapbuffers_hook(EGLDisplay dpy, EGLSurface surf);
swapbuffers_orig o_swapbuffers = nullptr;

void (*menuAddress)();
void (*onInitAddr)();

bool isInitialized = false;
// setupMenu 只应该试一次（失败时不要每帧重来重建 context）；没就绪时按帧重试。
// 定义在 Main.cpp。
int g_initState = INIT_PENDING;
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
    // hook 失败时 origInput 是空的，直接调就是空指针跳转
    if (!origInput)
        return;
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
        if (!swapBuffers)
        {
            // 解析不到 eglSwapBuffers 就没法渲染菜单，标失败让 setupMenu 早退，
            // 否则后面每帧都会拿一个空的原函数指针去调。
            LOGE("解析不到 eglSwapBuffers，菜单无法工作");
            g_initState = INIT_FAILED;
            return nullptr;
        }
        KittyMemory::ProtectAddr((void *)swapBuffers, sizeof(swapBuffers), PROT_READ | PROT_WRITE | PROT_EXEC);
        if (DobbyHook((void *)swapBuffers, (void *)swapbuffers_hook, (void **)&o_swapbuffers) != 0 || !o_swapbuffers)
        {
            LOGE("eglSwapBuffers hook 安装失败");
            g_initState = INIT_FAILED;
            return nullptr;
        }

// // Taken from https://github.com/fedes1to/Zygisk-ImGui-Menu/blob/main/module/src/main/cpp/hook.cpp
#ifdef LIB_INPUT
        void *sym_input = DobbySymbolResolver(
            OBFUSCATE("/system/lib/libinput.so"),
            OBFUSCATE("_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE"));
        if (sym_input != nullptr)
        {
            if (DobbyHook((void *)sym_input, (void *)myInput, (void **)&origInput) != 0 || !origInput)
            {
                // 没装上就当作没这个 hook，myInput 里也会因为 origInput 为空而直接返回
                LOGE("libinput hook 安装失败，忽略触摸转发");
                origInput = nullptr;
            }
        }
#endif
    }
    LOGI("%s", (char *)OBFUSCATE("ImGUI Hooks initialized"));
    return nullptr;
}

void setupMenu()
{
    if (isInitialized || g_initState == INIT_FAILED)
        return;

    // 重试路径会重复进来，ImGui context 只能建一次
    static bool ctxCreated = false;
    if (!ctxCreated)
    {
        auto ctx = ImGui::CreateContext();
        if (!ctx)
        {
            LOGI("%s", (char *)OBFUSCATE("Failed to create context"));
            g_initState = INIT_FAILED;
            return;
        }

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)glWidth, (float)glHeight);
    // io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.IniFilename = nullptr;
    // enable docking
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Setup Platform/Renderer backends
    if (!ImGui_ImplAndroid_Init())
    {
        LOGE("ImGui_ImplAndroid_Init 失败");
        ImGui::DestroyContext();
        g_initState = INIT_FAILED;
        return;
    }
    if (!ImGui_ImplOpenGL3_Init("#version 300 es"))
    {
        LOGE("ImGui_ImplOpenGL3_Init 失败");
        ImGui_ImplAndroid_Shutdown();
        ImGui::DestroyContext();
        g_initState = INIT_FAILED;
        return;
    }

ImFontConfig font_cfg;
        font_cfg.SizePixels = 22.0f;
        io.Fonts->AddFontFromMemoryTTF((void *)OPPOSans_H, OPPOSans_H_size, 28.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
        io.Fonts->AddFontDefault(&font_cfg);

        ImGui::GetStyle().ScaleAllSizes(2);
        ImGuiStyle &style = ImGui::GetStyle();
        style.ScrollbarSize *= 2.5f;

        ctxCreated = true;
    }

    // 通知输入 hook「ImGui 上下文已就绪」：
    // 装在游戏输入路径上的 hook 必须知道什么时候才可以安全地摸 ImGui::GetIO()。
    Unity::g_uiContextAlive = true;

    if (onInitAddr)
        onInitAddr();

    // INIT_PENDING：依赖还没就绪（例如 libil2cpp.so 还没加载）。
    // 直接跳过这一帧的菜单渲染 —— 游戏画面照常，钩子下一帧会再试。
    // 这样「等依赖」不再占用渲染线程，不会造成首帧卡死 / ANR。
    if (g_initState == INIT_PENDING)
    {
        static int attempts = 0;
        if (++attempts == 1)
        {
            LOGI("等待 il2cpp 就绪中…（菜单暂不显示，游戏不受影响）");
        }
        if (attempts >= INIT_MAX_ATTEMPTS)
        {
            LOGE("等待 %d 帧仍未就绪，放弃菜单初始化", attempts);
            g_initState = INIT_FAILED;
        }
    }

    if (g_initState == INIT_FAILED)
    {
        LOGE("on_init 失败，关闭菜单");
        // 关键顺序：先摘输入 hook，再销毁 context。
        // 否则游戏下一次输入事件会进来摸一个已经销毁的 ImGui context。
        Unity::g_uiContextAlive = false;
        Unity::UninstallInputHooks();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplAndroid_Shutdown();
        ImGui::DestroyContext();
        ctxCreated = false;
        return;
    }
    if (g_initState == INIT_PENDING)
    {
        return;
    }

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
    // hook 安装失败时原函数是空的，直接调就是空指针跳转 —— 这种情况根本不该进来，
    // 真进来了也不能崩游戏。
    if (!o_swapbuffers)
        return EGL_FALSE;

    EGLint w = 0, h = 0;
    // 旧代码忽略返回值：查询失败时 w/h 是未初始化的垃圾值，
    // 会一路喂给 ImGui 的 DisplaySize 和 glViewport。
    if (eglQuerySurface(dpy, surf, EGL_WIDTH, &w) != EGL_TRUE || w <= 0)
        w = glWidth;
    if (eglQuerySurface(dpy, surf, EGL_HEIGHT, &h) != EGL_TRUE || h <= 0)
        h = glHeight;
    if (w <= 0 || h <= 0)
    {
        // 尺寸还拿不到就这一帧不画菜单，但仍然要把画面交还给游戏
        return o_swapbuffers(dpy, surf);
    }
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
