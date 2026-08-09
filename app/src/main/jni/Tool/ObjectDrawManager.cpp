#include "ObjectDrawManager.h"
#include "Includes/Logger.h"
#include <algorithm>

std::vector<GameObjectInfo> ObjectDrawManager::gameObjects;
std::vector<DrawObject> ObjectDrawManager::drawObjects;
bool ObjectDrawManager::showObjectManager = false;
ImVec2 ObjectDrawManager::screenCenter = ImVec2(0, 0);

bool ObjectDrawManager::autoRefresh = true;
float ObjectDrawManager::refreshInterval = 0.1f;

ObjectDrawManager g_ObjectDrawManager;

// 静态变量 - 基于你的 GameObjects() 函数
static std::vector<Il2CppObject*> g_AllGameObjects;
static Il2CppObject* g_MainCamera = nullptr;
static MethodInfo* g_WorldToScreenPoint = nullptr;
static MethodInfo* g_IsNativeObjectAlive = nullptr;

void ObjectDrawManager::Initialize() {
    LOGI("初始化对象绘制管理器");

    // 初始化静态资源 - 基于你的 GameObjects() 函数
    auto GameObjectClass = Il2cpp::FindClass("UnityEngine.GameObject");
    if (GameObjectClass) {
        g_AllGameObjects = Il2cpp::GC::FindObjects(GameObjectClass);
    }
    
    auto CameraClass = Il2cpp::FindClass("UnityEngine.Camera");
    if (CameraClass) {
        g_MainCamera = CameraClass->invoke_static_method<Il2CppObject*>("get_main");
        if (g_MainCamera) {
            auto methods = g_MainCamera->klass->getMethods("WorldToScreenPoint");
            if (methods.size() > 1) {
                g_WorldToScreenPoint = methods[1];
            }
        }
    }
    
    auto UnityObject = Il2cpp::FindClass("UnityEngine.Object");
    if (UnityObject) {
        g_IsNativeObjectAlive = UnityObject->getMethod("IsNativeObjectAlive");
    }
}

void ObjectDrawManager::Shutdown() {
    LOGI("关闭对象绘制管理器");
    g_AllGameObjects.clear();
    drawObjects.clear();
}

void ObjectDrawManager::UpdateGameObjects() {
    std::vector<GameObjectInfo> newObjects;
    
    if (!g_MainCamera || !g_WorldToScreenPoint || !g_IsNativeObjectAlive) {
        return;
    }
    
    ImVec2 screenCenter = GetScreenCenter();
    
    for (auto go : g_AllGameObjects) {
        try {
            // 检查对象是否存活 - 使用你的方式
            if (g_IsNativeObjectAlive->invoke_static<bool>(go) == false) {
                continue;
            }
            
            GameObjectInfo info;
            info.gameObject = go;
            
            // 获取 transform - 使用你的方式
            info.transform = go->invoke_method<Il2CppObject*>("get_transform");
            if (!info.transform) continue;
            
            // 获取世界位置 - 使用你的方式
            auto position = info.transform->invoke_method<Vector3>("get_position");
            info.worldPosition = position;
            
            // 转换为屏幕坐标 - 使用你的方式
            auto screen = g_WorldToScreenPoint->invoke_static<Vector3>(g_MainCamera, position);
            info.screenPosition = screen;
            
            // 获取对象名称 - 使用你的方式
            auto nameObj = go->invoke_method<Il2CppObject*>("get_name");
            if (nameObj) {
                Il2CppString* nameStr = reinterpret_cast<Il2CppString*>(nameObj);
                if (nameStr) {
                    const char* nameChars = Il2cpp::GetChars(nameStr);
                    if (nameChars) {
                        info.name = std::string(nameChars);
                    }
                }
            }
            
            // 只添加在屏幕内的对象
            if (info.screenPosition.z > 0 && 
                info.screenPosition.x >= 0 && info.screenPosition.x <= screenCenter.x * 2 &&
                info.screenPosition.y >= 0 && info.screenPosition.y <= screenCenter.y * 2) {
                newObjects.push_back(info);
            }
        } catch (const std::exception& e) {
            LOGD("处理GameObject时出错: %s", e.what());
        }
    }
    
    gameObjects = std::move(newObjects);
}

