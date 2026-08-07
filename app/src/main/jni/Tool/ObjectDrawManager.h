#pragma once
#include "Il2cpp/Il2cpp.h"
#include "imgui/imgui.h"
#include <vector>
#include <string>
#include <functional>
#include <atomic>
#include <thread>

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
    static bool showObjectManager;
    static ImVec2 screenCenter;
    
    // 添加缺失的成员变量
    static bool isRunning;
    static bool autoRefresh;
    static float refreshInterval;
    static std::atomic<bool> updating;
    static std::thread updateThread;
    
public:
    static void Initialize();
    static void Shutdown(); // 添加声明
    static void UpdateGameObjects();
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
    static void StartAutoRefresh(); // 添加声明
    static void StopAutoRefresh();  // 添加声明
    
    static void DrawLineToCenter(const DrawObject& drawObj);
    static void DrawBox(const DrawObject& drawObj);
    static void DrawCircle(const DrawObject& drawObj);
    static void DrawObjectInfo(const GameObjectInfo& obj, ImVec2 screenPos);
};

extern ObjectDrawManager g_ObjectDrawManager;