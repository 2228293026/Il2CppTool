#include "ObjectDrawManager.h"
#include "Includes/NeverDestroyedMutex.h"
#include "Includes/Logger.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <unordered_set>

static NeverDestroyedMutex g_objectsMutex;
static NeverDestroyedMutex g_drawMutex;

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
// 最大绘制距离（世界单位/米）。<= 0 表示不限制。
// 有了距离之后这个开关才真正有意义：场景里几百米外的东西画出来只是
// 屏幕角落一堆看不清的点，纯属遮挡视线。
float ObjectDrawManager::maxDrawDistance = 0.f;

ObjectDrawManager g_ObjectDrawManager;

// 强引用句柄的归属跟着 GameObjectInfo 走。
// 拷贝被禁止、只允许移动，是为了保证「一个 gchandle 恰好有一个主人」：
// 浅拷贝会让两个条目共用同一个句柄，析构两次就 double free。
GameObjectInfo::GameObjectInfo(GameObjectInfo &&other) noexcept
    : gameObject(other.gameObject), transform(other.transform), worldPosition(other.worldPosition),
      screenPosition(other.screenPosition), name(std::move(other.name)), isSelected(other.isSelected),
      gameObjectHandle(other.gameObjectHandle), transformHandle(other.transformHandle)
{
    other.gameObjectHandle = 0;
    other.transformHandle = 0;
    other.gameObject = nullptr;
    other.transform = nullptr;
}

GameObjectInfo &GameObjectInfo::operator=(GameObjectInfo &&other) noexcept
{
    if (this != &other)
    {
        // 先释放自己原有的句柄，否则会泄漏
        Il2cpp::GC::FreeHandle(gameObjectHandle);
        Il2cpp::GC::FreeHandle(transformHandle);

        gameObject = other.gameObject;
        transform = other.transform;
        worldPosition = other.worldPosition;
        screenPosition = other.screenPosition;
        name = std::move(other.name);
        isSelected = other.isSelected;
        gameObjectHandle = other.gameObjectHandle;
        transformHandle = other.transformHandle;

        other.gameObjectHandle = 0;
        other.transformHandle = 0;
        other.gameObject = nullptr;
        other.transform = nullptr;
    }
    return *this;
}

GameObjectInfo::~GameObjectInfo()
{
    Il2cpp::GC::FreeHandle(gameObjectHandle);
    Il2cpp::GC::FreeHandle(transformHandle);
}

static MethodInfo* g_GetTransform = nullptr;
static MethodInfo* g_GetPosition = nullptr;
static MethodInfo* g_GetName = nullptr;
static Il2CppClass* g_CameraClass = nullptr;
static Il2CppClass* g_GameObjectClass = nullptr;
static Il2CppClass* g_TransformClass = nullptr;
// 真实包围盒：GetComponent<Renderer>() 与 Renderer.get_bounds
static MethodInfo* g_GetComponent = nullptr;
static MethodInfo* g_GetBounds = nullptr;
static Il2CppClass* g_RendererClass = nullptr;
// 真实包围盒 → 屏幕矩形。定义在文件后面（挨着 DrawBox），这里先前置声明。
static bool ComputeScreenBounds(Il2CppObject *gameObject, ImVec2 &outMin, ImVec2 &outMax);

static bool g_drawAllObjects = false;
static bool g_autoAddAll = false;
// 是否在名字旁边显示到相机的距离。默认开：这是 ESP 里最有用的一个数值。
static bool g_showDistance = true;
static bool g_limitDistance = false;
// 真实包围盒 vs 固定 ±20px。默认开真实包围盒 —— 固定尺寸让远处的
// 大建筑和近处的小道具画出来一样大，完全没有尺寸信息。
// 每个目标每帧要多 8 次投影 + 1 次 GetComponent，所以留开关。
static bool g_useRealBounds = true;

// 主相机的 **GC 强根**。
//
// 相机原本是一个裸的全局 Il2CppObject*，被 7 处以上直接解引用
// （CameraWorldPosition / 各种 WorldToScreenPoint 调用点）。
//
// 场景切换时旧的 Camera 会被销毁；只要游戏侧不再引用它，GC 就会回收 ——
// 我们手上那个裸指针随即变野，而 IsValidGameObject 之类的检查必须先
// 解引用才能调用，所以「先判断再用」在这里根本救不了。
//
// RefreshCamera 每 2 秒才刷新一次，场景切换的检测又只是「对象数量比值」
// 这个启发式（数量相近的切换根本测不出来）。所以在这两秒的空档里，
// 一个已经死掉的相机指针会被喂给 WorldToScreenPoint。
//
// 这和 ESP 列表里 GameObject 的加根是同一个道理（见下面的加根说明）。
static uint32_t g_MainCameraHandle = 0;

// 兼容用的裸指针缓存 —— 权威来源是上面的句柄。
// **只允许**在持有句柄的前提下读它；任何解引用之前都应先经过
// ResolveMainCamera()，不要直接用这个全局。
static Il2CppObject* g_MainCamera = nullptr;

// 取当前主相机；句柄为空（没相机 / 已被回收）时返回 nullptr。
//
// 必须替换掉所有直接用 g_MainCamera 解引用的地方。
//
// 【线程约束】这个句柄**只由渲染线程访问，不需要加锁**：
//   Initialize / Tick / Shutdown 以及它们内部的 ProcessScannedObjects
//   和 RefreshCamera 全部跑在 draw_thread 上（Main.cpp:504 的 Tick、
//   Tool::Init 的 Initialize/Shutdown），没有别的线程会碰它。
//
// 这条不变量是「碰巧成立」的，没有任何机制强制。如果哪天有人想
// 「检测到场景切换就顺便在后台线程里刷新相机」，就会静默引入
// 竞态（句柄被换掉的同时渲染线程正在读，读到的是撕裂的中间态）。
// 真要跨线程用，必须先给句柄加一把专门的锁。
static inline Il2CppObject* ResolveMainCamera()
{
    if (g_MainCameraHandle == 0)
    {
        return nullptr;
    }
    return Il2cpp::GC::GetHandleTarget(g_MainCameraHandle);
}

// 释放相机句柄并清空缓存指针。定义在 RefreshCamera 之后。
static void ReleaseMainCamera();

static MethodInfo* g_WorldToScreenPoint = nullptr;

// 上次惰性重试的时间（渲染线程独占）。用 steady_clock 的 epoch 秒数，
// 而不是 ImGui::GetTime() —— 后者只有 ImGui 上下文活着时才有意义。
//
// 存成 double 而不是 time_point：time_point 的默认构造不是 constexpr，
// 静态初始化期会因此生成调用构造函数的代码（clang-tidy cert-err58-cpp）。
// double + 常量 0.0 就没有这个问题。
// 初值 0 表示「从没重试过」：epoch 秒数远大于 2，于是第一次一定立即重试。
static double s_lastResolveRetry = 0.0;
static MethodInfo* g_IsNativeObjectAlive = nullptr;

