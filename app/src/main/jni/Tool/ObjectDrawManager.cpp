#include "ObjectDrawManager.h"
#include "Includes/Logger.h"
#include <algorithm>
#include <atomic>
#include <thread>
#include <mutex>
#include <unordered_map>

static std::mutex g_objectsMutex;
// 保护跨线程共享的 drawObjects / gameObjects：
// DrawAll 跑在 Unity 图形线程（swap 钩子里），UpdateGameObjects/DrawUI 跑在菜单线程，
// 两线程同时读写这两个 vector 会造成迭代器失效/UAF → SIGSEGV。
static std::mutex g_drawMutex;

std::vector<GameObjectInfo> ObjectDrawManager::gameObjects;
std::vector<DrawObject> ObjectDrawManager::drawObjects;
bool ObjectDrawManager::showObjectManager = false;
ImVec2 ObjectDrawManager::screenCenter = ImVec2(0, 0);

bool ObjectDrawManager::autoRefresh = true;

ObjectDrawManager g_ObjectDrawManager;

// 静态变量 - 基于你的 GameObjects() 函数
static std::vector<Il2CppObject*> g_AllGameObjects;
static Il2CppObject* g_MainCamera = nullptr;
static MethodInfo* g_WorldToScreenPoint = nullptr;
static MethodInfo* g_IsNativeObjectAlive = nullptr;
// 缓存的常用方法指针，避免每帧反射解析 + 便于判空
static MethodInfo* g_GetTransform = nullptr;
static MethodInfo* g_GetPosition = nullptr;
static MethodInfo* g_GetName = nullptr;
// 缓存的类指针（类不会随场景失效，对象会）
static Il2CppClass* g_CameraClass = nullptr;
static Il2CppClass* g_GameObjectClass = nullptr;

// 全对象动态绘制开关（面板里切换）
static bool g_drawAllObjects = false;
// 自动添加全部：自动刷新时把屏幕内对象并入绘制列表，新出现对象自动跟着显示
static bool g_autoAddAll = true;
// 后台重扫是否正在进行（防止刷新过快时多个重扫线程叠加，拖垮 CPU / 频繁停 GC 世界）
static std::atomic<bool> g_rescanBusy{false};

// 后台重新枚举：切换场景后旧对象指针会失效，需要周期性重扫，别长期持旧指针
void RescanGameObjectsInBackground()
{
    if (!g_GameObjectClass)
        return;
    if (g_rescanBusy.exchange(true))
        return; // 已有重扫在跑，跳过，避免叠加拖垮 CPU / 频繁停 GC 世界
    std::thread([]() {
        auto objs = Il2cpp::GC::FindObjects(g_GameObjectClass);
        {
            std::lock_guard<std::mutex> lock(g_objectsMutex);
            g_AllGameObjects = std::move(objs);
        }
        LOGD("对象绘制管理器: 重扫完成，%zu 个 GameObject", g_AllGameObjects.size());
        g_rescanBusy = false;
    }).detach();
}

void ObjectDrawManager::Initialize() {
    LOGI("初始化对象绘制管理器");

    auto GameObjectClass = Il2cpp::FindClass("UnityEngine.GameObject");
    g_GameObjectClass = GameObjectClass;
    if (GameObjectClass) {
        g_GetTransform = GameObjectClass->getMethod("get_transform");
        g_GetName = GameObjectClass->getMethod("get_name");
    }

    auto TransformClass = Il2cpp::FindClass("UnityEngine.Transform");
    if (TransformClass) {
        g_GetPosition = TransformClass->getMethod("get_position");
    }

    auto CameraClass = Il2cpp::FindClass("UnityEngine.Camera");
    g_CameraClass = CameraClass;
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

    LOGD("对象绘制管理器: 方法解析 -> transform=%p position=%p name=%p alive=%p w2s=%p",
         (void*)g_GetTransform, (void*)g_GetPosition, (void*)g_GetName,
         (void*)g_IsNativeObjectAlive, (void*)g_WorldToScreenPoint);

    // 枚举对象会触发 il2cpp liveness（内部 il2cpp_stop_gc_world 停世界），
    // 不能在渲染/主线程上直接跑，否则一放就崩。与 ClassesTab 的
    // ImGuiObjectSelector 保持一致：放到后台线程里做枚举，完成后才可用。
    if (g_GameObjectClass) {
        RescanGameObjectsInBackground();
    }
}

void ObjectDrawManager::Shutdown() {
    LOGI("关闭对象绘制管理器");
    {
        std::lock_guard<std::mutex> lock(g_objectsMutex);
        g_AllGameObjects.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_drawMutex);
        drawObjects.clear();
    }
}

