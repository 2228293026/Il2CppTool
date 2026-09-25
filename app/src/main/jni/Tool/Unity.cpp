#include "Unity.h"
#include "Il2cpp/Il2cpp.h"
#include "Includes/Macros.h"
#include "Includes/obfuscate.h"
#include "imgui/imgui.h"

// this function hook will prevent touch pass through the ImGui window
int (*o_get_touchCount)();
int get_touchCount();
bool (*oInput_GetMouseButton)(int n);
bool Input_GetMouseButton(int n);

static Il2CppClass *Input;

extern bool collapsed;
extern bool fullScreen;

bool Input_GetMouseButton(int n)
{
    // 同上：hook 失败时直通，绝不能调用空的原函数指针
    if (!oInput_GetMouseButton)
        return false;

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
} // namespace Unity
