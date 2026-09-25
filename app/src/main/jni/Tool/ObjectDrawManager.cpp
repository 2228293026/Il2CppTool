#include "ObjectDrawManager.h"
#include "Includes/Logger.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <unordered_set>

static std::mutex g_objectsMutex;
static std::mutex g_drawMutex;

// 单调时钟（秒）。后台扫描线程不能碰 ImGui 上下文，所以计时统一走这里，
// 不再用 ImGui::GetTime()（它读的是 GImGui->Time，属于渲染线程状态）。
static double NowSeconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::vector<DrawObject> ObjectDrawManager::drawObjects;
bool ObjectDrawManager::showObjectManager = false;
ImVec2 ObjectDrawManager::screenCenter = ImVec2(0, 0);
bool ObjectDrawManager::autoRefresh = true;

ObjectDrawManager g_ObjectDrawManager;

static MethodInfo* g_GetTransform = nullptr;
static MethodInfo* g_GetPosition = nullptr;
static MethodInfo* g_GetName = nullptr;
static Il2CppClass* g_CameraClass = nullptr;
static Il2CppClass* g_GameObjectClass = nullptr;
static Il2CppClass* g_TransformClass = nullptr;

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
static std::atomic<double> g_rescanFinishTime{0.0};

// 后台扫描线程由本文件持有，Shutdown 时 join，避免线程还活着时全局状态被清掉。
static std::thread g_scanThread;
static std::atomic<bool> g_shutdownRequested{false};

// 场景切换检测阈值：对象数量变化超过50%认为发生场景切换
static constexpr double SCENE_CHANGE_RATIO = 0.5;

// 场景切换提示：在 UI 上告诉用户"有多少个对象因为场景切换被清掉了"
static std::mutex g_noticeMutex;
static size_t g_sceneChangeRemoved = 0;
static double g_sceneChangeNoticeTime = 0.0;
static constexpr double SCENE_CHANGE_NOTICE_SECONDS = 6.0;

static void SetSceneChangeNotice(size_t removed)
{
    std::lock_guard<std::mutex> lock(g_noticeMutex);
    g_sceneChangeRemoved = removed;
    g_sceneChangeNoticeTime = NowSeconds();
}

// 安全的对象有效性检查
// 先通过 IsNativeObjectAlive 验证原生对象存活（避免访问已释放内存）
// 再检查 klass 是否匹配
static bool IsValidGameObject(Il2CppObject* o) {
    if (o == nullptr) return false;
    if (!g_IsNativeObjectAlive) return false;
    try {
        if (!g_IsNativeObjectAlive->invoke_static<bool>(o))
            return false;
    } catch (...) {
        return false;
    }
    return o->klass != nullptr && o->klass == g_GameObjectClass;
}

// 刷新相机引用（场景切换后旧相机可能失效）
void ObjectDrawManager::RefreshCamera() {
    if (!g_CameraClass) return;
    try {
        auto cam = g_CameraClass->invoke_static_method<Il2CppObject*>("get_main");
        if (cam && cam->klass == g_CameraClass) {
            g_MainCamera = cam;
        }
    } catch (...) {
        g_MainCamera = nullptr;
    }
}

// 后台线程执行对象扫描
static void RescanGameObjectsInBackground() {
    if (!g_GameObjectClass || g_shutdownRequested.load())
        return;
    if (g_rescanBusy.exchange(true))
        return;

    // 上一轮线程如果已经跑完，先回收再开新的（g_rescanBusy 保证同一时刻只有一个在跑）
    if (g_scanThread.joinable())
        g_scanThread.join();

    g_rescanInProgress.store(true);
    g_scanThread = std::thread([]() {
        // 关键：这是新线程，il2cpp 不知道它。
        // GC::FindObjects 走的是 stop_gc_world / liveness 那套，会遍历 GC 结构，
        // 在未 attach 的线程上调用是崩溃或静默错数据的来源。
        // 渲染线程的 EnsureAttached 管不到这里，必须自己挂。
        //
        // 每轮扫描都是一次性线程，所以 attach 之后必须配对 detach：
        // 只挂不摘会在 il2cpp 的 attached-thread 表里留下悬空条目，
        // GC 遍历线程表时会踩到已退出的线程。
        const bool attachedHere = Il2cpp::EnsureAttached();
        if (!attachedHere)
        {
            LOGE("对象扫描: 无法 attach 到 il2cpp VM，跳过本轮扫描");
            g_rescanBusy.store(false);
            g_rescanInProgress.store(false);
            return;
        }

        auto objs = Il2cpp::GC::FindObjects(g_GameObjectClass);

        // 扫描结果先落地，再 detach（detach 之后不能再碰 il2cpp 对象）
        const bool shuttingDown = g_shutdownRequested.load();
        if (!shuttingDown)
        {
            std::lock_guard<std::mutex> lock(g_objectsMutex);
            g_cachedGameObjects = std::move(objs);
        }

        Il2cpp::Detach();

        if (shuttingDown)
        {
            // Shutdown 已经清过状态，别再往里写
            g_rescanBusy.store(false);
            g_rescanInProgress.store(false);
            return;
        }
        g_rescanFinishTime.store(NowSeconds());
        g_hasNewList.store(true);
        g_rescanBusy.store(false);
        g_rescanInProgress.store(false);
    });
}