// 给一个待绘制目标加根。
//
// 没有这个的时候，ESP 列表里的 GameObject 全是「悬着的裸指针」：一旦游戏侧
// 不再引用它（例如切场景时销毁），GC 就会回收，我们手上的指针变野 ——
// 而 IsValidGameObject 之类的检查必须先解引用指针才能调用，所以「先判断再用」
// 根本防不住，只能靠强引用让它别被回收。
static void RootGameObject(GameObjectInfo& info)
{
    if (info.gameObjectHandle == 0 && info.gameObject)
    {
        info.gameObjectHandle = Il2cpp::GC::NewHandle(info.gameObject);
        if (info.gameObjectHandle == 0)
        {
            LOGW("GameObject 加根失败，ESP 目标可能在 GC 后失效");
        }
    }
    if (info.transformHandle == 0 && info.transform)
    {
        info.transformHandle = Il2cpp::GC::NewHandle(info.transform);
    }
}

// 通过句柄取回仍然有效的对象指针。
//
// **句柄为 0 时返回 nullptr，而不是 info.gameObject。**（第 98 轮改）
//
// 旧代码在这里 `return info.gameObject;` —— 也就是「加根失败时，
// 退回那个没有根的裸指针」。而 RootGameObject() 在加根失败时
// **只打一行 LOGW 就继续**，所以这个分支不是理论情况：
//
//     加根失败 → 句柄 0 → Resolve 返回裸指针
//            → DrawAll 每帧 transform->invoke_method(g_GetPosition)
//            → 对一个随时会被 GC 回收的对象解引用 = 崩游戏
//
// 上面两行注释（「绝不能去解引用 info.gameObject 那个旧地址」）
// 说的正是这件事 —— **注释是对的，代码是错的**。
//
// 真正的失败（句柄指向的对象被回收）本来就由 GetHandleTarget 返回
// nullptr 来表达。加根失败是另一种失败，它表达的是「我们不敢碰它」。
// 两者都该返回 nullptr，调用方一律跳过。
static Il2CppObject* ResolveGameObject(const GameObjectInfo& info)
{
    if (!info.gameObjectHandle)
    {
        // 加根没成功。不退回裸指针 —— 那个地址随时可能是野的。
        return nullptr;
    }
    return Il2cpp::GC::GetHandleTarget(info.gameObjectHandle);
}

static Il2CppObject* ResolveTransform(const GameObjectInfo& info)
{
    if (!info.transformHandle)
    {
        return nullptr;
    }
    return Il2cpp::GC::GetHandleTarget(info.transformHandle);
}

static std::vector<Il2CppObject*> g_cachedGameObjects;
    // 与 g_cachedGameObjects **一一对应**的 GC 强根。
    // 裸指针本身不做任何保活 —— 有了它才能安全地解引用（第 100 轮）。
    static std::vector<uint32_t> g_cachedHandles;

    // 清缓存**必须连句柄一起放**。
    //
    // 只 clear() 裸指针 vector 的话，句柄会一直留在 GC 的句柄表里 ——
    // 而那正是第 82 轮修好的那个 GC 强根计数器要抓的「句柄只增不减」。
    // 每切一次场景走一次这条路，所以这不是「理论上会涨」，是必然涨。
    // 必须在持有 g_objectsMutex 时调用。
    static void ClearCachedObjectListLocked()
    {
        for (auto h : g_cachedHandles)
        {
            if (h)
            {
                Il2cpp::GC::FreeHandle(h);
            }
        }
        g_cachedHandles.clear();
        g_cachedGameObjects.clear();
    }
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
static NeverDestroyedMutex g_noticeMutex;
static size_t g_sceneChangeRemoved = 0;
static double g_sceneChangeNoticeTime = 0.0;
static constexpr double SCENE_CHANGE_NOTICE_SECONDS = 6.0;

static void SetSceneChangeNotice(size_t removed)
{
    std::lock_guard<NeverDestroyedMutex> lock(g_noticeMutex);
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
    if (!g_CameraClass) {
        ReleaseMainCamera();
        return;
    }
    try {
        auto cam = g_CameraClass->invoke_static_method<Il2CppObject*>("get_main");
        if (cam && cam->klass == g_CameraClass) {
            if (g_MainCameraHandle == 0 || g_MainCamera != cam) {
                // 换了相机 → 先释放旧句柄再挂新的，别泄漏。
                ReleaseMainCamera();
                g_MainCameraHandle = Il2cpp::GC::NewHandle(cam);
                if (g_MainCameraHandle == 0) {
                    LOGW("主相机加 GC 根失败，ESP 将不可用直到下次刷新");
                    g_MainCamera = nullptr;
                    return;
                }
                g_MainCamera = cam;
            }
        } else {
            // Camera.main 拿不到了（加载中 / 场景刚切）。释放句柄，
            // 让所有取用点自然拿到 nullptr 而不是野指针。
            ReleaseMainCamera();
        }
    } catch (...) {
        ReleaseMainCamera();
    }
}

// 释放相机句柄。
static void ReleaseMainCamera()
{
    if (g_MainCameraHandle != 0)
    {
        Il2cpp::GC::FreeHandle(g_MainCameraHandle);
        g_MainCameraHandle = 0;
    }
    g_MainCamera = nullptr;
}

