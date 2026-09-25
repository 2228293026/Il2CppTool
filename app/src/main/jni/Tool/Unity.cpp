#include "Unity.h"
#include "Il2cpp/Il2cpp.h"
#include "Includes/Macros.h"
#include "Includes/obfuscate.h"
#include "dobby.h"
#include "imgui/imgui.h"

// this function hook will prevent touch pass through the ImGui window
int (*o_get_touchCount)();
int get_touchCount();
bool (*oInput_GetMouseButton)(int n);
bool Input_GetMouseButton(int n);

static Il2CppClass *Input;

// ImGui context 是否还活着。输入 hook 装在游戏的输入路径上，
// 初始化失败销毁 context 后游戏仍会调进来，这时必须直通，不能摸 ImGui。
// 由 Menu/ImGui.cpp 在创建/销毁 context 时维护。
namespace Unity
{
bool g_uiContextAlive = false;
} // namespace Unity

extern bool collapsed;
extern bool fullScreen;

bool Input_GetMouseButton(int n)
{
    // 同上：hook 失败时直通，绝不能调用空的原函数指针
    if (!oInput_GetMouseButton)
        return false;

    // ImGui context 已被销毁（初始化失败后 setupMenu 会 DestroyContext）时，
    // 不能再摸 ImGui::GetIO()：GImGui 为空 → 空指针解引用。
    // 这两个 hook 装在游戏的输入路径上，销毁 context 前必须先摘掉。
    // 这两个 hook 定义在 namespace Unity 之外，必须写全名
    if (!Unity::g_uiContextAlive)
        return oInput_GetMouseButton(n);

    ImGuiIO &io = ImGui::GetIO();

    ImVec2 size{ImGui::GetFrameHeight() * 2.f, ImGui::GetFrameHeight() * 2.f};
    if (io.WantCaptureMouse && !(collapsed && fullScreen && (io.MousePos.x > size.x && io.MousePos.y > size.y)))
        return false;
    return oInput_GetMouseButton(n);
}
int get_touchCount()
{
    // 输入 hook 没装成功时必须直通原函数，不能去调还为空的 o_get_touchCount
    if (!o_get_touchCount)
        return 0;

    // ImGui context 已销毁时不能摸 GetIO()，见上方说明
    if (!Unity::g_uiContextAlive)
        return o_get_touchCount();

    ImGuiIO &io = ImGui::GetIO();

    auto count = o_get_touchCount();
    if (count > 0 && Input)
    {
        // auto mousePresent = Input->invoke_static_method<bool>("get_mousePresent");
        // if (mousePresent)
        // {
        //     LOGD("MOUSE");
        //     io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
        // }
        // else
        // {
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        // }
        auto touch = Input->invoke_static_method<UnityEngine_Touch>("GetTouch", 0);
        float x = touch.m_Position.x;
        float y = io.DisplaySize.y - touch.m_Position.y;

        if (touch.m_Phase == UnityEngine_TouchPhase::Began)
        {
            io.AddMousePosEvent(x, y);
            io.AddMouseButtonEvent(0, true);
        }
        else if (touch.m_Phase == UnityEngine_TouchPhase::Ended)
        {
            io.AddMousePosEvent(x, y);
            io.AddMouseButtonEvent(0, false);
            io.AddMousePosEvent(-1, -1);
        }
        else if (touch.m_Phase == UnityEngine_TouchPhase::Moved)
        {
            io.AddMousePosEvent(x, y);
        }
    }

    ImVec2 size{ImGui::GetFrameHeight() * 2.f, ImGui::GetFrameHeight() * 2.f};
    if (io.WantCaptureMouse && !(collapsed && fullScreen && (io.MousePos.x > size.x && io.MousePos.y > size.y)))
    {
        return 0;
    }

    return count;
}

namespace Unity
{
    static Il2CppImage *g_Image; // REPLACE_* macro depends on g_Image

    // 输入 hook 是否完整可用。false 时 get_touchCount / Input_GetMouseButton
    // 必须直通，否则它们会调用还为空的原函数指针 → 跳进 0 地址。
    static bool g_inputHooked = false;

    void HookInput()
    {
        // 元数据没就绪 / 类名不存在时，直接放弃输入 hook，而不是硬着头皮解引用。
        g_Image = Il2cpp::GetImage("UnityEngine.InputLegacyModule"); // hack
        if (!g_Image)
        {
            LOGE("找不到 UnityEngine.InputLegacyModule，跳过输入 hook");
            return;
        }
        Input = g_Image->getClass("UnityEngine.Input");
        if (!Input)
        {
            LOGE("找不到 UnityEngine.Input 类，跳过输入 hook");
            return;
        }

        REPLACE_NAME_ORIG("UnityEngine.Input", "get_touchCount", get_touchCount,
                          o_get_touchCount); // TODO: pass image to REPLACE macro
        REPLACE_NAME_ORIG("UnityEngine.Input", "GetMouseButton", Input_GetMouseButton, oInput_GetMouseButton);

        // 两个原函数指针都必须拿到，缺一个就不能接管输入
        if (!o_get_touchCount || !oInput_GetMouseButton)
        {
            LOGE("输入 hook 安装不完整（touch=%p mouse=%p），直通原函数", (void *)o_get_touchCount,
                 (void *)oInput_GetMouseButton);
            g_inputHooked = false;
            return;
        }
        g_inputHooked = true;
    }

    void UninstallInputHooks()
    {
        if (!g_inputHooked)
        {
            return;
        }
        // 销毁 ImGui context 之前必须把输入 hook 摘掉：
        // 否则游戏下一次调 Input.get_touchCount 会进来摸已经不存在的 ImGui 上下文。
        // g_uiContextAlive 置 false 让两个 hook 先直通原函数，摘干净后再销毁 context。
        g_uiContextAlive = false;
        if (Input)
        {
            if (auto *m = Input->getMethod("get_touchCount"))
            {
                DobbyDestroy((void *)m->methodPointer);
            }
            if (auto *m = Input->getMethod("GetMouseButton"))
            {
                DobbyDestroy((void *)m->methodPointer);
            }
        }
        o_get_touchCount = nullptr;
        oInput_GetMouseButton = nullptr;
        g_inputHooked = false;
        LOGI("已卸载 Unity 输入 hook");
    }
} // namespace Unity