// 处理扫描结果：构建UI用的对象列表，检测场景切换
// 注意：此函数不在绘制路径上，只在后台扫描完成后执行一次
static void ProcessScannedObjects() {
    std::vector<Il2CppObject*> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_objectsMutex);
        snapshot = g_cachedGameObjects;
    }

    size_t prevCount = g_lastObjectCount.load();
    size_t currCount = snapshot.size();

    // 场景切换检测
    if (prevCount > 0 && currCount > 0) {
        size_t minCount = std::min(prevCount, currCount);
        size_t maxCount = std::max(prevCount, currCount);
        if (maxCount > 0 && (double)minCount / (double)maxCount < SCENE_CHANGE_RATIO) {
            LOGW("检测到场景切换: 对象数从 %zu 变为 %zu", prevCount, currCount);

            // 只清理已经失效的 Il2CppObject*，不要整个 clear()。
            // 旧实现把用户手动挑的对象全删了，只留一行 logcat，用户视角就是"我选的东西凭空消失"。
            size_t before = 0, after = 0;
            {
                std::lock_guard<std::mutex> lock(g_drawMutex);
                before = ObjectDrawManager::drawObjects.size();
                ObjectDrawManager::drawObjects.erase(
                    std::remove_if(ObjectDrawManager::drawObjects.begin(),
                                   ObjectDrawManager::drawObjects.end(),
                                   [](const DrawObject& obj) {
                                       return !IsValidGameObject(obj.target.gameObject);
                                   }),
                    ObjectDrawManager::drawObjects.end());
                after = ObjectDrawManager::drawObjects.size();
            }
            {
                std::lock_guard<std::mutex> lock(g_objectsMutex);
                g_cachedGameObjects.clear();
            }
            g_lastObjectCount.store(0);
            g_MainCamera = nullptr;

            // 给 UI 留个提示，别让用户以为工具坏了
            if (before > after) {
                SetSceneChangeNotice(before - after);
            }
            return;
        }
    }

    g_lastObjectCount.store(currCount);

    // g_autoAddAll: 自动将新扫描到的对象加入绘制列表
    if (g_autoAddAll && g_MainCamera && g_WorldToScreenPoint) {
        std::lock_guard<std::mutex> lock(g_drawMutex);

        // 用 unordered_set 快速判断对象是否已在 drawObjects 中
        std::unordered_set<Il2CppObject*> existing;
        existing.reserve(ObjectDrawManager::drawObjects.size() * 2);
        for (const auto& d : ObjectDrawManager::drawObjects) {
            existing.insert(d.target.gameObject);
        }

        for (auto go : snapshot) {
            if (!IsValidGameObject(go)) continue;
            if (existing.find(go) != existing.end()) continue;

            try {
                auto transform = go->invoke_method<Il2CppObject*>(g_GetTransform);
                if (!transform) continue;
                auto position = transform->invoke_method<Vector3>(g_GetPosition);
                auto screen = g_WorldToScreenPoint->invoke_static<Vector3>(g_MainCamera, position);
                screen.y = ImGui::GetIO().DisplaySize.y - screen.y;

                if (screen.z <= 0) continue;

                std::string name;
                auto nameObj = go->invoke_method<Il2CppObject*>(g_GetName);
                if (nameObj) {
                    auto nameStr = reinterpret_cast<Il2CppString*>(nameObj);
                    if (nameStr) name = nameStr->to_string();
                }

                DrawObject newDrawObj;
                newDrawObj.target.gameObject = go;
                newDrawObj.target.transform = transform;
                newDrawObj.target.worldPosition = position;
                newDrawObj.target.screenPosition = screen;
                newDrawObj.target.name = std::move(name);
                newDrawObj.color = IM_COL32(rand() % 255, rand() % 255, rand() % 255, 255);
                newDrawObj.thickness = 2.0f;
                newDrawObj.drawLine = true;
                newDrawObj.drawBox = true;
                newDrawObj.drawCircle = false;
                ObjectDrawManager::drawObjects.push_back(std::move(newDrawObj));
            } catch (...) {
                continue;
            }
        }
    }
}