// 相机在世界空间的位置。用来算目标到相机的距离 ——
// 屏幕上「离得近/远」的信息丢失了，只有 3D 距离才对应用户心里的远近。
//
// 走 Camera.get_transform().get_position()：Camera 继承自 Behaviour→Component，
// 「get_transform」是继承来的方法，所以必须从父类上取，单类查找拿不到。
static bool CameraWorldPosition(Vector3 &out)
{
    auto *cam = ResolveMainCamera();
    if (!cam || !g_GetTransform || !g_GetPosition) return false;
    try {
        auto transform = cam->invoke_method<Il2CppObject*>(g_GetTransform);
        if (!transform) return false;
        out = transform->invoke_method<Vector3>(g_GetPosition);
        return true;
    } catch (...) {
        return false;
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

    // **必须 try/catch**：std::thread 的构造函数在线程创建失败时抛异常。
    // 裸写是 std::terminate → 游戏崩。而且 g_rescanBusy 会一直停在 true，
    // 用户再也点不了「重新扫描」，也没人告诉他为什么。
    try
    {
    g_rescanInProgress.store(true);
    g_scanThread = std::thread([]() {
        // 关键：这是新线程，il2cpp 不知道它。
        // GC::FindObjects 走的是 stop_gc_world / liveness 那套，会遍历 GC 结构，
        // 在未 attach 的线程上调用是崩溃或静默错数据的来源。
        // 渲染线程的 EnsureAttached 管不到这里，必须自己挂。
        //
        // 每轮扫描都是一次性线程，所以 attach 之后必须配对 detach：
        // 只挂不摘会在 il2cpp 的 attached-thread 表里留下悬空条目，
        // GC 遍历线程表时会踩到已退出的线程。用 RAII 保证异常路径也不会漏。
        const bool attachedHere = Il2cpp::EnsureAttached();
        if (!attachedHere)
        {
            LOGE("对象扫描: 无法 attach 到 il2cpp VM，跳过本轮扫描");
            g_rescanBusy.store(false);
            g_rescanInProgress.store(false);
            return;
        }
        struct DetachGuard
        {
            ~DetachGuard() { Il2cpp::Detach(); }
        } detachGuard;

        // FindObjects 内部要分配 vector；一旦 bad_alloc 抛出来，
        // 没有 DetachGuard 就会带着「已挂载」的线程直接 terminate。
        std::vector<Il2CppObject *> objs;
        try
        {
            objs = Il2cpp::GC::FindObjects(g_GameObjectClass);
        }
        catch (const std::exception &e)
        {
            LOGE("对象扫描失败: %s", e.what());
        }
        catch (...)
        {
            LOGE("对象扫描未知异常");
        }

        if (g_shutdownRequested.load())
        {
            // Shutdown 已经清过状态，别再往里写
            g_rescanBusy.store(false);
            g_rescanInProgress.store(false);
            return;
        }
        // **当场加根**，在扫描线程里就做完（第 100 轮改）。
        //
        // 旧代码把裸指针存进 g_cachedGameObjects 就完事，而这个列表最长
        // 要活 5 秒（下一次重扫之前）才被替换。BuildUIObjectList 在这期间
        // 会对每个条目做 IsValidGameObject() —— 那个函数要读 o->klass 并
        // invoke。而扫描结果里的对象**没有任何 GC 根**（FindObjects 找到
        // 它们只是说明「此刻还活着」，不是「它们会被保活」）：
        // 游戏侧一放手，GC 随时可以收走，于是这里就是 use-after-free。
        //
        // 5 秒里游戏做一次 GC（切场景、切图鉴）是完全正常的。
        std::vector<uint32_t> newHandles;
        newHandles.reserve(objs.size());
        for (auto *o : objs)
        {
            newHandles.push_back(o ? Il2cpp::GC::NewHandle(o) : 0);
        }
        {
            std::lock_guard<NeverDestroyedMutex> lock(g_objectsMutex);
            // 旧句柄随旧列表一起退休。放在锁外做 FreeHandle 更安全，
            // 但 NewHandle/FreeHandle 都不碰 drawObjects，这里足够。
            for (auto h : g_cachedHandles)
            {
                if (h)
                {
                    Il2cpp::GC::FreeHandle(h);
                }
            }
            g_cachedHandles = std::move(newHandles);
            g_cachedGameObjects = std::move(objs);
        }
        g_rescanFinishTime.store(NowSeconds());
        g_hasNewList.store(true);
        g_rescanBusy.store(false);
        g_rescanInProgress.store(false);
    });
    }
    catch (const std::exception &e)
    {
        g_rescanBusy.store(false);   // 否则再也点不了「重新扫描」
        g_rescanInProgress.store(false);
        LOGE("无法创建对象扫描线程: %s（对象列表不会自动更新，可手动重试）", e.what());
    }
}

// 处理扫描结果：构建UI用的对象列表，检测场景切换
// 注意：此函数不在绘制路径上，只在后台扫描完成后执行一次
static void ProcessScannedObjects() {
    std::vector<Il2CppObject*> snapshot;
    {
        std::lock_guard<NeverDestroyedMutex> lock(g_objectsMutex);
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
                std::lock_guard<NeverDestroyedMutex> lock(g_drawMutex);
                before = ObjectDrawManager::drawObjects.size();
                ObjectDrawManager::drawObjects.erase(
                    std::remove_if(ObjectDrawManager::drawObjects.begin(),
                                   ObjectDrawManager::drawObjects.end(),
                                   [](const DrawObject& obj) {
                                       // 走句柄，不碰裸指针。直接拿 target.gameObject
                                       // 判活就是解引用一个可能已被回收的地址 ——
                                       // 而这段代码的用途恰恰是「清理失效对象」，
                                       // 所以它是最容易自己踩雷的地方（第 98 轮）。
                                       return !IsValidGameObject(ResolveGameObject(obj.target));
                                   }),
                    ObjectDrawManager::drawObjects.end());
                after = ObjectDrawManager::drawObjects.size();
            }
            {
                std::lock_guard<NeverDestroyedMutex> lock(g_objectsMutex);
                ClearCachedObjectListLocked();
            }
            g_lastObjectCount.store(0);
            ReleaseMainCamera();

            // 给 UI 留个提示，别让用户以为工具坏了
            if (before > after) {
                SetSceneChangeNotice(before - after);
            }
            return;
        }
    }

    g_lastObjectCount.store(currCount);

    // g_autoAddAll: 自动将新扫描到的对象加入绘制列表
    auto *cam = ResolveMainCamera();
    if (g_autoAddAll && cam && g_WorldToScreenPoint) {
        std::lock_guard<NeverDestroyedMutex> lock(g_drawMutex);

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
                auto screen = g_WorldToScreenPoint->invoke_static<Vector3>(cam, position);
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
                // 加根：自动添加的对象同样要保活，否则扫描列表一刷新
                // （下一轮 FindObjects 之前）GC 就能把它收走
                RootGameObject(newDrawObj.target);
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
    std::lock_guard<NeverDestroyedMutex> lock(g_objectsMutex);
    return g_cachedGameObjects.size();
}

// 对 drawObjects 中匹配的条目做一次加锁修改。
// UI 遍历的是快照，不能拿快照下标去索引原件（两者大小可能不同 → 越界写）。
template <typename F>
static void MutateDrawObject(Il2CppObject* gameObject, F&& fn) {
    std::lock_guard<NeverDestroyedMutex> lock(g_drawMutex);
    for (auto& d : ObjectDrawManager::drawObjects) {
        if (d.target.gameObject == gameObject) {
            fn(d);
            return;
        }
    }
}

