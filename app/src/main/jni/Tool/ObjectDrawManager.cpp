qalse;
ImVec2 ObjectDrawManager::screenCenter = ImVec2(0, 0);
bool ObjectDrawManager::autoRefresh = true;

ObjectDrawManager g_ObjectDrawManager;

static MethodInfo* g_GetTransform = nullptr;
static MethodInfo* g_GetPosition = nullptr;
static MethodInfo* g_GetName = nullptr;
static Il2CppClass* g_CameraClass = nullptr;
static Il2CppClass* g_GameObjectClass = nullptr;

static bool g_drawAllObjects = false;
static bool g_autoAddAll = false;

static Il2CppObject* g_MainCamera = nullptr;
static MethodInfo* g_WorldToScreenPoint = nullptr;
static MethodInfo* g_IsNativeObjectAlive = nullptr;

static std::vector<Il2CppObject*> g_cachedGameObjects;
static std::atomic<bool> g_needsRescan{false};
static std::atomic<bool> g_hasNewList{false};
static std::atomic<bool> g_rescanBusy{false};
static std::atomic<bool> g_rescanInProgress{false};
static std::atomic<size_t> g_lastObjectCount{0};
static std::atomic<float> g_rescanFinishTime{0.f};

static bool IsValidGameObject(Il2CppObject* o) {
    return o != nullptr && o->klass != nullptr && o->klass == g_GameObjectClass;
}

static void RescanGameObjectsInBackground() {
    if (!g_GameObjectClass || g_rescanBusy.exchange(true))
        return;
    g_rescanInProgress.store(true);
    std::thread([]() {
        auto objs = Il2cpp::GC::FindObjects(g_GameObjectClass);
        {
            std::lock_guard<std::mutex> lock(g_objectsMutex);
            g_cachedGameObjects = std::move(objs);
        }
        g_rescanFinishTime.store(ImGui::GetTime());
        g_hasNewList.store(true);
        g_rescanBusy.store(false);
        g_rescanInProgress.store(false);
    }).detach();
}

void ObjectDrawManager::ProcessObjectList() {
    try {
    std::vector<Il2CppObject*> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_objectsMutex);
        snapshot = g_cachedGameObjects;
    }

    if (!g_CameraClass) return;
    auto mainCamera = g_CameraClass->invoke_static_method<Il2CppObject*>("get_main");
    if (!mainCamera || mainCamera->klass != g_CameraClass) return;
    if (!g_WorldToScreenPoint || !g_GetTransform || !g_GetPosition || !g_GetName || !g_IsNativeObjectAlive) return;

    size_t prevCount = g_lastObjectCount.load();
    size_t currCount = snapshot.size();

    if (prevCount > 0 && currCount > 0) {
        size_t minCount = std::min(prevCount, currCount);
        size_t maxCount = std::max(prevCount, currCount);
        if (maxCount > 0 && (double)minCount / (double)maxCount < 0.5) {
            LOGW("检测到场景切换: 对象数从 %zu 变为 %zu，清空绘制列表", prevCount, currCount);
            std::lock_guard<std::mutex> lock(g_drawMutex);
            drawObjects.clear();
        }
    }

    std::vector<GameObjectInfo> infos;
    infos.reserve(snapshot.size());
    for (auto go : snapshot) {
        try {
            if (!IsValidGameObject(go)) continue;
            if (g_IsNativeObjectAlive->invoke_static<bool>(go) == false)
                continue;

            GameObjectInfo info;
            info.gameObject = go;
            auto transform = go->invoke_method<Il2CppObject*>(g_GetTransform);
            if (!transform) continue;
            auto position = transform->invoke_method<Vector3>(g_GetPosition);
            info.worldPosition = position;
            auto screen = g_WorldToScreenPoint->invoke_static<Vector3>(mainCamera, position);
            screen.y = ImGui::GetIO().DisplaySize.y - screen.y;
            info.screenPosition = screen;

            auto nameObj = go->invoke_method<Il2CppObject*>(g_GetName);
            if (nameObj) {
                auto nameStr = reinterpret_cast<Il2CppString*>(nameObj);
                if (nameStr) {
                    info.name = nameStr->to_string();
                }
            }

            if (info.screenPosition.z > 0) {
                infos.push_back(info);
            }
        } catch (const std::exception& e) {
            LOGD("ProcessObjectList: 处理对象异常: %s", e.what());
        } catch (...) {
            LOGW("ProcessObjectList: 处理对象未知异常，已跳过");
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_drawMutex);
        gameObjects = std::move(infos);
    }
    g_lastObjectCount.store(currCount);

    if (g_autoAddAll) {
        std::lock_guard<std::mutex> lock(g_drawMutex);
        for (const auto& info : gameObjects) {
            bool exists = false;
            for (const auto& d : drawObjects) {
                if (d.target.gameObject == info.gameObject) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                DrawObject newDrawObj;
                newDrawObj.target = info;
                newDrawObj.color = IM_COL32(rand() % 255, rand() % 255, rand() % 255, 255);
                newDrawObj.thickness = 2.0f;
                newDrawObj.drawLine = true;
                newDrawObj.drawBox = true;
                newDrawObj.drawCircle = false;
                drawObjects.push_back(newDrawObj);
            }
        }
    }
    } catch (const std::exception& e) {
        LOGW("ProcessObjectList 异常: %s", e.what());
    } catch (...) {
        LOGW("ProcessObjectList 未知异常，已跳过");
    }
}