// 校验裸指针是否仍指向一个有效的 GameObject。FindObjects 返回的对象在切场景 / 对象回收后
// 可能被 GC 回收并复用成别的对象，直接 invoke 会 UAF 崩在 libunity。这里读 obj->klass
// （托管堆一般仍映射，读取本身安全）：若被复用成其它类型则跳过。
// 注：UnityEngine.GameObject 是 sealed，故直接比较类指针即可。
static bool IsValidGameObject(Il2CppObject* o) {
    return o != nullptr && o->klass != nullptr && o->klass == g_GameObjectClass;
}

void ObjectDrawManager::UpdateGameObjects() {
    try {
    // 场景可能已切换，每轮先重取当前 Camera.main（类指针不会失效，对象会）
    if (g_CameraClass) {
        g_MainCamera = g_CameraClass->invoke_static_method<Il2CppObject*>("get_main");
        // 校验相机指针仍有效，避免把悬空相机传进 WorldToScreenPoint 崩在 libunity
        if (g_MainCamera && g_MainCamera->klass != g_CameraClass)
            g_MainCamera = nullptr;
    }

    if (!g_MainCamera || !g_WorldToScreenPoint || !g_IsNativeObjectAlive ||
        !g_GetTransform || !g_GetPosition || !g_GetName) {
        LOGW("对象绘制管理器: 缺少方法指针，跳过本轮刷新");
        return;
    }

    // 快照对象列表（后台枚举线程可能正在写入 g_AllGameObjects）
    std::vector<Il2CppObject*> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_objectsMutex);
        snapshot = g_AllGameObjects;
    }

    ImVec2 screenCenter = GetScreenCenter();

    // 实时全量扫描：每轮把快照里所有对象都投影一遍（真正的实时刷新）。
    // 每次 invoke 前先 IsValidGameObject 校验裸指针 —— 切场景后失效对象会被 GC 回收并
    // 复用成别的对象，直接 invoke 就 UAF 崩在 libunity；先读 klass 把这类指针挡掉。
    std::vector<GameObjectInfo> newObjects;
    newObjects.reserve(snapshot.size());
    for (auto go : snapshot) {
        try {
            if (!IsValidGameObject(go)) continue;
            // 检查对象是否存活
            if (g_IsNativeObjectAlive->invoke_static<bool>(go) == false)
                continue;

            GameObjectInfo info;
            info.gameObject = go;

            // 获取 transform
            info.transform = go->invoke_method<Il2CppObject*>(g_GetTransform);
            if (!info.transform) continue;

            // 获取世界位置
            auto position = info.transform->invoke_method<Vector3>(g_GetPosition);
            info.worldPosition = position;

            // 转换为屏幕坐标
            auto screen = g_WorldToScreenPoint->invoke_static<Vector3>(g_MainCamera, position);
            // WorldToScreenPoint 左下原点 → ImGui 左上原点：y 翻转
            screen.y = ImGui::GetIO().DisplaySize.y - screen.y;
            info.screenPosition = screen;

            // 只保留在屏幕内的对象
            if (info.screenPosition.z > 0 &&
                info.screenPosition.x >= 0 && info.screenPosition.x <= screenCenter.x * 2 &&
                info.screenPosition.y >= 0 && info.screenPosition.y <= screenCenter.y * 2) {
                // 名字仅屏内对象才取：避免对场景里全部对象做字符串分配，减少 GC 抖动卡顿
                auto nameObj = go->invoke_method<Il2CppObject*>(g_GetName);
                if (nameObj) {
                    auto nameStr = reinterpret_cast<Il2CppString*>(nameObj);
                    if (nameStr) {
                        // GetChars 返回 UTF-16，按 char* 截断在第一个 NUL 就断了，名字会空
                        info.name = nameStr->to_string();
                    }
                }
                newObjects.push_back(info);
            }
        } catch (const std::exception& e) {
            LOGD("处理GameObject时出错: %s", e.what());
        } catch (...) {
            LOGW("处理GameObject时出错: 未知异常，已跳过");
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_drawMutex);
        gameObjects = std::move(newObjects);
    }

    // 自动添加：把当前在屏物体并入绘制列表（去重），新生成的会自动出现
    // 去重交给 SelectObject 内部完成（它持锁），这里不再手动遍历 drawObjects，
    // 避免和图形线程的 RemoveDrawObject 并发读改写造成迭代器失效。
    if (g_autoAddAll) {
        for (auto& info : gameObjects) {
            SelectObject(info);
        }
    }

    // 同步已绘制对象的坐标：屏内对象直接复用本轮已经投影好的结果（零额外 invoke），
    // 仅对屏外/已失效对象做一次判活+投影；这样既砍掉原先每刷新一次的“二次投影”卡顿，
    // 又能在切场景后及时剔除失效对象。绘制路径 DrawAll 依旧绝不 invoke，避免 UAF。
    {
        std::lock_guard<std::mutex> lock(g_drawMutex);
        std::unordered_map<Il2CppObject*, const GameObjectInfo*> screenMap;
        for (const auto& g : gameObjects)
            screenMap[g.gameObject] = &g;

        for (size_t i = drawObjects.size(); i-- > 0; ) {
            auto& target = drawObjects[i].target;
            auto it = screenMap.find(target.gameObject);
            if (it != screenMap.end()) {
                const GameObjectInfo& g = *it->second;
                target.worldPosition = g.worldPosition;
                target.screenPosition = g.screenPosition;
                target.name = g.name;
                continue;
            }
            // 屏外或已失效：先验 klass，再判活，最后投影；任一不过就移除
            try {
                if (!IsValidGameObject(target.gameObject)) {
                    drawObjects.erase(drawObjects.begin() + i);
                    continue;
                }
                if (g_IsNativeObjectAlive->invoke_static<bool>(target.gameObject) == false) {
                    drawObjects.erase(drawObjects.begin() + i);
                    continue;
                }
                auto transform = target.gameObject->invoke_method<Il2CppObject*>(g_GetTransform);
                if (!transform) {
                    drawObjects.erase(drawObjects.begin() + i);
                    continue;
                }
                auto position = transform->invoke_method<Vector3>(g_GetPosition);
                target.worldPosition = position;
                auto screen = g_WorldToScreenPoint->invoke_static<Vector3>(g_MainCamera, position);
                // WorldToScreenPoint 左下原点 → ImGui 左上原点：y 翻转
                screen.y = ImGui::GetIO().DisplaySize.y - screen.y;
                target.screenPosition = screen;
                auto nameObj = target.gameObject->invoke_method<Il2CppObject*>(g_GetName);
                if (nameObj) {
                    auto s = reinterpret_cast<Il2CppString*>(nameObj);
                    if (s)
                        target.name = s->to_string();
                }
            } catch (const std::exception& e) {
                LOGD("同步绘制对象出错(已移除): %s", e.what());
                drawObjects.erase(drawObjects.begin() + i);
            } catch (...) {
                LOGW("同步绘制对象未知异常(已移除)");
                drawObjects.erase(drawObjects.begin() + i);
            }
        }
    }
    } catch (const std::exception& e) {
        LOGW("UpdateGameObjects 异常: %s", e.what());
    } catch (...) {
        LOGW("UpdateGameObjects 未知异常，跳过本轮刷新");
    }
}