// 清理 drawObjects 中已失效的对象（场景切换后或对象被销毁）
void ObjectDrawManager::CleanupInvalidDrawObjects() {
    std::lock_guard<NeverDestroyedMutex> lock(g_drawMutex);
    drawObjects.erase(
        std::remove_if(drawObjects.begin(), drawObjects.end(),
            [](const DrawObject& obj) {
                // 先经句柄确认对象是否还在。直接拿 target.gameObject 去判活
                // 是不行的：对象被回收后那个地址已经是野的，解引用它去读 klass
                // 就崩了 —— 而这正是「清理失效对象」的代码本身最容易犯的错。
                auto go = ResolveGameObject(obj.target);
                return !IsValidGameObject(go);
            }),
        drawObjects.end()
    );
    // erase 之后被移除的 GameObjectInfo 析构，句柄随之释放。
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

    // 定期刷新相机（每 2 秒），防止场景切换后相机失效。
    // 先刷再取：句柄可能是上一轮才被释放的，用旧值会拿到野指针。
    static float lastCameraRefresh = 0.f;
    if (now - lastCameraRefresh > 2.0f) {
        lastCameraRefresh = now;
        RefreshCamera();
    }
    auto *cam = ResolveMainCamera();
    if (!cam || !g_WorldToScreenPoint) {
        RefreshCamera();
        cam = ResolveMainCamera();
        if (!cam) return;
    }

    // 相机位置在循环外取一次：它对所有目标都是同一个值，
    // 每个目标都去 invoke 一次 get_transform/get_position 是浪费。
    Vector3 cameraPos{};
    const bool haveCameraPos = CameraWorldPosition(cameraPos);

    std::lock_guard<NeverDestroyedMutex> lock(g_drawMutex);

    for (auto& drawObj : drawObjects) {
        // 先经句柄取回对象。对象已被 GC 回收时句柄返回 nullptr，
        // 这一步就是在「碰那个裸指针之前」拦住它 —— 之后的 klass/调用才安全。
        auto go = ResolveGameObject(drawObj.target);

        // 快速判活：句柄失效或指针/klass 为空就跳过
        if (!go || !go->klass) {
            drawObj.target.screenPosition.z = -1;
            continue;
        }

        try {
            // 获取 Transform（如果缓存的 transform 失效则重新获取）
            // 同样走句柄：缓存的 transform 同样可能已被回收。
            Il2CppObject* transform = ResolveTransform(drawObj.target);
            if (!transform || transform->klass != g_TransformClass) {
                transform = go->invoke_method<Il2CppObject*>(g_GetTransform);
                if (transform) {
                    // 重新拿到的 transform 也要加根，否则下一帧它就可能消失
                    Il2cpp::GC::FreeHandle(drawObj.target.transformHandle);
                    drawObj.target.transformHandle = Il2cpp::GC::NewHandle(transform);
                    if (drawObj.target.transformHandle == 0)
                    {
                        // 上一行刚把旧句柄放掉了，现在这个 transform **没有任何根**。
                        // 而下面紧接着就会拿它去取世界坐标 —— 必须在这里就停下，
                        // 不能带着一个可能被回收的对象往下走（第 98 轮）。
                        LOGW("重新获取的 transform 加根失败，本帧跳过这个目标");
                        drawObj.target.transform = nullptr;
                        drawObj.target.screenPosition.z = -1;
                        continue;
                    }
                    drawObj.target.transform = transform;
                }
            }
            if (!transform) {
                drawObj.target.screenPosition.z = -1;
                continue;
            }

            // 获取世界坐标
            auto position = transform->invoke_method<Vector3>(g_GetPosition);
            drawObj.target.worldPosition = position;

            // 到相机的 3D 距离。屏幕坐标把「远近」这个信息压扁掉了：
            // 屏幕上 10 像素的两个物体，一个可能在脚边、一个在几百米外。
            // 距离是 ESP 里最有用的一个数值（判断该不该打、打不打得到）。
            if (haveCameraPos) {
                const float dx = position.x - cameraPos.x;
                const float dy = position.y - cameraPos.y;
                const float dz = position.z - cameraPos.z;
                drawObj.target.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                drawObj.target.hasDistance = true;
            } else {
                drawObj.target.hasDistance = false;
            }

            // 世界坐标转屏幕坐标
            auto screen = g_WorldToScreenPoint->invoke_static<Vector3>(cam, position);
            screen.y = ImGui::GetIO().DisplaySize.y - screen.y;
            drawObj.target.screenPosition = screen;

            // 真实包围盒。拿不到就退回固定尺寸框（DrawBox 里处理）。
            if (g_useRealBounds) {
                ImVec2 bmin{}, bmax{};
                if (ComputeScreenBounds(go, bmin, bmax)) {
                    drawObj.target.screenBoundsMin = bmin;
                    drawObj.target.screenBoundsMax = bmax;
                    drawObj.target.hasScreenBounds = true;
                } else {
                    drawObj.target.hasScreenBounds = false;
                }
            } else {
                drawObj.target.hasScreenBounds = false;
            }

        } catch (...) {
            drawObj.target.screenPosition.z = -1;
        }
    }
}