void ObjectDrawManager::DrawAll() {
    auto drawList = ImGui::GetForegroundDrawList();
    ImVec2 screenCenter = GetScreenCenter();
    
    // 绘制所有选中的对象
    for (const auto& drawObj : drawObjects) {
        if (drawObj.drawLine) {
            DrawLineToCenter(drawObj);
        }
        if (drawObj.drawBox) {
            DrawBox(drawObj);
        }
        if (drawObj.drawCircle) {
            DrawCircle(drawObj);
        }
    }
    
    // 绘制所有对象的可选按钮
    for (const auto& obj : gameObjects) {
        DrawObjectInfo(obj, ImVec2(obj.screenPosition.x, obj.screenPosition.y));
    }
}

void ObjectDrawManager::DrawUI() {
    static bool showObjectDrawer = true;
    
    if (ImGui::Begin("对象绘制管理器", &showObjectDrawer)) {
        // 节流自动刷新：在渲染线程按帧执行，避免后台线程调 il2cpp 造成崩溃
        static float lastRefresh = 0.f;
        float now = ImGui::GetTime();
        if (autoRefresh && (now - lastRefresh) >= refreshInterval) {
            lastRefresh = now;
            UpdateGameObjects();
        }

        ImGui::Text("对象绘制管理器");
        ImGui::Separator();
        
        // 控制面板
        ImGui::Columns(2, "controls", false);
        
        // 左侧控制
        ImGui::Checkbox("自动刷新", &autoRefresh);
        ImGui::SliderFloat("刷新间隔(s)", &refreshInterval, 0.05f, 1.0f, "%.2f");
        
        ImGui::NextColumn();
        
        // 右侧按钮
        if (ImGui::Button("手动刷新")) {
            UpdateGameObjects();
        }
        ImGui::SameLine();
        if (ImGui::Button("清除所有")) {
            ClearAllDrawObjects();
        }
        
        ImGui::Columns(1);
        ImGui::Separator();
        
        // 统计信息
        ImGui::Text("统计信息:");
        ImGui::Text("屏幕内对象: %zu", gameObjects.size());
        ImGui::Text("已绘制对象: %zu", drawObjects.size());
        
        ImGui::Separator();
        
        // 绘制对象列表
        ImGui::Text("已绘制的对象:");
        if (drawObjects.empty()) {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "暂无绘制对象");
            ImGui::Text("点击屏幕中的对象名称来添加绘制");
        } else {
            for (size_t i = 0; i < drawObjects.size(); i++) {
                ImGui::PushID(i);
                
                ImGui::Text("%s", drawObjects[i].target.name.c_str());
                ImGui::SameLine();
                
                // 颜色预览
                ImVec4 color = ImGui::ColorConvertU32ToFloat4(drawObjects[i].color);
                ImGui::ColorButton("##color", color, ImGuiColorEditFlags_NoTooltip, ImVec2(20, 20));
                
                ImGui::SameLine();
                if (ImGui::Button("移除")) {
                    RemoveDrawObject(drawObjects[i].target.gameObject);
                    ImGui::PopID();
                    break;
                }
                
                ImGui::PopID();
            }
        }
    }
    ImGui::End();
}

void ObjectDrawManager::DrawLineToCenter(const DrawObject& drawObj) {
    auto drawList = ImGui::GetForegroundDrawList();
    ImVec2 targetPos(drawObj.target.screenPosition.x, drawObj.target.screenPosition.y);
    ImVec2 screenCenter = GetScreenCenter();
    
    // 绘制从对象指向屏幕中心的线
    drawList->AddLine(targetPos, screenCenter, drawObj.color, drawObj.thickness);
    
    // 在对象位置绘制一个小圆点
    drawList->AddCircleFilled(targetPos, 5.0f, drawObj.color);
    
    // 在屏幕中心绘制十字准星
    float crossSize = 10.0f;
    drawList->AddLine(ImVec2(screenCenter.x - crossSize, screenCenter.y), 
                     ImVec2(screenCenter.x + crossSize, screenCenter.y), drawObj.color, 2.0f);
    drawList->AddLine(ImVec2(screenCenter.x, screenCenter.y - crossSize), 
                     ImVec2(screenCenter.x, screenCenter.y + crossSize), drawObj.color, 2.0f);
}

void ObjectDrawManager::DrawBox(const DrawObject& drawObj) {
    auto drawList = ImGui::GetForegroundDrawList();
    ImVec2 pos(drawObj.target.screenPosition.x, drawObj.target.screenPosition.y);
    float boxSize = 20.0f;
    
    drawList->AddRect(ImVec2(pos.x - boxSize, pos.y - boxSize),
                     ImVec2(pos.x + boxSize, pos.y + boxSize),
                     drawObj.color, 0.0f, 0, drawObj.thickness);
}

