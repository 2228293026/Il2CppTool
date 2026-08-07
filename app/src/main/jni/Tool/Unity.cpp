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
    ImGuiIO &io = ImGui::GetIO();

    ImVec2 size{ImGui::GetFrameHeight() * 2.f, ImGui::GetFrameHeight() * 2.f};
    if (io.WantCaptureMouse && !(collapsed && fullScreen && (io.MousePos.x > size.x && io.MousePos.y > size.y)))
        return false;
    return oInput_GetMouseButton(n);
}
int get_touchCount()
{
    ImGuiIO &io = ImGui::GetIO();

    auto count = o_get_touchCount();
    if (count > 0)
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
    void HookInput()
    {
        g_Image = Il2cpp::GetImage("UnityEngine.InputLegacyModule"); // hack
        REPLACE_NAME_ORIG("UnityEngine.Input", "get_touchCount", get_touchCount,
                          o_get_touchCount); // TODO: pass image to REPLACE macro
        REPLACE_NAME_ORIG("UnityEngine.Input", "GetMouseButton", Input_GetMouseButton, oInput_GetMouseButton);
        Input = g_Image->getClass("UnityEngine.Input");
    }
} // namespace Unity
