#pragma once
#include "Il2cpp/Il2cpp.h"
#include "imgui/imgui.h"
#include <vector>
#include <string>

struct Vector3 {
    float x, y, z;
    
    Vector3() : x(0), y(0), z(0) {}
    Vector3(float x, float y, float z) : x(x), y(y), z(z) {}
};

struct GameObjectInfo {
    Il2CppObject* gameObject;
    Il2CppObject* transform;
    Vector3 worldPosition;
    Vector3 screenPosition;
    std::string name;
    bool isSelected = false;
};

struct DrawObject {
    GameObjectInfo target;
    ImColor color;
    float thickness;
    bool drawLine = true;
    bool drawBox = false;
    bool drawCircle = false;
};

class ObjectDrawManager {
private:
    static std::vector<GameObjectInfo> gameObjects;
    static std::vector<DrawObject> drawObjects;
    static ImVec2 screenCenter;
    // 自动刷新开关与间隔（在渲染线程按帧节流，不走后台线程）
    static bool autoRefresh;
    static float refreshInterval;

public:
    // 显示开关：由工具页的 Checkbox 读写
    static bool showObjectManager;
    static void Initialize();
    static void Shutdown(); // 添加声明
    static void UpdateGameObjects();
    static void Tick(); // 每帧刷新（独立于 UI 是否打开），保证 ESP 持续更新
    static void DrawAll();
    static void DrawUI();
    
    static void SelectObject(const GameObjectInfo& obj);
    static void DeselectAll();
    static void AddDrawObject(const GameObjectInfo& obj);
    static void RemoveDrawObject(Il2CppObject* gameObject);
    static void ClearAllDrawObjects();
    
    static bool WorldToScreen(const Vector3& worldPos, Vector3& screenPos);
    static ImVec2 GetScreenCenter();
    
private:
    static void DrawLineToCenter(const DrawObject& drawObj);
    static void DrawBox(const DrawObject& drawObj);
    static void DrawCircle(const DrawObject& drawObj);
};

extern ObjectDrawManager g_ObjectDrawManager;