void ObjectDrawManager::RefreshDrawObjectsCoordinates() {
    if (!g_CameraClass) return;
    auto mainCamera = g_CameraClass->invoke_static_method<Il2CppObject*>("get_main");
    if (!mainCamera || mainCamera->klass != g_CameraClass) return;

    std::lock_guard<std::mutex> lock(g_drawMutex);

    std::unordered_map<Il2CppObject*, const GameObjectInfo*> screenMap;
    for (const auto& g : gameObjects) {
        screenMap[g.gameObject] = &g;
    }

    for (size_t i = drawObjects.size(); i-- > 0; ) {
        auto& target = drawObjects[i].target;
        auto go = target.gameObject;
        auto it = screenMap.find(go);
        if (it != screenMap.end()) {
            const GameObjectInfo& g = *it->second;
            target.worldPosition = g.worldPosition;
            target.screenPosition = g.screenPosition;
            target.name = g.name;
        } else {
            target.screenPosition.z = -1;
        }
    }
}

void ObjectDrawManager::Tick() {
    if (g_rescanInProgress.load()) return;

    static float lastRescan = 0.f;
    float now = ImGui::GetTime();
    if (g_needsRescan.exchange(false) || (autoRefresh && now - lastRescan > 5.0f)) {
        lastRescan = now;
        RescanGameObjectsInBackground();
    }

    if (g_hasNewList.load() && (now - g_rescanFinishTime.load() > 1.0f)) {
        g_hasNewList.store(false);
        ProcessObjectList();
    }

    if (autoRefresh) {
        RefreshDrawObjectsCoordinates();
    }
}

void ObjectDrawManager::DrawAll() {
    auto drawList = ImGui::GetForegroundDrawList();
    std::lock_guard<std::mutex> lock(g_drawMutex);
    for (const auto& drawObj : drawObjects) {
        if (drawObj.target.screenPosition.z <= 0)
            continue;
        float x = drawObj.target.screenPosition.x;
        float y = drawObj.target.screenPosition.y;
        if (std::isnan(x) || std::isnan(y) || std::isinf(x) || std::isinf(y))
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
            ImVec2 namePos(x, y - 18);
            drawList->AddText(namePos, drawObj.color, drawObj.target.name.c_str());
        }
    }

    if (g_drawAllObjects) {
        for (const auto& info : gameObjects) {
            if (info.screenPosition.z <= 0)
                continue;
            float x = info.screenPosition.x;
            float y = info.screenPosition.y;
            if (std::isnan(x) || std::isnan(y) || std::isinf(x) || std::isinf(y))
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
                ImVec2 namePos(x, y - 20);
                drawList->AddText(namePos, IM_COL32(255, 255, 0, 220), info.name.c_str());
            }
        }
    }
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
}

void ObjectDrawManager::Shutdown() {
    LOGI("关闭对象绘制管理器");
    {
        std::lock_guard<std::mutex> lock(g_objectsMutex);
        g_cachedGameObjects.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_drawMutex);
        drawObjects.clear();
        gameObjects.clear();
    }
    g_lastObjectCount.store(0);
    g_rescanInProgress.store(false);
    g_needsRescan.store(false);
}