void ObjectDrawManager::DrawAll() {
    auto drawList = ImGui::GetForegroundDrawList();

    // 绘制只使用刷新阶段(UpdateGameObjects)已经算好的屏幕坐标，
    // 这里绝不再 invoke il2cpp。否则切场景后悬空指针会在 libunity 里 UAF 崩，且
    // SIGSEGV 不是 C++ 异常，try/catch 拦不住。
    std::vector<DrawObject> localDraw;
    std::vector<GameObjectInfo> localAll;
    {
        std::lock_guard<std::mutex> lock(g_drawMutex);
        localDraw = drawObjects;
        if (g_drawAllObjects)
            localAll = gameObjects;
    }

    // 选中的对象：用刷新阶段投影好的坐标画（最多 0.1s 旧，足够跟随，且绝不触发 il2cpp）
    for (auto& drawObj : localDraw) {
        if (drawObj.target.screenPosition.z <= 0)
            continue;
        if (drawObj.drawLine) {
            DrawLineToCenter(drawObj);
        }
        if (drawObj.drawBox) {
            DrawBox(drawObj);
        }
        if (drawObj.drawCircle) {
            DrawCircle(drawObj);
        }
        if (!drawObj.target.name.empty()) {
            ImVec2 namePos(drawObj.target.screenPosition.x, drawObj.target.screenPosition.y - 18);
            drawList->AddText(namePos, drawObj.color, drawObj.target.name.c_str());
        }
    }

    // 全对象动态：用已刷新、且已判活的 gameObjects 来画，不含任何 il2cpp 调用。
    if (g_drawAllObjects) {
        for (const auto& info : localAll) {
            if (info.screenPosition.z <= 0)
                continue;
            DrawObject esp;
            esp.target = info;
            esp.color = IM_COL32(255, 255, 0, 220);
            esp.thickness = 2.0f;
            esp.drawLine = true;
            esp.drawBox = true;
            esp.drawCircle = false;
            DrawLineToCenter(esp);
            DrawBox(esp);
            if (!info.name.empty()) {
                ImVec2 namePos(info.screenPosition.x, info.screenPosition.y - 20);
                drawList->AddText(namePos, IM_COL32(255, 255, 0, 220), info.name.c_str());
            }
        }
    }
}