// 绘制所有已选对象
void ObjectDrawManager::DrawAll() {
    auto drawList = ImGui::GetForegroundDrawList();

    std::lock_guard<NeverDestroyedMutex> lock(g_drawMutex);
    for (const auto& drawObj : drawObjects) {
        if (drawObj.target.screenPosition.z <= 0)
            continue;

        // 距离过滤。放在这里（而不是 Tick）是为了不浪费后面的绘制开销。
        if (maxDrawDistance > 0.f && drawObj.target.hasDistance &&
            drawObj.target.distance > maxDrawDistance) {
            continue;
        }

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

        // 名称 + 距离画成两行。
        // 距离是判断「该不该打 / 打不打得到」最直接的信息，而屏幕坐标本身
        // 不含远近 —— 必须显式给出。
        // 名字/距离跟着包围盒顶端走，而不是物体中心 —— 大物件的标签压在
        // 中心很难对上，站在框上面才看得出这个标签属于哪个。
        ImVec2 namePos(x, y - 18);
        if (drawObj.target.hasScreenBounds) {
            namePos.y = drawObj.target.screenBoundsMin.y - 6.f;
        }
        const bool showDist = g_showDistance && drawObj.target.hasDistance;
        if (showDist) {
            char distLabel[48];
            // 小于 10 米保留一位小数，再远就取整 —— 10.0 米和 1000 米
            // 没人需要看到小数。
            const char *fmt = drawObj.target.distance < 10.f ? "%.1fm" : "%.0fm";
            int written = snprintf(distLabel, sizeof(distLabel), fmt, drawObj.target.distance);
            if (written < 0)
            {
                // 格式化失败（理论上不该发生）。别把未定义内容交给 AddText。
                distLabel[0] = '\0';
            }
            else if ((size_t)written >= sizeof(distLabel))
            {
                // 被截断了。距离值不会长到这个程度，但真发生了要标出来，
                // 而不是默默显示一个错误的数字。
                // 48 字节装 "%.1fkm" 绰绰有余（最坏也就 "-12345.6km"），
                // 所以这里不需要再判返回值。
                (void)snprintf(distLabel, sizeof(distLabel), "%.1fkm", drawObj.target.distance / 1000.f);
            }
            if (distLabel[0] == '\0')
            {
                // 拿不到距离就只显示名字
                if (!drawObj.target.name.empty())
                {
                    drawList->AddText(namePos, drawObj.color, drawObj.target.name.c_str());
                }
            }
            else if (drawObj.target.name.empty()) {
                drawList->AddText(namePos, drawObj.color, distLabel);
            } else {
                // 这个 ImGui 版本的 AddText 没有 printf 重载，得自己拼。
                // 名字长度不受控（Unity 对象名可以很长），所以用 std::string
                // 让它自然增长。
                std::string label = drawObj.target.name;
                label += "  ";
                label += distLabel;
                drawList->AddText(namePos, drawObj.color, label.c_str());
            }
        } else if (!drawObj.target.name.empty()) {
            drawList->AddText(namePos, drawObj.color, drawObj.target.name.c_str());
        }
    }

    // g_drawAllObjects: 绘制场景中所有对象（从缓存列表读取）
    auto *cam = ResolveMainCamera();
    if (g_drawAllObjects && cam && g_WorldToScreenPoint) {
        std::vector<Il2CppObject*> snapshot;
        {
            std::lock_guard<NeverDestroyedMutex> lock(g_objectsMutex);
            snapshot = g_cachedGameObjects;
        }

        for (auto go : snapshot) {
            if (!IsValidGameObject(go)) continue;

            try {
                auto transform = go->invoke_method<Il2CppObject*>(g_GetTransform);
                if (!transform) continue;
                auto position = transform->invoke_method<Vector3>(g_GetPosition);
                auto screen = g_WorldToScreenPoint->invoke_static<Vector3>(cam, position);
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

// ESP / 对象绘制依赖的方法解析。
//
// 之前是**一次性**的：Initialize() 里解析一次，失败就是 null 到会话结束。
// 而这个工具的初始化时机不由它控制 —— 用户在加载画面早期就勾上
// 「对象绘制管理器」是完全可能的，那时 UnityEngine 的一些类还没注册好，
// FindClass 拿不到 → ESP **整个会话都是死的**。
//
// 症状还特别隐蔽：解析结果只记在 LOGD 里，而 **LOGD 在正式版是被编译掉的**
// —— 也就是说正式版用户看到的是「ESP 什么都不画」，没有任何线索。
//
// 现在改成**惰性重试**：Tick 里发现有还是 null 的，最多每 2 秒重试一次，
// 只补 null 的那些（幂等，开销极小），一旦补上用 LOGI 明说 ——
// 这样正式版的日志里能看到「晚到的解析」，排障时是个明确的线索。
static void ResolveDrawingApis()
{
    // 门禁挂在 **g_GetName** 上（不是 g_GetTransform）。
    //
    // 原来挂在 g_GetTransform 上，于是：get_transform 拿到了、get_name
    // 没拿到（元数据被裁剪/改名）时，g_GetTransform 非空 → 整个块跳过
    // → g_GetName 永远为 null → ESP 的名称标签整个会话都不显示。
    // 三个变量在**不同的时候**可能分别失败，门禁必须覆盖最晚失败的那个。
    if (g_GetName == nullptr || g_GetComponent == nullptr || g_GameObjectClass == nullptr)
    {
        if (g_GameObjectClass == nullptr)
        {
            g_GameObjectClass = Il2cpp::FindClass("UnityEngine.GameObject");
        }
        if (auto *c = g_GameObjectClass)
        {
            if (g_GetTransform == nullptr)
            {
                g_GetTransform = c->getMethod("get_transform");
            }
            if (g_GetName == nullptr)
            {
                g_GetName = c->getMethod("get_name");
            }
            if (g_GetComponent == nullptr)
            {
                // GameObject.GetComponent<T>() 在 il2cpp 里是泛型方法，编译后名字是
                // "GetComponent<Renderer>"（元数据里保留泛型参数）。用单参的
                // "GetComponent" 拿到的是 GetComponent(Type)，传错参数会取到
                // 随便什么东西 —— 比拿不到更糟，因为它「成功」了。
                g_GetComponent = c->getMethod("GetComponent<UnityEngine.Renderer>", 0);
            }
        }
    }
    if (g_GetPosition == nullptr)
    {
        if (auto *c = Il2cpp::FindClass("UnityEngine.Transform"))
        {
            g_TransformClass = c;
            g_GetPosition = c->getMethod("get_position");
        }
    }
    if (g_GetBounds == nullptr)
    {
        if (auto *c = Il2cpp::FindClass("UnityEngine.Renderer"))
        {
            g_RendererClass = c;
            g_GetBounds = c->getMethod("get_bounds");
        }
    }
    if (g_IsNativeObjectAlive == nullptr)
    {
        if (auto *c = Il2cpp::FindClass("UnityEngine.Object"))
        {
            g_IsNativeObjectAlive = c->getMethod("IsNativeObjectAlive");
        }
    }
    // 相机这一组的门禁**必须是 g_WorldToScreenPoint 本身**。
    //
    // 旧写法是 `if (g_CameraClass == nullptr) { ...; if (ResolveMainCamera()) {...} }` ——
    // 门禁挂在「类」上，但真正需要的是「方法」。于是：
    //   第 1 次重试：Camera 类找到了（g_CameraClass 被赋值），
    //               但此刻场景还没加载完，ResolveMainCamera() 返回空
    //               → g_WorldToScreenPoint 保持 null
    //   第 2 次重试：g_CameraClass 非空 → **整个 if 块跳过**
    //   之后永远不会再解析
    //
    // 也就是说 ESP 的投影会**整个会话都是死的**，而原因是「场景当时没加载完」
    // —— 一个本来完全可以自愈的时序问题。
    //
    // 这正是我在第 39/40 轮反复遇到的「门禁挂在错误的变量上」：
    // 门禁必须挂在**你要确保的那个东西**上。
    if (g_WorldToScreenPoint == nullptr)
    {
        if (g_CameraClass == nullptr)
        {
            if (auto *c = Il2cpp::FindClass("UnityEngine.Camera"))
            {
                g_CameraClass = c;
            }
        }
        if (g_CameraClass != nullptr && ResolveMainCamera())
        {
            // 不能盲目取重载列表里的 [1]。Camera.WorldToScreenPoint 有
            // 多个重载（含带 MonoOrStereoscopicEye 的两参版本），按下标取
            // 很容易挑错签名；而 invoke 是按「尾部再塞一个 MethodInfo*」
            // 的约定直接 reinterpret 成函数指针调的，签名一错就等于把隐藏
            // 参数喂到枚举参数的位置上 → 寄存器/栈错乱。
            g_WorldToScreenPoint = g_CameraClass->getMethod("WorldToScreenPoint", 1);
        }
    }
}

void ObjectDrawManager::Initialize() {
    LOGI("初始化对象绘制管理器");

    ResolveDrawingApis();

    // 走 RefreshCamera 而不是直接赋值：它会把新相机挂上 GC 根，
    // 直接写裸指针会漏掉句柄。
    if (g_CameraClass)
    {
        RefreshCamera();
    }

    LOGD("对象绘制管理器: 方法解析 -> transform=%p position=%p name=%p alive=%p w2s=%p "
         "getComponent=%p bounds=%p",
         (void*)g_GetTransform, (void*)g_GetPosition, (void*)g_GetName,
         (void*)g_IsNativeObjectAlive, (void*)g_WorldToScreenPoint,
         (void*)g_GetComponent, (void*)g_GetBounds);

    // 解析不全要说一声，而且要用 **LOGI 不是 LOGD** —— LOGD 在正式版被
    // 编译掉，那样用户就是「ESP 不画框」且毫无线索。
    if (g_GetTransform == nullptr || g_GetPosition == nullptr || g_WorldToScreenPoint == nullptr)
    {
        LOGW("对象绘制: 部分方法尚未解析到（transform=%p position=%p w2s=%p）。"
             "会每 2 秒自动重试；如果一直失败，通常是勾选得太早（游戏还没加载完）"
             "或这个游戏裁掉了相应 API。",
             (void*)g_GetTransform, (void*)g_GetPosition, (void*)g_WorldToScreenPoint);
    }
}

// 惰性重试。渲染线程上调用。
void ObjectDrawManager::RetryPendingResolve()
{
    if (g_GetTransform && g_GetPosition && g_GetBounds && g_GetComponent &&
        g_IsNativeObjectAlive && g_CameraClass && g_WorldToScreenPoint)
    {
        return; // 都齐了
    }
    const double now = std::chrono::duration<double>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    // **指数退避**，不是固定 2 秒。
    //
    // FindClass 查不到时要遍历该程序集的所有类型做名字比对 —— 一个正常
    // 规模的 Unity 游戏是十万量级的类型。固定 2 秒重试 = 每 2 秒几万次
    // 字符串比较，**永远**持续下去（有的游戏就是没有某个 API，
    // 那它永远解析不到）。这是我上一轮引入的一笔永久开销。
    //
    // 2s → 4s → 8s … 封顶 60s：真正可能成功的那几次依然很快，
    // 确实解析不出来时成本衰减到可忽略。
    static double s_retryInterval = 2.0;
    if (now - s_lastResolveRetry < s_retryInterval)
    {
        return;
    }
    s_lastResolveRetry = now;
    s_retryInterval = std::min(s_retryInterval * 2.0, 60.0);

    ResolveDrawingApis();
    if (g_CameraClass)
    {
        RefreshCamera();
    }

    if (g_GetTransform && g_GetPosition && g_WorldToScreenPoint)
    {
        // 用 LOGI：正式版日志里能看到「晚到的解析」，是排障时的明确线索。
        LOGI("对象绘制: 方法解析已完成（重试生效），ESP 现在可用");
        // 成功后退回快速重试，以便应对后续的早期失败
        s_retryInterval = 2.0;
    }
    else if (s_retryInterval >= 60.0)
    {
        // 到顶了还解析不到，说明这个游戏就是没有相应 API。
        // 停在这里，别再每分钟白烧一次 CPU。
        LOGW("对象绘制: 多次重试仍无法解析到所需 API，已停止重试。"
             "这个游戏可能裁剪了对应功能（例如没有 Renderer 包围盒）。");
    }
}

// ---- 自检用的查询接口 ----

bool ObjectDrawManager::WorldToScreenAvailable()
{
    // 相机句柄是渲染线程独占的（见 ResolveMainCamera 的线程约束说明），
    // 而自检也在渲染线程上跑，所以这里直接取是安全的。
    return ResolveMainCamera() != nullptr && g_WorldToScreenPoint != nullptr;
}

bool ObjectDrawManager::RendererBoundsAvailable()
{
    return g_GetComponent != nullptr && g_GetBounds != nullptr && g_RendererClass != nullptr;
}

size_t ObjectDrawManager::DrawObjectCount()
{
    std::lock_guard<NeverDestroyedMutex> lock(g_drawMutex);
    return drawObjects.size();
}

size_t ObjectDrawManager::TotalRootCount()
{
    size_t total = 0;
    {
        std::lock_guard<NeverDestroyedMutex> lock(g_objectsMutex);
        total += g_cachedGameObjects.size();
    }
    {
        std::lock_guard<NeverDestroyedMutex> lock(g_drawMutex);
        for (const auto &d : drawObjects)
        {
            if (d.target.gameObjectHandle)
            {
                total++;
            }
            if (d.target.transformHandle)
            {
                total++;
            }
        }
    }
    // savedSet 是 static 的（ClassesTab::savedSet），在 ClassesTab.cpp 里。
    // 跨文件访问它的内部状态需要它自己暴露；这里只统计本模块持有的，
    // 数量级上够判断「是否在持续泄漏」了。
    return total;
}

void ObjectDrawManager::Shutdown() {
    LOGI("关闭对象绘制管理器");

    // 先置位并 join，确保扫描线程不再访问下面要清掉的全局状态。
    // 旧实现直接 clear() 而线程仍 detach 在跑，属于 use-after-free 窗口。
    g_shutdownRequested.store(true);
    if (g_scanThread.joinable())
        g_scanThread.join();

    {
        std::lock_guard<NeverDestroyedMutex> lock(g_objectsMutex);
        ClearCachedObjectListLocked();
    }
    {
        std::lock_guard<NeverDestroyedMutex> lock(g_drawMutex);
        // clear() 析构每个 GameObjectInfo，句柄随之释放。
        // 必须释放：句柄是 GC 的强根，漏掉就等于让游戏的对象永远回收不掉
        // （反复开关菜单会持续泄漏）。
        size_t before = drawObjects.size();
        drawObjects.clear();
        if (before)
        {
            LOGI("释放 %zu 个已绘制对象的 GC 根", before);
        }
    }
    g_lastObjectCount.store(0);
    g_rescanInProgress.store(false);
    g_rescanBusy.store(false);
    g_needsRescan.store(false);
    g_hasNewList.store(false);
    ReleaseMainCamera();

    // 允许再次启用
    g_shutdownRequested.store(false);
}

void ObjectDrawManager::SelectObject(const GameObjectInfo& obj) {
    std::lock_guard<NeverDestroyedMutex> lock(g_drawMutex);
    for (const auto& drawObj : drawObjects) {
        if (drawObj.target.gameObject == obj.gameObject) {
            return;
        }
    }

    DrawObject newDrawObj;
    // obj 是 const 引用但 GameObjectInfo 现在禁止拷贝，所以要自己重建一个条目
    // 并重新加根（不能直接搬 obj 的句柄过去：那会变成两个条目共用一个句柄，
    // 析构两次就 double free）。
    newDrawObj.target.gameObject = obj.gameObject;
    newDrawObj.target.transform = obj.transform;
    newDrawObj.target.worldPosition = obj.worldPosition;
    newDrawObj.target.screenPosition = obj.screenPosition;
    newDrawObj.target.name = obj.name;
    newDrawObj.target.isSelected = obj.isSelected;
    RootGameObject(newDrawObj.target);
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

    std::lock_guard<NeverDestroyedMutex> lock(g_drawMutex);
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
    // 加根：只要这个条目在列表里，GC 就不能回收它。
    RootGameObject(newDrawObj.target);
    newDrawObj.color = IM_COL32(rand() % 255, rand() % 255, rand() % 255, 255);
    newDrawObj.thickness = 2.0f;
    newDrawObj.drawLine = true;
    newDrawObj.drawBox = true;
    newDrawObj.drawCircle = false;

    drawObjects.push_back(std::move(newDrawObj));
}

void ObjectDrawManager::DeselectAll() {
    std::lock_guard<NeverDestroyedMutex> lock(g_drawMutex);
    drawObjects.clear();
}

void ObjectDrawManager::AddDrawObject(const GameObjectInfo& obj) {
    SelectObject(obj);
}

void ObjectDrawManager::RemoveDrawObject(Il2CppObject* gameObject) {
    std::lock_guard<NeverDestroyedMutex> lock(g_drawMutex);
    drawObjects.erase(
        std::remove_if(drawObjects.begin(), drawObjects.end(),
                       [gameObject](const DrawObject& obj) {
                           return obj.target.gameObject == gameObject;
                       }),
        drawObjects.end()
    );
}

void ObjectDrawManager::ClearAllDrawObjects() {
    std::lock_guard<NeverDestroyedMutex> lock(g_drawMutex);
    drawObjects.clear();
}

void ObjectDrawManager::UpdateGameObjects() {
    g_needsRescan = true;
}

bool ObjectDrawManager::WorldToScreen(const Vector3& worldPos, Vector3& screenPos) {
    auto *cam = ResolveMainCamera();
    if (!cam || !g_WorldToScreenPoint) return false;
    try {
        auto screen = g_WorldToScreenPoint->invoke_static<Vector3>(cam, worldPos);
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

// 算出对象的世界空间 AABB（轴对齐包围盒）在屏幕上的二维外接矩形。
//
// 为什么需要：旧的 DrawBox 是固定 ±20 像素 —— 一个远处的巨大建筑和一个
// 近处的小道具画出来一样大，ESP 完全没有「这东西有多大」的信息。
//
// 做法：Renderer.bounds 给出世界空间的 center + extents（AABB），
// 把 8 个角点逐个投影到屏幕，取 x/y 的最小/最大值。
//
// 一个必须处理的坑：WorldToScreenPoint 对 z<0（相机背后）的点返回的
// x/y 是镜像的，直接参与 min/max 会得到一个横跨整个屏幕的假框。
// 所以只采信相机前方的角点；一个都没采到就返回 false（物体整个在身后）。
//
// 返回 false 表示拿不到包围盒（没有 Renderer / 方法没解析 / 数值异常），
// 调用方回退到固定尺寸。
static bool ComputeScreenBounds(Il2CppObject *gameObject, ImVec2 &outMin, ImVec2 &outMax)
{
    auto *cam = ResolveMainCamera();
    if (!gameObject || !g_GetComponent || !g_GetBounds || !cam || !g_WorldToScreenPoint)
    {
        return false;
    }

    Il2CppObject *renderer = nullptr;
    try
    {
        renderer = gameObject->invoke_method<Il2CppObject *>(g_GetComponent);
    }
    catch (...)
    {
        return false;
    }
    if (renderer == nullptr || renderer->klass != g_RendererClass)
    {
        // 没有渲染器（空物体、纯逻辑对象）—— 本来就画不出包围盒。
        return false;
    }

    // UnityEngine.Bounds 是 struct（center + extents 两个 Vector3，24 字节）。
    // il2cpp 的方法返回结构体走隐藏返回缓冲区，由 C++ 编译器按 sret 约定
    // 处理，这里定义同样的布局即可。
    struct UnityBounds
    {
        Vector3 center;
        Vector3 extents;
    };

    UnityBounds bounds{};
    try
    {
        bounds = renderer->invoke_method<UnityBounds>(g_GetBounds);
    }
    catch (...)
    {
        return false;
    }

    // 数值校验：extents 非正、NaN、无穷 都说明这不是一个可用的包围盒。
    // 拿着这种值去算 8 个角点会直接产出 NaN 屏幕坐标，画出乱线。
    const Vector3 &c = bounds.center;
    const Vector3 &e = bounds.extents;
    const float coords[6] = {c.x, c.y, c.z, e.x, e.y, e.z};
    for (float v : coords)
    {
        if (std::isnan(v) || std::isinf(v))
        {
            return false;
        }
    }
    if (e.x < 0.f || e.y < 0.f || e.z < 0.f)
    {
        return false;
    }

    const float displayHeight = ImGui::GetIO().DisplaySize.y;
    bool haveAny = false;
    float minX = 0.f, maxX = 0.f, minY = 0.f, maxY = 0.f;

    for (int i = 0; i < 8; ++i)
    {
        // 用位组合枚举 8 个角点：第 i 位为 1 就取 +extents，取 0 就取 -。
        const Vector3 corner{c.x + ((i & 1) ? e.x : -e.x),
                              c.y + ((i & 2) ? e.y : -e.y),
                              c.z + ((i & 4) ? e.z : -e.z)};
        Vector3 screen;
        try
        {
            screen = g_WorldToScreenPoint->invoke_static<Vector3>(cam, corner);
        }
        catch (...)
        {
            return false;
        }
        if (!(screen.z > 0.f) || std::isnan(screen.x) || std::isnan(screen.y) ||
            std::isinf(screen.x) || std::isinf(screen.y))
        {
            continue;
        }
        const float sx = screen.x;
        const float sy = displayHeight - screen.y;
        if (!haveAny)
        {
            minX = maxX = sx;
            minY = maxY = sy;
            haveAny = true;
        }
        else
        {
            minX = std::min(minX, sx);
            maxX = std::max(maxX, sx);
            minY = std::min(minY, sy);
            maxY = std::max(maxY, sy);
        }
    }

    if (!haveAny)
    {
        return false;
    }

    outMin = ImVec2(minX, minY);
    outMax = ImVec2(maxX, maxY);
    return true;
}

void ObjectDrawManager::DrawBox(const DrawObject& drawObj) {
    auto drawList = ImGui::GetForegroundDrawList();

    // 优先用真实包围盒；拿不到（没有 Renderer / 整个在相机背后 /
    // 数值异常）时回退到固定尺寸。
    if (drawObj.target.hasScreenBounds) {
        drawList->AddRect(drawObj.target.screenBoundsMin, drawObj.target.screenBoundsMax,
                          drawObj.color, 0.0f, 0, drawObj.thickness);
        return;
    }

    ImVec2 pos(drawObj.target.screenPosition.x, drawObj.target.screenPosition.y);
    const float boxSize = 20.0f;
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
    // 拷**句柄**，不拷裸指针：句柄在列表被替换之前一直是有效的根，
    // 而裸指针随时可能因为一次 GC 变成野的（第 100 轮）。
    std::vector<uint32_t> handles;
    {
        std::lock_guard<NeverDestroyedMutex> lock(g_objectsMutex);
        handles = g_cachedHandles;
    }

    auto *cam = ResolveMainCamera();
    if (!cam || !g_WorldToScreenPoint || !g_GetTransform || !g_GetPosition)
        return result;

    result.reserve(handles.size());
    for (auto h : handles) {
        // 句柄为 0 = 扫描时就没加上根；对象可能已经被回收。
        // **不解引用裸指针**，直接跳过。
        Il2CppObject *go = h ? Il2cpp::GC::GetHandleTarget(h) : nullptr;
        if (!go) continue;
        if (!IsValidGameObject(go)) continue;

        try {
            auto transform = go->invoke_method<Il2CppObject*>(g_GetTransform);
            if (!transform) continue;
            auto position = transform->invoke_method<Vector3>(g_GetPosition);
            auto screen = g_WorldToScreenPoint->invoke_static<Vector3>(cam, position);
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

            // 这个快照会被渲染线程拿着，直到用户点「添加」。那之前对象可能
            // 被游戏销毁、被 GC 回收；点击时 SelectObject 会对这个裸指针加根，
            // 而 NewHandle 传进去的若是已回收对象，GC 句柄表会被写坏。
            // 所以在快照存活期间就加根，句柄随快照析构释放。
            RootGameObject(info);

            result.push_back(std::move(info));
        } catch (...) {
            continue;
        }
    }
    return result;
}

// DrawUI 用的**非拥有**快照。
//
// 为什么不用 DrawObject 直接快照：GameObjectInfo 拥有 GC 句柄、拷贝构造
// 被 delete 了（句柄只能有一个主人，拷贝会 double free）。
// 所以早期实现写的是 `drawSnapshot = std::move(drawObjects)` ——
// 那不是快照，那是**把绘制列表搬空**。
//
// 而 DrawUI 是在 `ImGui::BeginTabItem("对象绘制")` 里面调的，
// 也就是**这一页可见的每一帧**都会搬一次。同一帧里后面的顺序是
//
//     DrawUI()   -> 把 drawObjects 搬空
//     Tick()     -> 遍历 drawObjects（空的，什么都不更新）
//     DrawAll()  -> 遍历 drawObjects（空的，**一个都不画**）
//
// 于是只要用户停在「对象绘制」这一页，ESP 就停了；要等下一次后台重扫
// （5 秒间隔）才恢复 —— 表现是绘制每隔 5 秒闪一下。
//
// 而这一行代码只是要**显示**列表，所以下面这几个字段拷一份就够了，
// 不需要转移所有权。
struct DrawRow
{
    Il2CppObject *gameObject = nullptr; // 裸指针，仅用于显示与定位（回写按它找）
    std::string name;                   // 拷贝出来的名字，UI 期间不依赖原对象
    ImColor color;
    float thickness = 2.0f;
    bool drawLine = true;
    bool drawBox = false;
    bool drawCircle = false;
};

void ObjectDrawManager::DrawUI() {
    // 快照只拷**显示/编辑需要的那几个字段**，不碰所有权。
    // 锁只护住「读 drawObjects」这一段：后面所有绘制都在锁外，
    // 和第 24 轮那条「锁的作用域正好到 move 为止」的教训一致。
    std::vector<DrawRow> drawSnapshot;
    {
        std::lock_guard<NeverDestroyedMutex> lock(g_drawMutex);
        drawSnapshot.reserve(drawObjects.size());
        for (const auto &d : drawObjects)
        {
            DrawRow row;
            row.gameObject = d.target.gameObject;
            row.name = d.target.name;
            row.color = d.color;
            row.thickness = d.thickness;
            row.drawLine = d.drawLine;
            row.drawBox = d.drawBox;
            row.drawCircle = d.drawCircle;
            drawSnapshot.push_back(std::move(row));
        }
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

    // 距离过滤 + 距离显示开关。
    // 有了距离之后才可能有「只画近处」这种需求：场景里几百米外的东西
    // 画出来只是屏幕角落一堆看不清的点，还会遮挡视线。
    ImGui::Checkbox("显示距离", &g_showDistance);
    if (ImGui::Checkbox("真实包围盒", &g_useRealBounds))
    {
        // GetComponent<Renderer>() 没解析出来时开这个也没用，直接告诉用户。
        if (g_useRealBounds && !g_GetComponent)
        {
            LOGW("未解析到 GetComponent<UnityEngine.Renderer>，真实包围盒不可用");
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("用 Renderer.bounds 的真实大小画框，而不是固定像素。\n"
                          "拿不到 Renderer（空物体/纯逻辑对象）时自动退回固定尺寸。");
    }
    if (ImGui::Checkbox("限制最大距离", &g_limitDistance))
    {
        if (!g_limitDistance)
        {
            maxDrawDistance = 0.f;
        }
        else if (maxDrawDistance <= 0.f)
        {
            maxDrawDistance = 100.f;
        }
    }
    if (g_limitDistance)
    {
        ImGui::SetNextItemWidth(160.f);
        ImGui::SliderFloat("##maxdist", &maxDrawDistance, 1.f, 1000.f, "%.0f m");
    }
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

    // GC 根数量。让「加根 / 释放」在界面上可见 —— 句柄只增不减意味着
    // 游戏对象永远回收不掉（内存泄漏），这是加根实现最容易出的错。
    {
        size_t roots = 0;
        size_t missing = 0;
        {
            std::lock_guard<NeverDestroyedMutex> lock(g_drawMutex);
            for (const auto &d : ObjectDrawManager::drawObjects)
            {
                if (d.target.gameObjectHandle)
                    roots++;
                else
                    missing++;
            }
        }
        ImGui::Text("GC 强引用: %zu%s", roots, missing ? " (!)" : "");
        if (missing)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.2f, 1.f), "(%zu 个未加根)", missing);
        }
    }

    // 场景切换提示（几秒后自动消失）
    {
        std::lock_guard<NeverDestroyedMutex> lock(g_noticeMutex);
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
        existing.insert(d.gameObject);
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

            // 注意：drawSnapshot 是**字段副本**，回写必须按 gameObject 定位，
            // 不能用快照下标 i 去索引 drawObjects（两者大小可能不同 → 越界写）。
            auto* go = drawSnapshot[i].gameObject;

            // 名字长度不受控（GameObject 名字是游戏作者起的，可以很长），
            // 用表格给名字列一个上限宽度，后面两个控件固定宽 ——
            // 否则 SameLine 会把颜色/移除按钮顶到屏幕外，等于按钮点不到。
            if (ImGui::BeginTable("##drawrow", 3, ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("color", ImGuiTableColumnFlags_WidthFixed, 24.0f);
                ImGui::TableSetupColumn("act", ImGuiTableColumnFlags_WidthFixed, 64.0f);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(drawSnapshot[i].name.c_str());
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", drawSnapshot[i].name.c_str());
                }
                ImGui::TableNextColumn();
                ImVec4 color = ImGui::ColorConvertU32ToFloat4(drawSnapshot[i].color);
                ImGui::ColorButton("##color", color, ImGuiColorEditFlags_NoTooltip, ImVec2(20, 20));
                ImGui::TableNextColumn();
                if (ImGui::Button("移除")) {
                    RemoveDrawObject(go);
                    ImGui::EndTable();
                    ImGui::PopID();
                    break;
                }
                ImGui::EndTable();
            }

            // 这里**不能**接 SameLine：EndTable 之后光标已经在新的一行，
            // 而「上一个控件」是表格内部的项。SameLine 会按表格的裁剪矩形
            // 定位，把三个勾选框塞进表格右边那条窄缝里。
            // 直接另起一行，简单且可预期。
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
