#pragma once

#include <cstdint>
#include "Includes/NeverDestroyedMutex.h"

struct UnityEngine_Vector2
{
    float x;
    float y;
};

enum class UnityEngine_TouchPhase
{
    Began = 0,
    Moved = 1,
    Stationary = 2,
    Ended = 3,
    Canceled = 4,
};

enum class UnityEngine_TouchType
{
    Direct = 0,
    Indirect = 1,
    Stylus = 2,
};

struct UnityEngine_Touch
{
    // Fields
    int32_t m_FingerId;                  // 0x10
    UnityEngine_Vector2 m_Position;      // 0x14
    UnityEngine_Vector2 m_RawPosition;   // 0x1c
    UnityEngine_Vector2 m_PositionDelta; // 0x24
    float m_TimeDelta;                   // 0x2c
    int32_t m_TapCount;                  // 0x30
    UnityEngine_TouchPhase m_Phase;      // 0x34
    UnityEngine_TouchType m_Type;        // 0x38
    float m_Pressure;                    // 0x3c
    float m_maximumPossiblePressure;     // 0x40
    float m_Radius;                      // 0x44
    float m_RadiusVariance;              // 0x48
    float m_AltitudeAngle;               // 0x4c
    float m_AzimuthAngle;                // 0x50
};

namespace Unity
{
    // ImGui context 存活标志。输入 hook 装在游戏的输入路径上，
    // context 被销毁后必须靠它直通原函数，不能再摸 ImGui。
    extern bool g_uiContextAlive;

    // 保护 ImGuiIO 的跨线程访问。
    //
    // 输入 hook（游戏输入线程）往 g.InputEventsQueue push_back，
    // 渲染线程的 NewFrame 遍历并清空同一个 vector —— 扩容 realloc 时
    // free 旧缓冲，另一线程还在遍历就是 use-after-free。
    // 渲染线程（Menu/ImGui.cpp 的 internalDrawMenu）必须用同一把锁。
    // 定义在 Unity.cpp。
    NeverDestroyedMutex &InputMutex();

    // 菜单窗口的屏幕矩形，由渲染线程每帧发布、输入线程读取。
    //
    // 用途：在触摸**开始**的那一刻就能判断它是不是落在菜单上。
    // 之前只看 io.WantCaptureMouse，而它是由上一帧的鼠标位置算出来的 ——
    // 于是「第一次按下」时用的是手指落下**之前**的位置，判断必然滞后一帧。
    //
    // 这两个函数**不自己加锁**，调用方必须已经持有 InputMutex。
    void PublishMenuRect(float x0, float y0, float x1, float y1);
    bool QueryMenuRect(float &x0, float &y0, float &x1, float &y1);

    void HookInput();
    void UninstallInputHooks();
}