void ObjectDrawManager::Tick() {
    if (autoRefresh) {
        UpdateGameObjects();
    }
    static float nextRescan = 0.f;
    float now = ImGui::GetTime();
    if (autoRefresh && now >= nextRescan) {
        nextRescan = now + 0.5f;
        RescanGameObjectsInBackground();
    }
}

void ObjectDrawManager::DrawUI() {
    // 配置 UI 内容（不再单独开浮窗，整合进主窗口的标签页）。
    // 拷一份快照用于本帧 UI 展示/遍历。图形线程(DrawAll)可能在后台并发
    // RemoveDrawObject，直接遍历 drawObjects 会迭代器失效→崩，故此处先持锁拷贝。
    std::vector<DrawObject> drawSnapshot;
    std::vector<GameObjectInfo> gameSnapshot;
    {
        std::lock_guard<std::mutex> lock(g_drawMutex);
        drawSnapshot = drawObjects;
        gameSnapshot = gameObjects;
    }

    ImGui::Text("对象绘制管理器");
        ImGui::Separator();
        
        // 控制面板
        ImGui::Columns(2, "controls", false);
        
        // 左侧控制
        ImGui::Checkbox("自动刷新", &autoRefresh);
        ImGui::Checkbox("自动添加全部", &g_autoAddAll);
        ImGui::Checkbox("全对象动态绘制", &g_drawAllObjects);
        
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
        ImGui::Text("屏幕内对象: %zu", gameSnapshot.size());
        ImGui::Text("已绘制对象: %zu", drawSnapshot.size());

        ImGui::Separator();

        // 添加对象：从列表点选 / 全选（不再点屏幕，那会把点击漏进游戏）
        if (ImGui::Button("全部添加")) {
            for (const auto& obj : gameSnapshot) {
                SelectObject(obj);
            }
        }
        ImGui::SameLine();
        static char objFilter[64] = {0};
        ImGui::InputText("筛选", objFilter, sizeof(objFilter));
        ImGui::BeginChild("##ObjAddList", ImVec2(0, 220));
        for (const auto& obj : gameSnapshot) {
            if (objFilter[0] && obj.name.find(objFilter) == std::string::npos)
                continue;
            bool already = false;
            for (const auto& d : drawSnapshot) {
                if (d.target.gameObject == obj.gameObject) { already = true; break; }
            }
            ImGui::PushID(obj.gameObject);
            if (ImGui::Selectable(((already ? "[已选] " : "") + obj.name).c_str(), already)) {
                if (!already) SelectObject(obj);
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::Separator();

        // 已绘制的对象
        ImGui::Text("已绘制的对象:");
        if (drawSnapshot.empty()) {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "暂无绘制对象");
        } else {
            for (size_t i = 0; i < drawSnapshot.size(); i++) {
                ImGui::PushID(i);
                
                ImGui::Text("%s", drawSnapshot[i].target.name.c_str());
                ImGui::SameLine();
                
                // 颜色预览
                ImVec4 color = ImGui::ColorConvertU32ToFloat4(drawSnapshot[i].color);
                ImGui::ColorButton("##color", color, ImGuiColorEditFlags_NoTooltip, ImVec2(20, 20));
                
                ImGui::SameLine();
                if (ImGui::Button("移除")) {
                    RemoveDrawObject(drawSnapshot[i].target.gameObject);
                    ImGui::PopID();
                    break;
                }
                
            ImGui::PopID();
        }
    }
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

void ObjectDrawManager::SelectObject(const GameObjectInfo& obj) {
    LOGD("选择对象: %s", obj.name.c_str());

    std::lock_guard<std::mutex> lock(g_drawMutex);

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
    std::lock_guard<std::mutex> lock(g_drawMutex);
    for (auto& obj : gameObjects) {
        obj.isSelected = false;
    }
}

void ObjectDrawManager::AddDrawObject(const GameObjectInfo& obj) {
    SelectObject(obj);
}

void ObjectDrawManager::RemoveDrawObject(Il2CppObject* gameObject) {
    std::lock_guard<std::mutex> lock(g_drawMutex);
    drawObjects.erase(
        std::remove_if(drawObjects.begin(), drawObjects.end(),
                       [gameObject](const DrawObject& obj) {
                           return obj.target.gameObject == gameObject;
                       }),
        drawObjects.end()
    );
}

void ObjectDrawManager::ClearAllDrawObjects() {
    std::lock_guard<std::mutex> lock(g_drawMutex);
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