void ObjectDrawManager::SelectObject(const GameObjectInfo& obj) {
    std::lock_guard<std::mutex> lock(g_drawMutex);
    for (const auto& drawObj : drawObjects) {
        if (drawObj.target.gameObject == obj.gameObject) {
            return;
        }
    }

    DrawObject newDrawObj;
    newDrawObj.target = obj;
    newDrawObj.color = IM_COL32(rand() % 255, rand() % 255, rand() % 255, 255);
    newDrawObj.thickness = 2.0f;
    newDrawObj.drawLine = true;
    newDrawObj.drawBox = true;
    newDrawObj.drawCircle = false;

    drawObjects.push_back(newDrawObj);
}

void ObjectDrawManager::SelectObject(Il2CppObject* gameObject) {
    if (!IsValidGameObject(gameObject))
        return;

    std::lock_guard<std::mutex> lock(g_drawMutex);
    for (const auto& drawObj : drawObjects) {
        if (drawObj.target.gameObject == gameObject) {
            return;
        }
    }

    std::string name;
    if (g_GetName) {
        try {
            auto nameObj = gameObject->invoke_method<Il2CppObject*>(g_GetName);
            if (nameObj) {
                auto nameStr = reinterpret_cast<Il2CppString*>(nameObj);
                if (nameStr) {
                    name = nameStr->to_string();
                }
            }
        } catch (...) {}
    }

    DrawObject newDrawObj;
    newDrawObj.target.gameObject = gameObject;
    newDrawObj.target.name = std::move(name);
    newDrawObj.color = IM_COL32(rand() % 255, rand() % 255, rand() % 255, 255);
    newDrawObj.thickness = 2.0f;
    newDrawObj.drawLine = true;
    newDrawObj.drawBox = true;
    newDrawObj.drawCircle = false;

    drawObjects.push_back(newDrawObj);
}

void ObjectDrawManager::DeselectAll() {
    std::lock_guard<std::mutex> lock(g_drawMutex);
    for (auto& obj : gameObjects) {
        obj.isSelected = false;
    }
    drawObjects.clear();
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

void ObjectDrawManager::UpdateGameObjects() {
    g_needsRescan = true;
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

void ObjectDrawManager::DrawLineToCenter(const DrawObject& drawObj) {
    auto drawList = ImGui::GetForegroundDrawList();
    ImVec2 targetPos(drawObj.target.screenPosition.x, drawObj.target.screenPosition.y);
    ImVec2 screenCenter = GetScreenCenter();
    drawList->AddLine(targetPos, screenCenter, drawObj.color, drawObj.thickness);
    drawList->AddCircleFilled(targetPos, 5.0f, drawObj.color);
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

void ObjectDrawManager::DrawUI() {
    std::vector<DrawObject> drawSnapshot;
    std::vector<GameObjectInfo> gameSnapshot;
    {
        std::lock_guard<std::mutex> lock(g_drawMutex);
        drawSnapshot = drawObjects;
        gameSnapshot = gameObjects;
    }

    ImGui::Text("对象绘制管理器");
    ImGui::Separator();
    
    ImGui::Columns(2, "controls", false);
    
    ImGui::Checkbox("自动刷新", &autoRefresh);
    ImGui::Checkbox("自动添加全部", &g_autoAddAll);
    ImGui::Checkbox("全对象动态绘制", &g_drawAllObjects);
    
    ImGui::NextColumn();
    
    if (ImGui::Button("手动刷新")) {
        g_needsRescan = true;
        RescanGameObjectsInBackground();
    }
    ImGui::SameLine();
    if (ImGui::Button("清除所有")) {
        ClearAllDrawObjects();
    }
    
    ImGui::Columns(1);
    ImGui::Separator();
    
    ImGui::Text("统计信息:");
    ImGui::Text("屏幕内对象: %zu", gameSnapshot.size());
    ImGui::Text("已绘制对象: %zu", drawSnapshot.size());

    ImGui::Separator();

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

    ImGui::Text("已绘制的对象:");
    if (drawSnapshot.empty()) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "暂无绘制对象");
    } else {
        for (size_t i = 0; i < drawSnapshot.size(); i++) {
            ImGui::PushID(i);
            
            ImGui::Text("%s", drawSnapshot[i].target.name.c_str());
            ImGui::SameLine();
            
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
