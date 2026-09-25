
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

// g_fontFullRangeRequested 定义在 Main.cpp（和设置界面的开关在一起），
// 声明见 ImGui.h。
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

    // 字体字形范围必须在字体图集烘焙之前读到，而图集是在 setupMenu
    // （第一帧 eglSwapBuffers）里建的，on_init 跑在那之后。
    // 所以这里 —— hack 线程、早于任何一帧 —— 单独把配置文件读一次。
    // 真正的 ConfigInit 仍由 on_init 做（它还要初始化其余配置项），
    // 这次只取需要提前决定的那一项。
    {
        // 声明在这里而不是靠 extern：ConfigGet/ConfigInit 在 Main.cpp，
        // 由 on_init 负责整体初始化，这里只做一次只读解析。
        extern void ReadFontConfigEarly();
        ReadFontConfigEarly();
    }
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

    // 注意 size_pixels 实参会覆盖 font_cfg.SizePixels，所以这里让两者一致，
    // 免得以后有人只改其中一处而困惑。
    ImFontConfig font_cfg;
    font_cfg.SizePixels = 28.0f;

    // 字体图集是启动耗时的大头，而且这份开销是**在渲染线程上**付的
    // （eglSwapBuffers → setupMenu），用户看到的是游戏开屏后卡住好几秒。
    //
    // 旧代码写的是 GetGlyphRangesChineseFull()：实测 21761 个字形
    // （0x4E00-0x9FA5 的 CJK 加上标点、假名、半角等），每个都要在 28px 下
    // 做 4x 超采样光栅化。在中低端机上光这一项就要好几秒 —— 换一个
    //「启动慢」的问题，却完全没换来对应的可用性：真正会被显示的汉字
    // 绝大多数是常用字，2 万个字形里绝大部分永远用不到。
    //
    // 现在默认用「常用简体(2500) + 拉丁扩展(336) + 西里尔(656) + 希腊(368)
    // + 基本拉丁与符号(224)」，合并重叠后约 3900 个码点，不到原来的 1/5.5。
    // 确实需要生僻字时把 fontFullRange 设为 true（代价就是启动变慢，
    // 界面上有开关和说明）。
    static ImVector<ImWchar> glyphRanges;

    // 由 hack 线程上的 initModMenu 提前从配置文件读好 —— 必须早于这里，
    // 因为图集一旦烘焙就固定了。声明见 ImGui.h。
    const bool fontFullRange = g_fontFullRangeRequested;

    const auto fontStart = std::chrono::steady_clock::now();

    if (fontFullRange)
    {
        glyphRanges.clear();
        const ImWchar *full = io.Fonts->GetGlyphRangesChineseFull();
        for (const ImWchar *p = full; p && *p; p++)
        {
            glyphRanges.push_back(*p);
        }
    }
    else
    {
        // GetGlyphRangesChineseFull() 返回的是以 0 结尾的区间列表，
        // 指向静态存储；ImFontAtlas 不会拷贝它，所以必须保证生命周期
        // 足够长 —— 这里用 ImVector 存一份，之后一直活着。
        glyphRanges.clear();
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(io.Fonts->GetGlyphRangesDefault()); // 基本拉丁 + 常用符号
        // 拉丁扩展（带重音的欧洲语言）。这个 ImGui 版本没有
        // GetGlyphRangesLatin()，直接手写区间。
        static const ImWchar latinExtended[] = {0x0100, 0x017F, // Latin Extended-A
                                                0x0180, 0x024F, // Latin Extended-B
                                                0};
        builder.AddRanges(latinExtended);
        builder.AddRanges(io.Fonts->GetGlyphRangesGreek());    // 希腊
        builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic()); // 西里尔（俄语游戏）
        builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon()); // 常用简体
        builder.BuildRanges(&glyphRanges);
    }

    // 这才是被真正使用的字体：ImGui 在 io.FontDefault 为空时用 Fonts[0]。
    // 旧代码在这里之后又 AddFontDefault 加了个 ProggyClean，那份永远不会被
    // 选中（Fonts[1]），纯属白占一份图集空间。
    io.FontDefault = io.Fonts->AddFontFromMemoryTTF((void *)OPPOSans_H, OPPOSans_H_size, 28.0f, &font_cfg,
                                                    glyphRanges.Size ? glyphRanges.Data : nullptr);

    const auto fontEnd = std::chrono::steady_clock::now();
    const auto fontMs = std::chrono::duration_cast<std::chrono::milliseconds>(fontEnd - fontStart).count();
    LOGI("字体图集构建完成：%s 范围，%d 个字形，耗时 %lld ms", fontFullRange ? "完整中日韩" : "常用字",
         glyphRanges.Size, (long long)fontMs);
    if (fontFullRange)
    {
        LOGW("启用了完整字形范围，字体会明显拖慢首次启动");
    }

        ImGui::GetStyle().ScaleAllSizes(2);
        ImGuiStyle &style = ImGui::GetStyle();
        style.ScrollbarSize *= 2.5f;

        ctxCreated = true;
    }

    // 通知输入 hook「ImGui 上下文已就绪」：
    // 装在游戏输入路径上的 hook 必须知道什么时候才可以安全地摸 ImGui::GetIO()。
    Unity::g_uiContextAlive = true;

    // on_init 会做分配、JSON 解析、il2cpp 元数据遍历，任何一步抛异常都会
    // 一路冲出 setupMenu → swapbuffers_hook。注入到别人进程里的库没有
    // 「异常边界」，逃出去就是 std::terminate，用户的游戏直接没了。
    try
    {
        if (onInitAddr)
            onInitAddr();
    }
    catch (const std::exception &e)
    {
        LOGE("on_init 抛出异常: %s", e.what());
        g_initState = INIT_FAILED;
    }
    catch (...)
    {
        LOGE("on_init 抛出未知异常");
        g_initState = INIT_FAILED;
    }

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
