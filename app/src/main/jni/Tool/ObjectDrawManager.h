#pragma once
#include "Il2cpp/Il2cpp.h"
#include "imgui/imgui.h"
#include <vector>
#include <string>
#include <mutex>
#include <atomic>

struct Vector3 {
    float x, y, z;
    Vector3() : x(0), y(0), z(0) {}
    Vector3(float x, float y, float z) : x(x), y(y), z(z) {}
};

struct GameObjectInfo {
    Il2CppObject* gameObject = nullptr;
    Il2CppObject* transform = nullptr;
    Vector3 worldPosition;
    Vector3 screenPosition;
    std::string name;
    bool isSelected = false;

    // 到相机的 3D 距离（世界单位，通常就是米）。
    // 屏幕坐标把远近信息压扁了 —— 屏幕上挨着的两个物体可能一个在脚边
    // 一个在几百米外。hasDistance=false 表示这一帧没能取到相机位置，
    // 此时 distance 无意义，不要显示。
    float distance = 0.f;
    bool hasDistance = false;

    // 真实包围盒投影到屏幕后的二维外接矩形（Renderer.bounds 的 8 个角点）。
    // hasScreenBounds=false 时退回固定尺寸的框。
    // 名字/距离文字画在 screenBoundsMin 的正上方，这样文字跟着框走，
    // 而不是留在物体中心 —— 大物件的标签压在中心很难对上。
    ImVec2 screenBoundsMin{0, 0};
    ImVec2 screenBoundsMax{0, 0};
    bool hasScreenBounds = false;

    // 强 GCHandle：只要我们还持有这个 GameObject，就给它加根，
    // 托管侧的 GC 就不会把它回收。gameObject/transform 这两个裸指针
    // 在此之前一直是被 GC 悬着的 —— 被回收后再拿去解引用就是随机崩溃。
    // 0 表示「没加根成功」（对象为空、或 gchandle 符号缺失）。
    //
    // 生命周期必须和对象条目严格配对：移除条目时 FreeHandle。
    uint32_t gameObjectHandle = 0;
    uint32_t transformHandle = 0;

    GameObjectInfo() = default;
    // 这两个重载不参与拷贝语义：拷贝条目时由调用方重新加根。
    GameObjectInfo(const GameObjectInfo&) = delete;
    GameObjectInfo& operator=(const GameObjectInfo&) = delete;
    GameObjectInfo(GameObjectInfo&& other) noexcept;
    GameObjectInfo& operator=(GameObjectInfo&& other) noexcept;
    ~GameObjectInfo();
};

struct DrawObject {
    GameObjectInfo target;
    ImColor color;
    float thickness = 2.0f;
    bool drawLine = true;
    bool drawBox = false;
    bool drawCircle = false;
};

class ObjectDrawManager {
private:
    static ImVec2 screenCenter;
    static bool autoRefresh;

    static void DrawLineToCenter(const DrawObject& drawObj);
    static void DrawBox(const DrawObject& drawObj);
    static void DrawCircle(const DrawObject& drawObj);

    static void RefreshCamera();
    static void CleanupInvalidDrawObjects();

public:
    static std::vector<DrawObject> drawObjects;

public:
    static bool showObjectManager;
    // 最大绘制距离（米）。<= 0 = 不限制。
    static float maxDrawDistance;
    static void Initialize();
    static void Shutdown();
    static void Tick();
    static void DrawAll();
    static void DrawUI();

    static void SelectObject(const GameObjectInfo& obj);
    static void SelectObject(Il2CppObject* gameObject);
    static void DeselectAll();
    static void AddDrawObject(const GameObjectInfo& obj);
    static void RemoveDrawObject(Il2CppObject* gameObject);
    static void ClearAllDrawObjects();
    static void UpdateGameObjects();

    static bool WorldToScreen(const Vector3& worldPos, Vector3& screenPos);
    static ImVec2 GetScreenCenter();
};

extern ObjectDrawManager g_ObjectDrawManager;