void ObjectDrawManager::DrawCircle(const DrawObject& drawObj) {
    auto drawList = ImGui::GetForegroundDrawList();
    ImVec2 pos(drawObj.target.screenPosition.x, drawObj.target.screenPosition.y);
    float radius = 15.0f;
    
    drawList->AddCircle(pos, radius, drawObj.color, 0, drawObj.thickness);
}

void ObjectDrawManager::DrawObjectInfo(const GameObjectInfo& obj, ImVec2 screenPos) {
    auto drawList = ImGui::GetForegroundDrawList();
    
    // 创建可点击区域
    float buttonSize = 30.0f;
    ImVec2 buttonMin(screenPos.x - buttonSize/2, screenPos.y - buttonSize/2);
    ImVec2 buttonMax(screenPos.x + buttonSize/2, screenPos.y + buttonSize/2);
    
    // 绘制半透明背景
    drawList->AddRectFilled(buttonMin, buttonMax, IM_COL32(0, 0, 0, 128));
    drawList->AddRect(buttonMin, buttonMax, IM_COL32(255, 255, 255, 200), 0.0f, 0, 1.0f);
    
    // 绘制对象名称
    std::string displayName = obj.name.length() > 10 ? obj.name.substr(0, 10) + "..." : obj.name;
    ImVec2 textSize = ImGui::CalcTextSize(displayName.c_str());
    ImVec2 textPos(screenPos.x - textSize.x/2, screenPos.y - 25);
    
    drawList->AddRectFilled(ImVec2(textPos.x - 2, textPos.y - 2),
                           ImVec2(textPos.x + textSize.x + 2, textPos.y + textSize.y + 2),
                           IM_COL32(0, 0, 0, 200));
    drawList->AddText(textPos, IM_COL32(255, 255, 255, 255), displayName.c_str());
    
    // 检查点击
    if (ImGui::IsMouseClicked(0)) {
        ImVec2 mousePos = ImGui::GetMousePos();
        if (mousePos.x >= buttonMin.x && mousePos.x <= buttonMax.x &&
            mousePos.y >= buttonMin.y && mousePos.y <= buttonMax.y) {
            SelectObject(obj);
        }
    }
}

void ObjectDrawManager::SelectObject(const GameObjectInfo& obj) {
    LOGD("选择对象: %s", obj.name.c_str());
    
    // 检查是否已经存在
    for (const auto& drawObj : drawObjects) {
        if (drawObj.target.gameObject == obj.gameObject) {
            LOGD("对象已存在，跳过添加");
            return;
        }
    }
    
    // 添加新的绘制对象
    DrawObject newDrawObj;
    newDrawObj.target = obj;
    newDrawObj.color = IM_COL32(rand() % 255, rand() % 255, rand() % 255, 255);
    newDrawObj.thickness = 2.0f;
    newDrawObj.drawLine = true;
    newDrawObj.drawBox = true;
    newDrawObj.drawCircle = false;
    
    drawObjects.push_back(newDrawObj);
    LOGD("已添加绘制对象: %s", obj.name.c_str());
}

void ObjectDrawManager::DeselectAll() {
    // 实现取消选择所有对象
    for (auto& obj : gameObjects) {
        obj.isSelected = false;
    }
}

void ObjectDrawManager::AddDrawObject(const GameObjectInfo& obj) {
    SelectObject(obj);
}

void ObjectDrawManager::RemoveDrawObject(Il2CppObject* gameObject) {
    drawObjects.erase(
        std::remove_if(drawObjects.begin(), drawObjects.end(),
                      [gameObject](const DrawObject& obj) {
                          return obj.target.gameObject == gameObject;
                      }),
        drawObjects.end()
    );
}

void ObjectDrawManager::ClearAllDrawObjects() {
    drawObjects.clear();
}

bool ObjectDrawManager::WorldToScreen(const Vector3& worldPos, Vector3& screenPos) {
    if (!g_MainCamera || !g_WorldToScreenPoint) return false;
    
    try {
        auto screen = g_WorldToScreenPoint->invoke_static<Vector3>(g_MainCamera, worldPos);
        screenPos = screen;
        return screenPos.z > 0;
    } catch (...) {
        return false;
    }
}

ImVec2 ObjectDrawManager::GetScreenCenter() {
    auto displaySize = ImGui::GetIO().DisplaySize;
    return ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f);
}