// 加锁读取缓存对象数。
// 直接写 g_cachedGameObjects.size() 是数据竞争：后台线程会整体替换这个 vector，
// 无锁读 size() 可能读到撕裂的值甚至已释放内存。
static size_t CachedObjectCount() {
    std::lock_guard<std::mutex> lock(g_objectsMutex);
    return g_cachedGameObjects.size();
}

// 对 drawObjects 中匹配的条目做一次加锁修改。
// UI 遍历的是快照，不能拿快照下标去索引原件（两者大小可能不同 → 越界写）。
template <typename F>
static void MutateDrawObject(Il2CppObject* gameObject, F&& fn) {
    std::lock_guard<std::mutex> lock(g_drawMutex);
    for (auto& d : ObjectDrawManager::drawObjects) {
        if (d.target.gameObject == gameObject) {
            fn(d);
            return;
        }
    }
}

// 清理 drawObjects 中已失效的对象（场景切换后或对象被销毁）
void ObjectDrawManager::CleanupInvalidDrawObjects() {
    std::lock_guard<std::mutex> lock(g_drawMutex);
    drawObjects.erase(
        std::remove_if(drawObjects.begin(), drawObjects.end(),
            [](const DrawObject& obj) {
                return !IsValidGameObject(obj.target.gameObject);
            }),
        drawObjects.end()
    );
}

// 每帧调用：高频刷新 drawObjects 中对象的坐标
// 不做 FindObjects，不做字符串分配，只做坐标变换
void ObjectDrawManager::Tick() {
    // 1. 低频后台重扫（5秒间隔或手动触发）
    static double lastRescan = 0.0;
    double now = NowSeconds();
    if (g_needsRescan.exchange(false) || (autoRefresh && now - lastRescan > 5.0)) {
        lastRescan = now;
        RescanGameObjectsInBackground();
    }

    // 2. 处理后台扫描结果（延迟1秒执行，避免扫描刚完成时对象状态不稳定）
    if (g_hasNewList.load() && (now - g_rescanFinishTime.load() > 1.0)) {
        g_hasNewList.store(false);
        ProcessScannedObjects();
    }

    // 3. 高频坐标刷新（每帧执行，只遍历 drawObjects）
    if (!autoRefresh) return;
    if (!g_MainCamera || !g_WorldToScreenPoint) {
        RefreshCamera();
        if (!g_MainCamera) return;
    }

    // 定期刷新相机（每2秒），防止场景切换后相机失效
    static float lastCameraRefresh = 0.f;
    if (now - lastCameraRefresh > 2.0f) {
        lastCameraRefresh = now;
        RefreshCamera();
    }

    std::lock_guard<std::mutex> lock(g_drawMutex);

    for (auto& drawObj : drawObjects) {
        auto go = drawObj.target.gameObject;

        // 快速判活：先检查指针和 klass（比 IsNativeObjectAlive 快）
        if (!go || !go->klass) {
            drawObj.target.screenPosition.z = -1;
            continue;
        }

        try {
            // 获取 Transform（如果缓存的 transform 失效则重新获取）
            Il2CppObject* transform = drawObj.target.transform;
            if (!transform || transform->klass != g_TransformClass) {
                transform = go->invoke_method<Il2CppObject*>(g_GetTransform);
                drawObj.target.transform = transform;
            }
            if (!transform) {
                drawObj.target.screenPosition.z = -1;
                continue;
            }

            // 获取世界坐标
            auto position = transform->invoke_method<Vector3>(g_GetPosition);
            drawObj.target.worldPosition = position;

            // 世界坐标转屏幕坐标
            auto screen = g_WorldToScreenPoint->invoke_static<Vector3>(g_MainCamera, position);
            screen.y = ImGui::GetIO().DisplaySize.y - screen.y;
            drawObj.target.screenPosition = screen;

        } catch (...) {
            drawObj.target.screenPosition.z = -1;
        }
    }
}

// 绘制所有已选对象
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

    // g_drawAllObjects: 绘制场景中所有对象（从缓存列表读取）
    if (g_drawAllObjects) {
        std::vector<Il2CppObject*> snapshot;
        {
            std::lock_guard<std::mutex> lock(g_objectsMutex);
            snapshot = g_cachedGameObjects;
        }

        for (auto go : snapshot) {
            if (!IsValidGameObject(go)) continue;

            try {
                auto transform = go->invoke_method<Il2CppObject*>(g_GetTransform);
                if (!transform) continue;
                auto position = transform->invoke_method<Vector3>(g_GetPosition);
                auto screen = g_WorldToScreenPoint->invoke_static<Vector3>(g_MainCamera, position);
                screen.y = ImGui::GetIO().DisplaySize.y - screen.y;

                if (screen.z <= 0) continue;

                float x = screen.x;
                float y = screen.y;
                if (std::isnan(x) || std::isnan(y) || std::isinf(x) || std::isinf(y))
                    continue;

                ImColor color = IM_COL32(255, 255, 0, 220);
                drawList->AddLine(ImVec2(x, y), GetScreenCenter(), color, 2.0f);
                drawList->AddRect(ImVec2(x - 20, y - 20), ImVec2(x + 20, y + 20), color, 0.0f, 0, 2.0f);

                std::string name;
                auto nameObj = go->invoke_method<Il2CppObject*>(g_GetName);
                if (nameObj) {
                    auto nameStr = reinterpret_cast<Il2CppString*>(nameObj);
                    if (nameStr) name = nameStr->to_string();
                }
                if (!name.empty()) {
                    drawList->AddText(ImVec2(x, y - 20), color, name.c_str());
                }
            } catch (...) {
                continue;
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
    g_TransformClass = TransformClass;
    if (TransformClass) {
        g_GetPosition = TransformClass->getMethod("get_position");
    }

    auto CameraClass = Il2cpp::FindClass("UnityEngine.Camera");
    g_CameraClass = CameraClass;
    if (CameraClass) {
        g_MainCamera = CameraClass->invoke_static_method<Il2CppObject*>("get_main");
        if (g_MainCamera) {
            // 不能盲目取重载列表里的 [1]。Camera.WorldToScreenPoint 有多个重载
            // （含带 MonoOrStereoscopicEye 的两参版本），按下标取很容易挑错签名；
            // 而 invoke 是按「尾部再塞一个 MethodInfo*」的约定直接 reinterpret 成函数指针调的，
            // 签名一错就等于把隐藏参数喂到枚举参数的位置上 → 寄存器/栈错乱。
            // 显式按名字 + 1 个参数找；找不到就留空，后面所有调用点都有判空。
            g_WorldToScreenPoint = CameraClass->getMethod("WorldToScreenPoint", 1);
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

    // 先置位并 join，确保扫描线程不再访问下面要清掉的全局状态。
    // 旧实现直接 clear() 而线程仍 detach 在跑，属于 use-after-free 窗口。
    g_shutdownRequested.store(true);
    if (g_scanThread.joinable())
        g_scanThread.join();

    {
        std::lock_guard<std::mutex> lock(g_objectsMutex);
        g_cachedGameObjects.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_drawMutex);
        drawObjects.clear();
    }
    g_lastObjectCount.store(0);
    g_rescanInProgress.store(false);
    g_rescanBusy.store(false);
    g_needsRescan.store(false);
    g_hasNewList.store(false);
    g_MainCamera = nullptr;

    // 允许再次启用
    g_shutdownRequested.store(false);
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

    drawObjects.push_back(std::move(newDrawObj));
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

    drawObjects.push_back(std::move(newDrawObj));
}

void ObjectDrawManager::DeselectAll() {
    std::lock_guard<std::mutex> lock(g_drawMutex);
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
        screenPos.y = ImGui::GetIO().DisplaySize.y - screenPos.y;
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

// 构建UI用的对象列表（从缓存读取，不做坐标转换）
static std::vector<GameObjectInfo> BuildUIObjectList() {
    std::vector<GameObjectInfo> result;
    std::vector<Il2CppObject*> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_objectsMutex);
        snapshot = g_cachedGameObjects;
    }

    if (!g_MainCamera || !g_WorldToScreenPoint || !g_GetTransform || !g_GetPosition)
        return result;

    result.reserve(snapshot.size());
    for (auto go : snapshot) {
        if (!IsValidGameObject(go)) continue;

        try {
            auto transform = go->invoke_method<Il2CppObject*>(g_GetTransform);
            if (!transform) continue;
            auto position = transform->invoke_method<Vector3>(g_GetPosition);
            auto screen = g_WorldToScreenPoint->invoke_static<Vector3>(g_MainCamera, position);
            screen.y = ImGui::GetIO().DisplaySize.y - screen.y;

            if (screen.z <= 0) continue;

            GameObjectInfo info;
            info.gameObject = go;
            info.transform = transform;
            info.worldPosition = position;
            info.screenPosition = screen;

            auto nameObj = go->invoke_method<Il2CppObject*>(g_GetName);
            if (nameObj) {
                auto nameStr = reinterpret_cast<Il2CppString*>(nameObj);
                if (nameStr) info.name = nameStr->to_string();
            }

            result.push_back(std::move(info));
        } catch (...) {
            continue;
        }
    }
    return result;
}

void ObjectDrawManager::DrawUI() {
    std::vector<DrawObject> drawSnapshot;
    {
        std::lock_guard<std::mutex> lock(g_drawMutex);
        drawSnapshot = drawObjects;
    }

    // UI 对象列表实时构建（只在打开UI时执行，不影响绘制性能）
    std::vector<GameObjectInfo> gameSnapshot = BuildUIObjectList();

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
    ImGui::SameLine();
    if (ImGui::Button("清理失效")) {
        CleanupInvalidDrawObjects();
    }

    ImGui::Columns(1);
    ImGui::Separator();

    ImGui::Text("统计信息:");
    ImGui::Text("缓存对象: %zu", CachedObjectCount());
    ImGui::Text("屏幕内对象: %zu", gameSnapshot.size());
    ImGui::Text("已绘制对象: %zu", drawSnapshot.size());

    // 场景切换提示（几秒后自动消失）
    {
        std::lock_guard<std::mutex> lock(g_noticeMutex);
        if (g_sceneChangeRemoved > 0 && (NowSeconds() - g_sceneChangeNoticeTime) < SCENE_CHANGE_NOTICE_SECONDS) {
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f),
                               "检测到场景切换，已清理 %zu 个失效对象（其余保留）", g_sceneChangeRemoved);
        } else if (g_sceneChangeRemoved > 0) {
            g_sceneChangeRemoved = 0;
        }
    }

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

    // 用 unordered_set 快速判断是否在 drawObjects 中
    std::unordered_set<Il2CppObject*> existing;
    existing.reserve(drawSnapshot.size() * 2);
    for (const auto& d : drawSnapshot) {
        existing.insert(d.target.gameObject);
    }

    for (const auto& obj : gameSnapshot) {
        if (objFilter[0] && obj.name.find(objFilter) == std::string::npos)
            continue;

        bool already = existing.find(obj.gameObject) != existing.end();

        ImGui::PushID(obj.gameObject);

        // 避免每帧构造 std::string，使用 ImGui::Text 格式化
        if (already) {
            ImGui::Text("[已选] %s", obj.name.c_str());
        } else {
            ImGui::Text("%s", obj.name.c_str());
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("添加")) {
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

            // 注意：drawSnapshot 是副本，回写必须按 gameObject 定位，
            // 不能用快照下标 i 去索引 drawObjects（两者大小可能不同 → 越界写）。
            auto* go = drawSnapshot[i].target.gameObject;

            ImGui::Text("%s", drawSnapshot[i].target.name.c_str());
            ImGui::SameLine();

            ImVec4 color = ImGui::ColorConvertU32ToFloat4(drawSnapshot[i].color);
            ImGui::ColorButton("##color", color, ImGuiColorEditFlags_NoTooltip, ImVec2(20, 20));

            ImGui::SameLine();
            if (ImGui::Button("移除")) {
                RemoveDrawObject(go);
                ImGui::PopID();
                break;
            }

            ImGui::SameLine();
            bool drawLine = drawSnapshot[i].drawLine;
            bool drawBox = drawSnapshot[i].drawBox;
            bool drawCircle = drawSnapshot[i].drawCircle;

            if (ImGui::Checkbox("线", &drawLine)) {
                MutateDrawObject(go, [drawLine](DrawObject& d) { d.drawLine = drawLine; });
            }
            ImGui::SameLine();
            if (ImGui::Checkbox("框", &drawBox)) {
                MutateDrawObject(go, [drawBox](DrawObject& d) { d.drawBox = drawBox; });
            }
            ImGui::SameLine();
            if (ImGui::Checkbox("圆", &drawCircle)) {
                MutateDrawObject(go, [drawCircle](DrawObject& d) { d.drawCircle = drawCircle; });
            }

            ImGui::PopID();
        }
    }
}