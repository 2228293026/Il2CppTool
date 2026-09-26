#include "ClassesTab.h"
#include "ChangeLog.h"
#include "Includes/NeverDestroyedMutex.h"
#include "Il2cpp/Il2cpp.h"
#include "KittyMemory/KittyMemory.h"
#include "Tool/Keyboard.h"
#include "Tool/Patcher.h"
#include "Tool/Tool.h"
#include "Tool/Util.h"
#include "imgui/imgui.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include "sstream"

static bool SpawnDetached(const std::function<void()> &body, const char *what,
                          const std::function<void()> &onFail = {})
{
    try
    {
        std::thread(body).detach();
        return true;
    }
    catch (const std::exception &e)
    {
        LOGE("无法创建后台线程(%s): %s", what, e.what());
    }
    catch (...)
    {
        LOGE("无法创建后台线程(%s): 未知异常", what);
    }
    if (onFail)
    {
        onFail();
    }
    return false;
}

extern std::vector<Il2CppImage *> g_Images;
extern Il2CppImage *g_Image;

// 「Find Objects」后台扫描的结果交接：
// 后台线程只往 g_pendingScanResults 写，真正并入 objectMap 由 UI 线程做。
// objectMap 里的 vector 在 UI 里是被边遍历边 erase 的，如果让后台线程直接
// objectMap[klass] = ...，UI 手上的引用会被整个换掉 → 迭代野指针 / UAF。
static NeverDestroyedMutex g_scanResultMutex;
static std::unordered_map<Il2CppClass *, std::vector<Il2CppObject *>> g_pendingScanResults;
// 后台扫描的**失败原因**。和 g_pendingScanResults 共用一把锁。
//
// 为什么需要它：扫描失败时如果什么都不发布，objectMap 会保留**上一次的
// 结果**，用户点了「Find Objects」看到列表没变，就会以为「扫出来就是这些」
// —— 而不是「根本没扫成功」。界面上没有任何提示。
static std::unordered_map<Il2CppClass *, std::string> g_pendingScanErrors;

// savedSet 里是「用户手动保存、要长期留着」的对象，同样必须保活：
// 它们在列表里可能挂很久，游戏侧随时可能销毁对应实体。
// 因为是 set（按指针去重），用一个并行的句柄表来管 GC 根。
static std::unordered_map<Il2CppObject *, uint32_t> g_savedHandles;

static void SaveObjectWithRoot(Il2CppObject *obj)
{
    if (!obj)
    {
        return;
    }
    if (g_savedHandles.find(obj) == g_savedHandles.end())
    {
        auto handle = Il2cpp::GC::NewHandle(obj);
        if (handle == 0)
        {
            LOGW("保存对象加根失败: %p", static_cast<void *>(obj));
        }
        g_savedHandles[obj] = handle;
    }
}

static void UnsaveObjectWithRoot(Il2CppObject *obj)
{
    auto it = g_savedHandles.find(obj);
    if (it != g_savedHandles.end())
    {
        Il2cpp::GC::FreeHandle(it->second);
        g_savedHandles.erase(it);
    }
}

// 已保存集合里取出的对象，走句柄确认仍然有效。
static Il2CppObject *ResolveSaved(Il2CppObject *obj)
{
    if (!obj)
    {
        return nullptr;
    }
    auto it = g_savedHandles.find(obj);
    if (it != g_savedHandles.end() && it->second)
    {
        return Il2cpp::GC::GetHandleTarget(it->second);
    }
    return obj;
}

// 「剩下多少宽度可用」，**并且保证不为负**。
//
// ImGui 的控件宽度如果算成负数，ImGui 自己并不知道这不对：
// 画出来是一个退化的矩形（点不中），或者和右边的控件叠在一起。
// 调试版有断言，正式版（我们就是正式版）静默画错。
//
// 手机上这个风险是实打实的：面板宽度随窗口缩放、tab 栏嵌套、
// 字体缩放而变，而这里减掉的是**固定像素**的按钮宽度。
// 窄一点的设备上，一个 "Save" + "W" 就要 70 多像素，
// 剩下的不够时就是负数。
static float AvailMinus(float reserved)
{
    return std::max(0.0f, ImGui::GetContentRegionAvail().x - reserved);
}

// 留出一个固定宽度按钮 + 边框内边距。
static float AvailMinusButton(const char *label, float padFactor = 5.0f)
{
    return AvailMinus(ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * padFactor);
}

constexpr int MAX_CLASSES = 500;

// ===================================================================
// 关注值（Watch）
// ===================================================================
//
// 实现要点：
//
// 1. **必须加 GC 根**。关注列表是跨帧持久的，而游戏随时可能销毁对应实体。
//    每 200ms 重读一次，两次之间对象被回收 = 拿野指针解引用 → 崩溃。
//    借用 savedSet 那套句柄管理，但**自己持有句柄**：watch 是「用户盯着看的
//    那个具体对象」，不应该因为它没被 Save 过就不保活。
//
// 2. **走句柄解析**，不直接用裸指针。对象在移动/重建后地址可能变，
//    gchandle 是唯一可靠的存活判断。
//
// 3. 轮询频率 200ms。不是每帧 —— dump 一条路径会构造 JSON、
//    走类型分派，N 个关注项每帧跑是不必要的开销。
//    又不是几秒一次 —— 那样「盯着看」就没意义了。
static std::vector<ClassesTab::Watch> g_watches;
static double g_watchLastPoll = 0.0;

void ClassesTab::AddWatch(Il2CppObject *object, const std::vector<std::string> &paths,
                          const std::string &label)
{
    if (!object || paths.empty())
    {
        return;
    }
    // 同一个对象的同一条路径不重复加
    for (auto &w : g_watches)
    {
        if (w.object == object && w.paths == paths)
        {
            return;
        }
    }
    auto handle = Il2cpp::GC::NewHandle(object);
    if (handle == 0)
    {
        LOGW("关注失败：加根没成功 %p", static_cast<void *>(object));
        return;
    }
    if (g_watches.size() >= 64)
    {
        LOGW("关注列表已满（64 条），忽略新增");
        Il2cpp::GC::FreeHandle(handle);
        return;
    }
    g_watches.push_back({object, handle, paths, label, {}, false, false});
    ChangeLog::Record(ChangeLog::Kind::Watch, label, "已加入关注（每 200ms 刷新）");
    // 立刻让下一帧重读一次。否则新加的这一项要等最多 200ms 才显示值，
    // 期间 lastValue 是空串 —— 界面上就是「加完是空的」，看着像没加上。
    g_watchLastPoll = 0.0;
}

void ClassesTab::RemoveWatchAt(size_t index)
{
    if (index >= g_watches.size())
    {
        return;
    }
    if (g_watches[index].handle)
    {
        Il2cpp::GC::FreeHandle(g_watches[index].handle);
    }
    g_watches.erase(g_watches.begin() + index);
}

void ClassesTab::ClearWatches()
{
    for (auto &w : g_watches)
    {
        if (w.handle)
        {
            Il2cpp::GC::FreeHandle(w.handle);
        }
    }
    g_watches.clear();
}

size_t ClassesTab::WatchCount()
{
    return g_watches.size();
}

// 把一个 JSON 标量渲染成短文本。不是标量就返回空串（不显示）。
static std::string JsonToText(const nlohmann::ordered_json &j)
{
    if (j.is_null())
    {
        return "null";
    }
    if (j.is_boolean())
    {
        return j.get<bool>() ? "true" : "false";
    }
    if (j.is_number_unsigned())
    {
        return std::to_string(j.get<uint64_t>());
    }
    if (j.is_number_integer())
    {
        return std::to_string(j.get<int64_t>());
    }
    if (j.is_number_float())
    {
        char buf[64]{0};
        // %g：血量之类不需要小数点后 15 位；但也要能显示 0.5 这种。
        snprintf(buf, sizeof(buf), "%.6g", j.get<double>());
        return buf;
    }
    if (j.is_string())
    {
        return j.get<std::string>();
    }
    return {};
}

void ClassesTab::DrawWatches()
{
    if (g_watches.empty())
    {
        return;
    }
    ImGui::Separator();
    ImGui::Text("关注值（每 200ms 自动刷新）");
    ImGui::SameLine();
    if (ImGui::SmallButton("全部清除"))
    {
        ClearWatches();
        return;
    }

    // 低频轮询。放在画之前一次性更新所有条目。
    const double now = std::chrono::duration<double>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    if (now - g_watchLastPoll >= 0.2)
    {
        g_watchLastPoll = now;
        for (auto &w : g_watches)
        {
            w.changed = false;
            Il2CppObject *live =
                w.handle ? Il2cpp::GC::GetHandleTarget(w.handle) : nullptr;
            if (!live)
            {
                w.invalid = true;
                continue;
            }
            w.invalid = false;
            try
            {
                // paths 是 non-const 引用（dump 的签名如此），这里用副本。
                auto paths = w.paths;
                auto result = live->dump(paths);
                std::string text = JsonToText(result.second);
                if (text.empty())
                {
                    // 不是标量（子对象 / 数组）—— 关注叶子字段才有意义。
                    text = "<非标量>";
                }
                if (text != w.lastValue)
                {
                    w.changed = !w.lastValue.empty();
                    w.lastValue = text;
                }
            }
            catch (const std::exception &e)
            {
                w.invalid = true;
                w.lastValue = std::string("<读取失败: ") + e.what() + ">";
            }
        }
    }

    for (size_t i = 0; i < g_watches.size();)
    {
        auto &w = g_watches[i];
        // 固定成两列的表格。不用「标签 + SameLine + 值」：
        // 标签长度不受限（类名可以很长），值会被 SameLine 顶到屏幕外 ——
        // 而值是这一行唯一真正要看的**东西**，标签只是定位用的。
        // 表格给标签列一个上限宽度，超出部分被裁掉。
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::BeginTable("##watchrow", 2, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::GetContentRegionAvail().x * 0.55f);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (w.changed)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(120, 255, 120, 255));
            }
            // 完整标签放 tooltip，被裁掉的部分还能看全。
            ImGui::TextUnformatted(w.label.c_str());
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", w.label.c_str());
            }
            ImGui::PopStyleColor();

            ImGui::TableNextColumn();
            if (w.invalid)
            {
                ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "%s",
                                   w.lastValue.empty() ? "<对象已失效>" : w.lastValue.c_str());
            }
            else
            {
                ImGui::TextUnformatted(w.lastValue.c_str());
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x"))
            {
                ImGui::EndTable();
                ImGui::PopID();
                RemoveWatchAt(i);
                continue;
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
        ++i;
    }
}

int maxLine{5};
std::unordered_map<void *, HookerData> hookerMap;
NeverDestroyedMutex hookerMtx;

#ifndef USE_FRIDA
void hookerHandler(void *address, DobbyRegisterContext *ctx)
{
    // 这段跑在**游戏线程**上 —— 每次被 hook 的方法被调用都会进来一次。
    // 所以这里的每一分开销都是直接加在游戏帧时间上的。
    //
    // 旧实现在这里 snprintf 出一个 512 字节的 "地址 | 方法名"，再拿它去
    // visited 里做最多 maxLine 次 std::string 比较。方法每秒被调上千次时，
    // 就是每秒上千次格式化 + 上千次字符串比较，外加全程持着全局互斥锁
    // —— 工具本身成了拖慢游戏的原因。
    //
    // 现在：去重改用指针比较（address 已经是 hookerMap 的 key，精确且免费），
    // 字符串只在**首次**命中、需要新建列表项时格式化一次。
    std::lock_guard guard(hookerMtx);
    auto it = hookerMap.find(address);
    if (it == hookerMap.end())
    {
        // 已经被摘钩子但回调还在路上。不碰任何数据。
        return;
    }
    auto &hookerData = it->second;
    // relaxed：纯统计计数器，丢一次更新无所谓，换取不在游戏路径上加锁。
    hookerData.hitCount.fetch_add(1, std::memory_order_relaxed);
    hookerData.time = 1.f;

    int i = 0;
    for (auto vit = HookerData::visited.rbegin(); vit != HookerData::visited.rend(); ++vit)
    {
        if (i >= maxLine)
        {
            break;
        }
        // 指针比较取代字符串比较
        if (vit->address == address)
        {
            vit->goneTime = 10.f;
            vit->time = 2.f;
            vit->hitCount++;
            return;
        }
        i++;
    }

    // 走到这里说明这个方法第一次出现在「最近调用」列表里 ——
    // 此时才做一次字符串格式化。
    HookerTrace trace;
    trace.address = address;
    trace.time = 2.f;
    trace.goneTime = 10.f;
    trace.hitCount = 0;
    if (hookerData.method)
    {
        const char *name = hookerData.method->getName();
        // 方法名长度不受控（混淆过的 il2cpp 元数据可以很长），
        // 128 字节固定缓冲 + 无界 sprintf 就是栈溢出。
        char buffer[512]{0};
        snprintf(buffer, sizeof(buffer), "%p | %s", (void *)hookerData.method->getAbsAddress(),
                 name ? name : "?");
        trace.name = buffer;
    }
    HookerData::visited.push_back(std::move(trace));
}
#endif

// 取某个类的方法列表（含参数信息），带缓存。
//
// 旧实现是「函数内 static + 只记 lastClass」的**单项缓存**，而且那份
// static 是**所有 ClassesTab 实例共享**的。问题有两个：
//
// 1. 缓存只有一项。弹窗是在「对每个对象遍历」的循环里打开的，
//    相邻对象的类几乎必然不同 —— 缓存命中率接近 0，每次都要重新
//    getMethods() + 每个方法 getParamsInfo()，而那正是这个函数本该避免的开销。
// 2. static 跨 tab 共享。函数返回的是**引用**，一旦在遍历过程中因为别处
//    又调了一次而重建，调用方手里的引用当场失效（列表被 clear + 重填）。
//    现在是「拿引用后立刻用完」，所以没炸，但这是靠巧合成立的。
//
// 现在改成**按类缓存**的成员 map：
// - 每个 tab 各有一份，tab 之间不再互相冲刷；
// - 命中任意类都直接返回，不需要重建；
// - 返回的引用在缓存被淘汰前一直有效。
//
// 只在渲染线程调用（MethodPopup 展开时），所以不需要额外加锁。
ClassesTab::MethodList &ClassesTab::buildMethodMap(Il2CppClass *klass)
{
    if (klass == nullptr)
    {
        // 返回一个空的静态列表而不是崩掉。调用方会走 "No methods" 分支。
        static MethodList empty;
        return empty;
    }

    auto it = methodCache.find(klass);
    if (it != methodCache.end())
    {
        return it->second;
    }

    MethodList methods;
    auto rawMethods = klass->getMethods();
    LOGD("Rebuilding %s | %lu methods", klass->getName() ? klass->getName() : "?", rawMethods.size());
    methods.reserve(rawMethods.size());
    for (auto method : rawMethods)
    {
        methods.push_back({method, method->getParamsInfo()});
    }

    // 缓存要有上限。方法列表是稳定的元数据，命中率会一直很高，
    // 但游戏可以动态加载 assembly，无界增长迟早吃掉内存。
    // 到上限时整体清空：宁可下一次重建，也不要维护复杂的 LRU。
    if (methodCache.size() >= kMethodCacheLimit)
    {
        LOGI("方法缓存达到上限 (%zu)，清空重建", methodCache.size());
        methodCache.clear();
    }
    auto inserted = methodCache.emplace(klass, std::move(methods));
    LOGD("Rebuilt %zu methods", inserted.first->second.size());
    return inserted.first->second;
}

ClassesTab::ClassesTab()
{
    selectedImage = g_Image;

    for (int i = 0; i < g_Images.size(); i++)
    {
        if (g_Images[i] == selectedImage)
        {
            selectedImageIndex = i;
            break;
        }
    }
    // g_Image 为空时（il2cpp 还没就绪 / 没有可用 assembly）不能去
    // getClasses() —— 那是空指针解引用。这里不预取，交给下面那次异步
    // 筛选去处理（它内部对 selectedImage 有判空）。
    //
    // 注意这里**不再**同步调 getClasses()：那是整个 assembly 的类枚举，
    // 而构造发生在 Tool::Init → on_init → 渲染线程上，正是启动卡顿的一部分。
    // 改成投递一次后台筛选，结果在第一次 Draw 时被认领。
    FilterClasses(filter);
}

ClassesTab::Paths &ClassesTab::getJsonPaths(Il2CppObject *object)
{
    return dataMap[object].second;
}

void ClassesTab::setJsonObject(Il2CppObject *object)
{
    // std::vector<uintptr_t> visited{};
    // dataMap[object].first = object->dump(visited, 9999);
    dataMap[object].first = object->dump({});

    tabMap.emplace(object, true);
}

ClassesTab::Json &ClassesTab::getJsonObject(Il2CppObject *object)
{
    return dataMap[object].first.second;
}

// 构造「<说明> [<地址>]」形式的控件标签。
//
// 这个字符串**同时是 ImGui 的控件 ID**，所以必须**逐个对象唯一**。
//
// 直接写 `snprintf(buff, 256, "%s [%p]", name, ptr)` 是不安全的：
// name 来自 il2cpp 元数据（类名、参数名），而元数据本来就不要求是
// 合法 C# 标识符 —— 混淆游戏里名字可以很长。一旦长到把 " [%p]" 挤掉，
// 两个**不同**的对象就会生成**同一个** buff → ID 冲突 → 点 A 选中 B。
//
// 而且这种错误完全静默：界面上看不出任何异常，只是「偶尔选错对象」。
//
// 所以显式给地址留足空间：64 位下 "%p" 是 "0x" + 16 位十六进制 = 18 字符，
// 加上空格和方括号共 20。前面的文字用精度截断。
static void FormatObjectButton(char (&buff)[256], const char *desc, const void *object)
{
    snprintf(buff, sizeof(buff), "%.220s [%p]", desc ? desc : "?", object);
}

void ClassesTab::ImGuiObjectSelector(int id, Il2CppClass *klass, const char *prefix,
                                     std::function<void(Il2CppObject *)> onSelect, bool canNew)
{
    ImGui::PushID(id);
    // liveObjects() 的结果**按低频缓存**，不要每帧重算。
    //
    // 它对每个对象做一次 il2cpp_gchandle_get_target（还分配一整个 vector）。
    // 「Find Objects」扫一个大类可能有几万个对象 —— 每帧几万次句柄解析，
    // 打开选择器时界面会明显卡顿。
    //
    // 但它只需要回答「哪些对象还活着」，这个信息**不逐帧变化**：
    // 对象被 GC 回收是低频事件。所以 250ms 刷新一次完全够用，
    // 开销降到 1/15 左右。
    //
    // 列表变空时立刻重算，否则刚重扫完会一直显示空列表。
    // 返回**拷贝**而不是引用：下面的循环会 `live.erase(...)`（按位置删，
    // 删完不递增下标），直接给引用会把缓存改坏。
    // 拷贝只是 memcpy，比 N 次句柄解析便宜得多 —— 真正要省的是后者。
    static std::unordered_map<void *, std::pair<double, std::vector<Il2CppObject *>>> liveCache;
    auto liveOf = [&](Il2cpp::GC::RootedObjectList &list) -> std::vector<Il2CppObject *>
    {
        auto &entry = liveCache[klass];
        const double now = std::chrono::duration<double>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
        if (entry.second.empty() || now - entry.first > 0.25)
        {
            entry.second = list.liveObjects();
            entry.first = now;
        }
        return entry.second;
    };
    // 扫描标记用 shared_ptr<atomic<bool>> 持有：后台线程拿到的地址必须稳定。
    // 旧代码是 std::unordered_map<void*,bool> + 捕获 bool&，UI 往 map 里再插一个
    // key 就会 rehash，那个引用当场悬空，后台线程再写就是堆破坏。
    static std::unordered_map<void *, std::shared_ptr<std::atomic<bool>>> scanState;
    std::shared_ptr<std::atomic<bool>> &scanFlag = scanState[klass];
    if (!scanFlag)
        scanFlag = std::make_shared<std::atomic<bool>>(false);
    bool scanning = scanFlag->load();
    if (ImGui::Button("Find Objects"))
    {
        scanFlag->store(true);
        auto keepAlive = scanFlag;
        // 线程创建失败会让 std::thread 的构造函数抛异常 —— 裸写就是
        // std::terminate，整个游戏进程崩掉。统一走 SpawnDetached。
        // （它还会在失败时复位 scanFlag，否则按钮会永远显示「扫描中…」。）
        Il2CppClass *scannedKlass = klass;
        Il2CppClass *scanFailedKlass = klass;
        SpawnDetached(
            [keepAlive, scannedKlass]()
            {
                auto klass = scannedKlass;
                // 后台线程是 il2cpp 的 foreign thread：GC::FindObjects 会 stop_gc_world
                // 并遍历 GC 结构，不 attach 就是崩溃/静默错数据；用完必须 detach，
                // 否则 il2cpp 的 attached-thread 表里会留下悬空条目。
                if (!Il2cpp::EnsureAttached())
                {
                    LOGE("对象扫描: 无法 attach 到 il2cpp VM");
                    // 必须**发布一个失败结果**，不能什么都不写。
                    // 什么都不写的话：用户点了「Find Objects」，界面毫无变化
                    // （旧的 objectMap 原样保留 → 看起来像「扫出来就是这些」），
                    // 界面上没有任何提示，只有 logcat 里一行 LOGE。
                    {
                        std::lock_guard<NeverDestroyedMutex> lock(g_scanResultMutex);
                        g_pendingScanErrors[klass] = "无法 attach 到 il2cpp VM";
                    }
                    keepAlive->store(false);
                    return;
                }
                // RAII：FindObjects 内部 vector 扩容失败会抛异常，
                // 没有守卫就会带着已挂载的线程直接 terminate。
                struct DetachGuard
                {
                    ~DetachGuard() { Il2cpp::Detach(); }
                } detachGuard;

                try
                {
                    auto objs = Il2cpp::GC::FindObjects(klass);
                    std::lock_guard<NeverDestroyedMutex> lock(g_scanResultMutex);
                    g_pendingScanResults[klass] = std::move(objs);
                    // 成功了就清掉上一轮的失败提示。
                    g_pendingScanErrors.erase(klass);
                }
                catch (const std::exception &e)
                {
                    LOGE("FindObjects(%s) 失败: %s", klass ? klass->getName() : "?", e.what());
                    std::lock_guard<NeverDestroyedMutex> lock(g_scanResultMutex);
                    g_pendingScanErrors[klass] = e.what();
                }
                catch (...)
                {
                    LOGE("FindObjects 未知异常");
                    std::lock_guard<NeverDestroyedMutex> lock(g_scanResultMutex);
                    g_pendingScanErrors[klass] = "扫描时发生未知异常";
                }
                keepAlive->store(false);
            },
            "Find Objects",
            [keepAlive, scanFailedKlass]()
            {
                // 线程没起来：复位标志，否则按钮会永远显示「扫描中…」，
                // 而且界面上没有任何解释。
                keepAlive->store(false);
                std::lock_guard<NeverDestroyedMutex> lock(g_scanResultMutex);
                g_pendingScanErrors[scanFailedKlass] = "无法创建扫描线程（线程资源不足）";
            });
    }
    ImGui::PopID();

    // 把后台线程的结果并进来（objectMap 只在 UI 线程被改动）
    std::string scanError;
    {
        std::lock_guard<NeverDestroyedMutex> lock(g_scanResultMutex);
        if (!g_pendingScanResults.empty())
        {
            for (auto &[pendingKlass, pending] : g_pendingScanResults)
            {
                // 旧条目（及其句柄）在这里被 RootedObjectList 的赋值运算符释放，
                // 新的一批同时加根。
                objectMap[pendingKlass].reset(std::move(pending));
            }
            g_pendingScanResults.clear();
        }
        if (!g_pendingScanErrors.empty())
        {
            if (auto it = g_pendingScanErrors.find(klass); it != g_pendingScanErrors.end())
            {
                scanError = it->second;
            }
            g_pendingScanErrors.clear();
        }
    }
    if (!scanError.empty())
    {
        ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "对象扫描失败: %s", scanError.c_str());
        ImGui::TextDisabled("下面的列表是「上一次」的结果，不是本次的。");
    }
    ImGuiIO &io = ImGui::GetIO();
    float width = io.DisplaySize.x;
    float height = io.DisplaySize.y;
    // FIXME: DRY!!
    {
        auto &objects = objectMap[klass];
        ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
        ImGui::BeginChild("##ScrollingObjects", ImVec2(width / 1.4f, 0), ImGuiChildFlags_AutoResizeY);
        {
            ImGui::SeparatorText("Result Object");
            if (objects.empty())
            {
                if (scanning)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(50, 255, 50, 255));
                }
                else
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
                }
                if (scanning)
                {
                    ImGui::Text("Scanning...");
                }
                else
                {
                    ImGui::Text("Nothing...");
                }
                ImGui::PopStyleColor();
            }
            else
            {
                // liveObjects() 会剔除已被 GC 回收的条目（并释放其句柄），
                // 下面遍历的是仍然有效的对象 —— 之后 object->klass 才安全。
                auto live = liveOf(objects);
                if (live.size() > 100)
                {
                    ImGui::Text("Showing 100 of %zu objects", live.size());
                }
                size_t limit = live.size() > 100 ? 100 : live.size();
                for (size_t i = 0; i < limit;)
                {
                    auto object = live[i];
                    if (object == nullptr || object->klass == nullptr)
                    {
                        ++i;
                        continue;
                    }
                    const char *className = object->klass->getName();
                    // 标签同时是 ImGui 的控件 ID，必须逐个对象唯一 ——
                    // 理由和精度截断都见 FormatObjectButton 的注释。
                    char buff[256];
                    FormatObjectButton(buff, prefix, object);
                    auto size = ImGui::GetWindowSize();
                    if (ImGui::Button(buff, ImVec2(size.x / 1.5, 0)))
                    {
                        onSelect(object);
                    }
                    ImGui::SetItemTooltip("%s", className ? className : "?");
                    ImGui::SameLine();
                    ImGui::PushID(buff);
                    if (ImGui::Button("Remove"))
                    {
                        // live[i] 与 objects[i] 的下标不一定对应（live 剔除了
                        // 失效项），所以按指针找原始下标再删。
                        const auto &raw = objects.raw();
                        for (size_t k = 0; k < raw.size(); k++)
                        {
                            if (raw[k] == object)
                            {
                                objects.removeAt(k);
                                break;
                            }
                        }
                        live.erase(live.begin() + i);
                        // 不递增：删掉后同一位置换成下一个
                        limit = live.size() > 100 ? 100 : live.size();
                        if (i >= limit)
                        {
                            break;
                        }
                    }
                    else
                    {
                        ++i;
                    }
                    ImGui::PopID();
                }
            }
        }
        ImGui::EndChild();
    }
    ImGui::Separator();

    // ImGui::Text("Inherited from %s", klass->getName());
    // for (auto [setKlass, _] : savedSet)
    // {
    //     if (klass != setKlass && Il2cpp::IsClassParentOf(setKlass, klass))
    //     {
    //         ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
    //         ImGui::BeginChild("##ScrollingInheritObjects", ImVec2(width / 2.f, 0), ImGuiChildFlags_AutoResizeY);
    //         for (auto object : savedSet[setKlass])
    //         {
    //             char buff[64];
    //             sprintf(buff, "%s [%p]", setKlass->getName(), object);
    //             if (ImGui::Button(buff))
    //             {
    //                 onSelect(object);
    //             }
    //         }
    //         ImGui::EndChild();
    //     }
    // }
    // ImGui::Separator();

    {
        // 类名长度不受控（混淆过的 il2cpp 元数据可以很长，中文名又是
        // UTF-8 三倍字节），固定 128 字节 + 无界 sprintf 就是栈溢出。
        // snprintf 截断即可 —— 这里只是给个折叠标题，截短不影响功能。
        char buffer[256];
        snprintf(buffer, sizeof(buffer), "Inherited from %s",
                 klass->getName() ? klass->getName() : "?");
        if (ImGui::CollapsingHeader(buffer))
        {
            ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
            ImGui::BeginChild("##ScrollingInheritedObjects", ImVec2(width / 1.4f, 0), ImGuiChildFlags_AutoResizeY);
            {
                // {
                //     char buffer[128];
                //     sprintf(buffer, "Inherited from %s", klass->getName());
                //     ImGui::SeparatorText(buffer);
                // }
                bool empty = true;
                for (auto &[setKlass, _] : savedSet)
                {
                    if (klass != setKlass && Il2cpp::IsClassParentOf(setKlass, klass))
                    {
                        auto &objects = savedSet[setKlass];
                        for (auto it = objects.begin(); it != objects.end();)
                        {
                            empty = false;
                            // 经句柄确认对象还在。直接解引用集合里的裸指针是危险的：
                            // 对象一旦被回收，那块内存可能已被复用，->klass 读到的
                            // 是别人的东西或随机值。
                            auto object = ResolveSaved(*it);
                            if (object == nullptr || object->klass == nullptr)
                            {
                                UnsaveObjectWithRoot(*it);
                                it = objects.erase(it);
                                continue;
                            }
                            char buff[256];
                            FormatObjectButton(buff, setKlass->getName(), object);
                            auto size = ImGui::GetWindowSize();
                            if (ImGui::Button(buff, ImVec2(size.x / 1.5, 0)))
                            {
                                onSelect(object);
                            }
                            ImGui::SetItemTooltip("%s", object->klass->getName() ? object->klass->getName() : "?");
                            ImGui::SameLine();
                            ImGui::PushID(buff);
                            if (ImGui::Button("Remove"))
                            {
                                UnsaveObjectWithRoot(object);
                                it = objects.erase(it);
                            }
                            else
                            {
                                ++it;
                            }
                            ImGui::PopID();
                        }
                    }
                }

                if (empty)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
                    ImGui::Text("Nothing...");
                    ImGui::PopStyleColor();
                }
            }
            ImGui::EndChild();
        }
    }
    // ImGui::Text("Saved objects");
    // if (savedSet[klass].empty())
    // {
    //     ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
    //     ImGui::Text("No objects");
    //     ImGui::PopStyleColor();
    // }
    // else
    // {
    //     ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
    //     ImGui::BeginChild("##ScrollingSavedObjects", ImVec2(width / 2.f, 0), ImGuiChildFlags_AutoResizeY);
    //     for (auto object : savedSet[klass])
    //     {
    //         char buff[64];
    //         sprintf(buff, "%s [%p]", klass->getName(), object);
    //         if (ImGui::Button(buff))
    //         {
    //             onSelect(object);
    //         }
    //     }
    //     ImGui::EndChild();
    // }
    // ImGui::Separator();

    {
        if (ImGui::CollapsingHeader("Saved Objects"))
        {
            auto &objects = savedSet[klass];
            ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
            ImGui::BeginChild("##ScrollingSavedObjects", ImVec2(width / 1.4f, 0), ImGuiChildFlags_AutoResizeY);
            {
                // ImGui::SeparatorText("Saved Objects");
                if (objects.empty())
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
                    ImGui::Text("Nothing...");
                    ImGui::PopStyleColor();
                }
                else
                {
                    for (auto it = objects.begin(); it != objects.end();)
                    {
                        // 经句柄确认对象还在。直接解引用集合里的裸指针是危险的：
                        // 对象一旦被回收，那块内存可能已被复用，->klass 读到的
                        // 是别人的东西或随机值。
                        auto object = ResolveSaved(*it);
                        if (object == nullptr || object->klass == nullptr)
                        {
                            UnsaveObjectWithRoot(*it);
                            it = objects.erase(it);
                            continue;
                        }
                        char buff[256];
                        FormatObjectButton(buff, klass->getName(), object);
                        auto size = ImGui::GetWindowSize();
                        if (ImGui::Button(buff, ImVec2(size.x / 1.5, 0)))
                        {
                            onSelect(object);
                        }
                        ImGui::SetItemTooltip("%s", object->klass->getName() ? object->klass->getName() : "?");
                        ImGui::SameLine();
                        ImGui::PushID(buff);
                        if (ImGui::Button("Remove"))
                        {
                            UnsaveObjectWithRoot(object);
                            it = objects.erase(it);
                        }
                        else
                        {
                            ++it;
                        }
                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndChild();
        }
    }
    // ImGui::Text("Collected objects");
    // if (HookerData::collectSet.find(klass) == HookerData::collectSet.end())
    // {
    //     ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
    //     ImGui::Text("No objects");
    //     ImGui::PopStyleColor();
    // }
    // else
    // {
    //     ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
    //     ImGui::BeginChild("##ScrollingCollectedObjects", ImVec2(width / 2.f, 0), ImGuiChildFlags_AutoResizeY);
    //     for (auto object : HookerData::collectSet[klass])
    //     {
    //         char buff[64];
    //         sprintf(buff, "%s [%p]", klass->getName(), object);
    //         if (ImGui::Button(buff))
    //         {
    //             onSelect(object);
    //         }
    //     }
    //     ImGui::EndChild();
    // }
    // ImGui::Separator();

    {
        if (ImGui::CollapsingHeader("Collected Objects"))
        {
            auto &objects = HookerData::collectSet[klass];
            ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
            ImGui::BeginChild("##ScrollingCollectedObjects", ImVec2(width / 1.4f, 0), ImGuiChildFlags_AutoResizeY);
            {
                // ImGui::SeparatorText("Collected Objects");
                if (objects.empty())
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
                    ImGui::Text("Nothing...");
                    ImGui::PopStyleColor();
                }
                else
                {
                    for (auto it = objects.begin(); it != objects.end();)
                    {
                        // 经句柄确认对象还在。直接解引用集合里的裸指针是危险的：
                        // 对象一旦被回收，那块内存可能已被复用，->klass 读到的
                        // 是别人的东西或随机值。
                        auto object = ResolveSaved(*it);
                        if (object == nullptr || object->klass == nullptr)
                        {
                            UnsaveObjectWithRoot(*it);
                            it = objects.erase(it);
                            continue;
                        }
                        char buff[256];
                        FormatObjectButton(buff, klass->getName(), object);
                        auto size = ImGui::GetWindowSize();
                        if (ImGui::Button(buff, ImVec2(size.x / 1.5, 0)))
                        {
                            onSelect(object);
                        }
                        ImGui::SetItemTooltip("%s", object->klass->getName() ? object->klass->getName() : "?");
                        ImGui::SameLine();
                        ImGui::PushID(buff);
                        if (ImGui::Button("Remove"))
                        {
                            UnsaveObjectWithRoot(object);
                            it = objects.erase(it);
                        }
                        else
                        {
                            ++it;
                        }
                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndChild();
        }
    }
    // if (canNew)
    // {
    //     if (ImGui::Button("New"))
    //     {
    //         auto newObject = klass->New();
    //         newObjectMap[klass].push_back(newObject);
    //         if (Il2cpp::GetClassType(klass)->isValueType())
    //         {
    //             newObject = (Il2CppObject *)Il2cpp::GetUnboxedValue(newObject);
    //         }
    //         onSelect(newObject);
    //     }
    // }
    // else
    // {
    //     ImGui::Text("Created objects");
    // }

    // ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
    // ImGui::BeginChild("##ScrollingNewObjects", ImVec2(width / 2.f, 0), ImGuiChildFlags_AutoResizeY);
    // for (auto createObject : newObjectMap[klass])
    // {
    //     char buff[64];
    //     sprintf(buff, "%s [%p]", prefix, createObject);
    //     if (ImGui::Button(buff))
    //     {
    //         onSelect(createObject);
    //     }
    // }
    // ImGui::EndChild();

    {
        if (ImGui::CollapsingHeader("Created Objects"))
        {
            auto &objects = newObjectMap[klass];
            ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
            ImGui::BeginChild("##ScrollingNewObjects", ImVec2(width / 1.4f, 0), ImGuiChildFlags_AutoResizeY);
            {
                // ImGui::SeparatorText("Created Objects");
                if (objects.empty())
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
                    ImGui::Text("Nothing...");
                    ImGui::PopStyleColor();
                }
                else
                {
                    auto live = liveOf(objects);
                    for (size_t i = 0; i < live.size();)
                    {
                        auto object = live[i];
                        if (object == nullptr || object->klass == nullptr)
                        {
                            ++i;
                            continue;
                        }
                        char buff[256];
                        FormatObjectButton(buff, prefix, object);
                        auto size = ImGui::GetWindowSize();
                        if (ImGui::Button(buff, ImVec2(size.x / 1.5, 0)))
                        {
                            onSelect(object);
                        }
                        ImGui::SetItemTooltip("%s", object->klass->getName() ? object->klass->getName() : "?");
                        ImGui::SameLine();
                        ImGui::PushID(buff);
                        if (ImGui::Button("Remove"))
                        {
                            const auto &raw = objects.raw();
                            for (size_t k = 0; k < raw.size(); k++)
                            {
                                if (raw[k] == object)
                                {
                                    objects.removeAt(k);
                                    break;
                                }
                            }
                            live.erase(live.begin() + i);
                        }
                        else
                        {
                            ++i;
                        }
                        ImGui::PopID();
                    }
                }
                if (canNew)
                {
                    if (ImGui::Button("New"))
                    {
                        auto newObject = klass->New();
                        if (newObject == nullptr)
                        {
                            LOGE("klass->New() 返回空（抽象类/无默认构造?）");
                        }
                        else
                        {
                            newObjectMap[klass].add(newObject);
                            // 注意：这里必须传**装箱后的对象**。
                            // 旧代码对值类型把 newObject 换成 GetUnboxedValue 的
                            // 载荷指针再交给 onSelect，而 onSelect 之后会读
                            // obj->klass / 调方法 —— 载荷指针根本不是 Il2CppObject，
                            // 那是把别处的内存当对象头解引用。
                            // 值类型的字段编辑路径会自己走 ensureIfValueType 拆箱。
                            onSelect(newObject);
                        }
                    }
                }
            }
            ImGui::EndChild();
        }
    }
    // if (!objectMap.empty())
    // {
    //     ImGui::Separator();
    //     static Il2CppObject *selected = nullptr;
    //     if (ImGui::Button("Force Select Existing"))
    //     {
    //         ImGui::OpenPopup("ForceObjectSelector");
    //         selected = nullptr;
    //     }
    //     if (ImGui::BeginPopup("ForceObjectSelector"))
    //     {
    //         for (auto it = objectMap.begin(); it != objectMap.end() && selected == nullptr; it++)
    //         {
    //             auto [objKlass, objects] = *it;
    //             char klassName[256]{0};
    //             for (auto object : objects)
    //             {
    //                 sprintf(klassName, "%s [%p]", objKlass->getName(), object);
    //                 if (ImGui::Button(klassName))
    //                 {
    //                     selected = object;
    //                     break;
    //                 }
    //             }
    //         }
    //         ImGui::EndPopup();
    //     }
    //     if (selected)
    //     {
    //         onSelect(selected);
    //         selected = nullptr;
    //     }
    // }
}

// 参数在预设里的稳定键：参数名 + 序号。
//
// **不能用 paramMap 的 key** —— 那个 key 形如 "%p%s%d"，嵌了 MethodInfo*
// （进程内的代码地址，每次启动都不同）。用它当预设的键，预设在第二次
// 启动后就再也匹配不上任何参数：用户选了预设、界面毫无反应、也不报错。
// 而它在单次会话内完全正常，所以这个 bug 极难被发现。
//
// 方法本身已经由 MethodSignature 定位了，所以「参数名 + 序号」在
// 方法内部是唯一的。
static std::string StableParamKey(const char *paramName, int index)
{
    char buf[224]{0};
    snprintf(buf, sizeof(buf), "%s#%d", paramName ? paramName : "?", index);
    return buf;
}

// 起一个后台线程，返回是否成功。
//
// **std::thread 的构造函数在线程创建失败时会抛 std::system_error**
// （线程/栈资源耗尽）。裸写就是 std::terminate → 整个游戏进程崩掉，
// 而且这个工具恰好最容易触发：它在游戏进程里，游戏自己已经吃掉了一堆
// 线程，每次勾选 Hook 还会再起一个。
//
// 抛出还有第二重危害：异常会穿过 ImGui 的 Begin/End，栈失配 → 下一帧
// IM_ASSERT → __builtin_trap() → 无声的 SIGILL。
//
// 失败时调用 onFail（如果有的话）把「进行中」这类状态复位 —— 否则
// 界面上会永远显示「处理中…」。
// （定义放在 ClassesTab.cpp 前部：对象扫描和 Hook 都要用。）



// 上一次字段写入失败的原因。空串 = 没有待显示的错误。
//
// 键盘回调是在**之后的某一帧**才被调用的，和绘制路径不在同一帧，
// 所以不能在里面直接 ImGui::Text —— 那时 ImGui 帧上下文早就不在了。
// 用文件级变量传回绘制路径显示。
static std::string g_fieldError;
static double g_fieldErrorAt = 0.0;

static void ReportFieldError(const std::string &msg)
{
    g_fieldError = msg;
    g_fieldErrorAt = std::chrono::duration<double>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    LOGW("字段写入失败: %s", msg.c_str());
}

// 显示上面那条错误。由 Tool::Draw 调用（见那里的说明），
// **不能**放在 ImGuiJson 里 —— 那儿是「有打开的对象 tab」才会走的路径。
//
// 之前挂在 ImGuiJson 里有两个后果，都比「看不见」更糟：
//
// 一、参数解析错误（第 46 轮加的）发生在 CallerView 的页签里，
//    而 CallerView 和 ImGuiJson 是**两个完全不同的界面**。
//    结果是「参数 health 无法解析」这句话出现在对象检视器里 ——
//    用户在一个界面操作，却在另一个界面看到报错，还以为是陈旧残留。
//
// 二、过期清除也挂在同一个 if 里。没有对象 tab 打开时**永远不清**，
//    于是一分钟前的错误会一直留着，等你下次随便打开一个对象检视器
//    才突然冒出来，指着一个你根本没在看的字段。
//
// 所以：设置和显示必须放在**同一个、一直都在**的地方。
void ClassesTab::DrawFieldErrorBanner()
{
    if (g_fieldError.empty())
    {
        return;
    }
    const double now = std::chrono::duration<double>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    if (now - g_fieldErrorAt > 8.0)
    {
        g_fieldError.clear();
        return;
    }
    // 一直显示到过期为止（而不是只显示剩下的秒数）—— 秒数每帧都在变，
    // 数字跳动比不显示更让人分心。
    ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "⚠ 上一次写入失败：%s", g_fieldError.c_str());
}

void ClassesTab::CallerView(Il2CppClass *klass, MethodInfo *method, const MethodParamList &paramsInfo,
                            Il2CppObject *thiz)
{
    static ImGuiIO &io = ImGui::GetIO();
    bool methodIsStatic = Il2cpp::GetIsMethodStatic(method);
    if (!paramMap) paramMap = std::make_shared<decltype(paramMap)::element_type>();
    auto &params = (*paramMap)[method];
    if (!methodIsStatic && !thiz)
    {
        auto &thisParam = params["this"];
        // 类名长度不受控，param.value 又是用户输入；固定 128 字节缓冲 + 无界 sprintf 会栈溢出。
        // 另外旧写法 sprintf(dst, "%s = %s", dst, ...) 把 dst 同时当源和目标，是未定义行为。
        // getName() 可能是 nullptr —— getFullName() 里有同样的防护，
        // 注释写的是「我确实遇到过 typeName 为空的情况」。
        // 旧代码 `std::string(GetClassType(klass)->getName())` 两层都裸解引用：
        // GetClassType 为空 → 空指针；getName() 为空 → std::string(nullptr)，UB。
        const char *klassName = "?";
        auto *thisType = Il2cpp::GetClassType(klass);
        if (thisType != nullptr && thisType->getName() != nullptr)
        {
            klassName = thisType->getName();
        }
        std::string thisLabel = std::string(klassName) + " this";
        if (!thisParam.value.empty())
        {
            thisLabel += " = " + thisParam.value;
        }
        if (ImGui::Button(thisLabel.c_str()))
        {
            ImGui::OpenPopup("ThisObjectSelector");
        }
        if (ImGui::BeginPopup("ThisObjectSelector"))
        {
            // 同上：不捕获 &thisParam，改用 shared_ptr + key 重新定位。
            auto paramMapRef = paramMap;
            auto paramMethod = method;
            ImGuiObjectSelector(
                ImGui::GetID("ThisObjectSelector"), klass, "this",
                [paramMapRef, paramMethod](Il2CppObject *object)
                {
                    auto it = paramMapRef->find(paramMethod);
                    if (it == paramMapRef->end()) return;
                    auto pit = it->second.find("this");
                    if (pit == it->second.end()) return;
                    auto &thisParam = pit->second;

                    // 64 位下 "%p" 要 "0x" + 16 位十六进制 + '\0' = 19 字节，
                    // 旧的 char[16] 必然溢出 3 字节。
                    char objStr[32]{0};
                    snprintf(objStr, sizeof(objStr), "%p", (const void *)object);
                    thisParam.value = objStr;
                    thisParam.object = object;
                    // 参数对象要活到用户按下「调用」，中间可能隔几帧，
                    // 期间随时会被 GC 回收 → arrayParams[k] 变成野指针。
                    if (object)
                    {
                        SaveObjectWithRoot(object);
                    }
                    ImGui::CloseCurrentPopup();
                },
                strcmp(method->getName(), ".ctor") == 0);
            ImGui::EndPopup();
        }
    }
    for (int k = 0; k < paramsInfo.size(); k++)
    {
        auto &[name, type] = paramsInfo[k];

        // paramKey 是 params 的键，必须完整唯一：方法地址 + 参数名 + 序号
        char paramKey[256]{0};
        snprintf(paramKey, sizeof(paramKey), "%p%s%d", (const void *)method, name, k);
        auto &param = params[paramKey];

        std::string buttonLabel = std::string(type->getName()) + " " + name;
        if (!param.value.empty())
        {
            buttonLabel += " = " + param.value;
        }
        ImGui::PushID(k);
        if (ImGui::Button(buttonLabel.c_str()))
        {
            bool isString = strcmp(type->getName(), "System.String") == 0;
            if (type->isPrimitive() || isString)
            {
                if (strcmp(type->getName(), "System.Boolean") == 0)
                {
                    // ImGui::OpenPopup("BooleanSelector");
                    // 回调**按值捕获 paramMap 的 shared_ptr + 键**，而不是
                    // 捕获 `&param`。
                    //
                    // 之前把 paramMap 改成 shared_ptr 是为了「tab 销毁后回调
                    // 仍能安全写入」—— 但那只在**回调持有那份 shared_ptr**
                    // 时才成立。而 lambda 捕获的是 `&param`（指向 map 节点里的
                    // 引用），shared_ptr 本身还是 tab 的成员：tab 一析构，
                    // refcount 归零，整个 map 连同节点一起被释放，回调再写
                    // 就是写已释放内存。也就是说上一版的修复**根本没生效**。
                    //
                    // 现在回调自己持有一份 shared_ptr，并在触发时按 key
                    // 重新定位 —— 数据一定还活着。
                    auto paramMapRef = paramMap;
                    auto paramMethod = method;
                    auto paramKeyStr = std::string(paramKey);
                    poper.Open("BooleanSelector", [paramMapRef, paramMethod, paramKeyStr](const std::string &result)
                               {
                                   auto it = paramMapRef->find(paramMethod);
                                   if (it == paramMapRef->end()) return;
                                   auto pit = it->second.find(paramKeyStr);
                                   if (pit == it->second.end()) return;
                                   pit->second.value = result;
                               });
                }
                else
                {
                    // isString 必须**按值**捕获。
                    //
                    // 旧代码是 [&param, &isString] 按引用捕获，但 isString 是本函数
                    // 栈上的局部变量，作用域到函数结束就没了。而这个 lambda 被
                    // Keyboard::Open 存进**全局**的 lastCallback，在**之后的某一帧**
                    // 才被调用 —— 那时栈帧早被复用了。
                    // 如果读到的垃圾恰好是 true，就会给一个非 String 参数塞进
                    // 托管字符串对象，随后在 1044 行把这个指针当作该参数的类型
                    // 传给 VM → 垃圾值或崩溃。
                    const bool isStringParam = isString;
                    auto paramMapRef = paramMap;
                    auto paramMethod = method;
                    auto paramKeyStr = std::string(paramKey);
                    Keyboard::Open(
                        [this, paramMapRef, paramMethod, paramKeyStr, isStringParam](const std::string &text)
                        {
                            auto it = paramMapRef->find(paramMethod);
                            if (it == paramMapRef->end())
                            {
                                return;
                            }
                            auto pit = it->second.find(paramKeyStr);
                            if (pit == it->second.end())
                            {
                                return;
                            }
                            auto &param = pit->second;
                            if (isStringParam)
                            {
                                param.object = Il2cpp::NewString(text.c_str());
                                // 这个托管字符串要一直活到用户按下「调用」为止 ——
                                // 中间隔了几帧，随时可能被 GC 回收。届时
                                // arrayParams[k] 里就是个野指针，被当作该参数的类型
                                // 交给 VM。加根保活。
                                if (param.object)
                                {
                                    SaveObjectWithRoot(param.object);
                                }
                            }
                            param.value = text;
                        });
                }
            }
            else if (type->isEnum())
            {
                // 同上：持有 shared_ptr + 按 key 重新定位，而不是捕获 &param。
                auto paramMapRef = paramMap;
                auto paramMethod = method;
                auto paramKeyStr = std::string(paramKey);
                poper.Open(
                    "EnumSelector",
                    [paramMapRef, paramMethod, paramKeyStr](const std::string &result)
                    {
                        auto it = paramMapRef->find(paramMethod);
                        if (it == paramMapRef->end()) return;
                        auto pit = it->second.find(paramKeyStr);
                        if (pit == it->second.end()) return;
                        pit->second.value = result;
                    },
                    type);
            }
            // else if (!type->isValueType() && !(type->isArray() || type->isList()
            // ||
            //                                    strstr(type->getName(),
            //                                    "System.Object")))
            // else if (!strstr(type->getName(), "System.Object"))
            else
            {
                ImGui::OpenPopup("ParamObjectSelector");
            }
        }
        if (ImGui::BeginPopup("ParamObjectSelector"))
        {
            // 同样按 key 重新定位，不捕获 &param。理由见上面 BooleanSelector
            // 那处的注释（共享出来的 map 活得比回调久，&param 不行）。
            auto paramMapRef = paramMap;
            auto paramMethod = method;
            auto paramKeyStr = std::string(paramKey);
            ImGuiObjectSelector(ImGui::GetID("ParamObjectSelector"), type->getClass(), name,
                                [paramMapRef, paramMethod, paramKeyStr](Il2CppObject *object)
                                {
                                    auto it = paramMapRef->find(paramMethod);
                                    if (it == paramMapRef->end()) return;
                                    auto pit = it->second.find(paramKeyStr);
                                    if (pit == it->second.end()) return;
                                    auto &param = pit->second;

                                    // 同上：64 位 %p 需要 19 字节，char[16] 必溢出
                                    char objStr[32]{0};
                                    snprintf(objStr, sizeof(objStr), "%p", (const void *)object);
                                    param.value = objStr;
                                    param.object = object;
                                    // 参数对象要活到用户按下「调用」，中间可能隔几帧，
                                    // 期间随时会被 GC 回收 → arrayParams[k] 变成野指针。
                                    if (object)
                                    {
                                        SaveObjectWithRoot(object);
                                    }
                                    ImGui::CloseCurrentPopup();
                                });
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(30, 200, 25, 128));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(30, 200, 25, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(30, 200, 25, 255));
    if (ImGui::Button("Call", ImVec2(io.DisplaySize.x / 2, 0)))
    {
        auto paramsInfo = method->getParamsInfo();
        // paramMap 理论上一定非空（CallerView 进来时就会创建），但这里仍然兜一下：
        // 缺失时给一份空的，让下面的查找全部落空 → parseFailed，而不是野指针。
        std::unordered_map<std::string, ParamValue> emptyParams;
        auto params = paramMap ? (*paramMap)[method] : emptyParams;
        // 必须值初始化：new T[n] 是默认初始化（不填零）。只要有任何一个参数
        // 没走到赋值分支，arrayParams[k] 就是野指针，随后被交给
        // il2cpp_runtime_invoke 去解引用。
        auto arrayParams =
            (paramsInfo.size() > 0) ? new Il2CppObject *[paramsInfo.size()]() : nullptr;

        bool hasParams = true;
        // 任一参数解析/构造失败就整体放弃这次调用：
        // 与其把不确定的参数数组丢给 VM，不如直接不调用。
        bool parseFailed = false;
        Il2CppObject *thisParam = nullptr;
        if (!methodIsStatic && !thiz)
        {
            if (params["this"].value.empty())
            {
                hasParams = false;
            }
            else
            {
                thisParam = params["this"].object;
                LOGD("this = %s", params["this"].value.c_str());
            }
        }
        else if (thiz)
        {
            thisParam = thiz;
        }

        for (int k = 0; k < paramsInfo.size(); k++)
        {
            auto &[name, type] = paramsInfo[k];

            char paramKey[256]{0};
            snprintf(paramKey, sizeof(paramKey), "%p%s%d", (const void *)method, name, k);
            auto &param = params[paramKey];
            LOGD("%s %s = %s", type->getName(), name, param.value.c_str());
            if (!param.value.empty())
            {
                // 参数值是用户通过软键盘输进来的，std::stoi/stol/... 在遇到
                // 非法文本或越界时会抛 std::invalid_argument / out_of_range。
                // 这里在 Call 按钮的回调栈上，一旦抛出就会冲出 ImGui 渲染、
                // 冲出 eglSwapBuffers 钩子 —— 整个游戏进程被 terminate。
                // 统一走带保护的解析，失败就跳过该参数并提示。
                auto parse = [&](auto tag) {
                    using T = decltype(tag);
                    T raw{};
                    try
                    {
                        // **必须整串吃掉**。std::stoll / stod 接受部分输入：
                        // std::stoll("12abc") 静默返回 12，std::stod("1.5e")
                        // 静默返回 1.5。也就是用户敲错一个字符，工具就把一个
                        // **不同的值**当成他的输入传给游戏 —— 界面上完全看不出异常。
                        //
                        // 上一轮在字段编辑器那边加了同样的检查，这里是
                        // 同一条路径的另一半：之前只修了一处。
                        const std::string &s = param.value;
                        size_t pos = 0;
                        if constexpr (std::is_same_v<T, int>)
                        {
                            raw = static_cast<int>(std::stol(s, &pos, 10));
                        }
                        else if constexpr (std::is_same_v<T, int64_t>)
                        {
                            raw = static_cast<int64_t>(std::stoll(s, &pos, 10));
                        }
                        else if constexpr (std::is_same_v<T, uint32_t>)
                        {
                            raw = static_cast<uint32_t>(std::stoul(s, &pos, 10));
                        }
                        else if constexpr (std::is_same_v<T, uint64_t>)
                        {
                            raw = static_cast<uint64_t>(std::stoull(s, &pos, 10));
                        }
                        else if constexpr (std::is_same_v<T, float>)
                        {
                            raw = std::stof(s, &pos);
                        }
                        else
                        {
                            raw = std::stod(s, &pos);
                        }
                        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos])))
                        {
                            ++pos;
                        }
                        if (pos != s.size())
                        {
                            LOGE("参数 %s 的值 \"%s\" 含有无法解析的尾部字符", name, s.c_str());
                            parseFailed = true;
                            ReportFieldError(std::string(name) + "：\"" + s + "\" 不是合法的数值");
                            return;
                        }
                    }
                    catch (const std::exception &e)
                    {
                        LOGE("参数 %s 的值 \"%s\" 无法解析为数值: %s", name, param.value.c_str(), e.what());
                        parseFailed = true;
                        ReportFieldError(std::string(name) + "：\"" + param.value + "\" 无法解析为数值");
                        return;
                    }
                    auto *klass = type->getClass();
                    if (!klass)
                    {
                        LOGE("参数 %s 的类型 %s 解析不到 Il2CppClass", name, type->getName());
                        parseFailed = true;
                        return;
                    }
                    ValueType<T> value{raw};
                    arrayParams[k] = value.box(klass);
                };

                if (strcmp(type->getName(), "System.Int32") == 0)
                    parse(int{});
                else if (strcmp(type->getName(), "System.Int64") == 0)
                    parse(int64_t{});
                else if (strcmp(type->getName(), "System.UInt32") == 0)
                    parse(uint32_t{});
                else if (strcmp(type->getName(), "System.UInt64") == 0)
                    parse(uint64_t{});
                else if (strcmp(type->getName(), "System.Single") == 0)
                    parse(float{});
                else if (strcmp(type->getName(), "System.Double") == 0)
                    parse(double{});
                else if (strcmp(type->getName(), "System.Boolean") == 0)
                {
                    // using true/false sometimes causing crash for me, don't know why
                    ValueType<int> value{param.value == "True" ? 1 : 0};
                    if (auto *klass = type->getClass())
                    {
                        arrayParams[k] = value.box(klass);
                    }
                    else
                    {
                        parseFailed = true;
                    }
                }
                else if (type->isEnum())
                {
                    // 枚举按名字取静态常量。getField 可能返回空（字段被重命名/裁剪），
                    // 旧代码直接 ->getStaticValue 就是空指针解引用。
                    auto *enumClass = type->getClass();
                    auto *field = enumClass ? enumClass->getField(param.value.c_str()) : nullptr;
                    if (field && enumClass)
                    {
                        // 按枚举真实底层宽度取值。底层可能是 long/ulong（8 字节），
                        // 旧代码固定用 int 承接，运行时就会往 4 字节变量上写 8 字节。
                        auto raw = FieldInfo::getEnumStaticValue(field);

                        // 再按底层宽度装箱：box() 读 sizeof(T) 个字节，
                        // 宽度对不上会把栈上的临时变量读过头。
                        auto *baseType = Il2cpp::GetEnumBaseType(enumClass);
                        const char *baseName = baseType ? Il2cpp::GetTypeName(baseType) : nullptr;
                        if (baseName && (strcmp(baseName, "System.Int64") == 0 ||
                                         strcmp(baseName, "System.UInt64") == 0))
                        {
                            int64_t wide = raw;
                            arrayParams[k] = ValueType<int64_t>{wide}.box(enumClass);
                        }
                        else
                        {
                            int32_t narrow = static_cast<int32_t>(raw);
                            arrayParams[k] = ValueType<int32_t>{narrow}.box(enumClass);
                        }
                    }
                    else
                    {
                        LOGE("枚举 %s 找不到成员 %s", type->getName(), param.value.c_str());
                        parseFailed = true;
                    }
                }
                else if (strcmp(type->getName(), "System.String") == 0)
                {
                    arrayParams[k] = Il2cpp::NewString(param.value.c_str());
                }
                else if (param.object)
                {
                    arrayParams[k] = param.object;
                }
                else
                {
                    // 旧代码只打一行日志就继续，arrayParams[k] 保持未初始化，
                    // 然后照样把这个野指针交给 il2cpp_runtime_invoke。
                    LOGE("参数 %s 的类型 %s 暂不支持，无法传值", name, type->getName());
                    parseFailed = true;
                }
            }
            else
            {
                hasParams = false;
            }
            if (parseFailed)
            {
                // 宁可拒绝这次调用，也不要把不确定的参数丢给 VM
                hasParams = false;
            }
        }
        if (hasParams)
        {
            Il2CppObject *result = nullptr;
            // GetClassType 可能返回空（元数据被裁剪时），GetName() 也可能为空。
            auto *thisClassType = (thisParam != nullptr) ? Il2cpp::GetClassType(thisParam->klass) : nullptr;
            const bool thisIsValueType =
                thisClassType != nullptr && thisClassType->isValueType();
            const bool isCtor = method->getName() == nullptr ||
                                strcmp(method->getName(), ".ctor") == 0;
            // 显式接住托管异常：抛异常时 il2cpp 会同时把结果置 null 并
            // 通过出参回填异常对象。不接的话，「抛异常」和「合法返回 null」
            // 在界面上长得一模一样 —— 而这个面板的卖点就是「返回值 / 异常」。
            Il2CppException *managedException = nullptr;
            if (!isCtor && thisParam && thisIsValueType)
            {
                auto thizz = Il2cpp::GetUnboxedValue(thisParam);
                result = Il2cpp::RuntimeInvokeConvertArgs(method, thizz, arrayParams,
                                                          paramsInfo.size(), &managedException);
            }
            else
            {
                result = Il2cpp::RuntimeInvokeConvertArgs(method, thisParam, arrayParams,
                                                          paramsInfo.size(), &managedException);
            }
            LOGPTR(result);
            if (managedException)
            {
                // 异常对象本身就是 Il2CppObject；to_string() 取它自己的
                // message 字段通常拿不到有意义的内容，但至少把类型和
                // 对象地址告诉用户 —— 比谎称「返回 null」强。
                Il2CppObject *excObj = reinterpret_cast<Il2CppObject *>(managedException);
                char excText[160]{0};
                const char *excName = excObj->klass ? excObj->klass->getName() : nullptr;
                snprintf(excText, sizeof(excText), "抛出异常: %s (%p)", excName ? excName : "?",
                         static_cast<void *>(excObj));
                callResults.at(method).push_back({excText, nullptr});
                LOGE("调用抛出托管异常: %s", excText);
            }
            else if (result && method->getName() && strcmp(method->getName(), ".ctor") != 0)
            {
                auto resultType = Il2cpp::GetClassType(result->klass);
                if (resultType == nullptr)
                {
                    char typeText[96]{0};
                    snprintf(typeText, sizeof(typeText), "返回值类型信息缺失 (%p)",
                             static_cast<void *>(result));
                    callResults.at(method).push_back({typeText, result});
                }
                else if (resultType->isPrimitive())
                {
                    std::vector<uintptr_t> visited;
                    auto j = result->dump(visited, 1);
                    // 装箱后的基础类型没有字段时 dump() 返回字符串 "(no-fields)"，
                    // 那时 j 不是容器，j.begin() == j.end()，再 .value() 就是解引用
                    // end() —— UB。显式判一下。
                    std::string primitiveText;
                    if (j.is_object() && !j.empty())
                    {
                        primitiveText = j.begin().value().dump();
                    }
                    else
                    {
                        primitiveText = j.is_string() ? j.get<std::string>() : std::string("(无字段)");
                    }
                    callResults.at(method).push_back(std::pair{primitiveText, nullptr});
                }
                else if (strcmp(resultType->getName(), "System.String") == 0)
                {
                    callResults.at(method).push_back({((Il2CppString *)result)->to_string(), nullptr});
                }
                else if (resultType->isEnum())
                {
                    callResults.at(method).push_back(
                        {result->invoke_method<Il2CppString *>("ToString")->to_string(), nullptr});
                }
                else
                {
                    if (resultType->isValueType())
                    {
                        Il2cpp::GC::KeepAlive(result); // does this actually work?
                    }
                    auto toString = result->klass->getMethod("ToString", 0);
                    if (toString)
                    {
                        Il2CppString *str = nullptr;
                        if (resultType->isValueType())
                        {
                            auto thizz = Il2cpp::GetUnboxedValue(result);
                            // str = toString->invoke_static<Il2CppString *>(thizz);
                            str = (Il2CppString *)Il2cpp::RuntimeInvokeConvertArgs(toString, thizz, nullptr, 0);
                        }
                        else
                        {
                            str = toString->invoke_static<Il2CppString *>(result);
                        }
                        if (str)
                        {
                            callResults.at(method).push_back({str->to_string(), result});
                        }
                        else
                        {
                            // 调用本身成功了（没有异常），只是 ToString() 没给出字符串。
                            // 措辞要和上面那个「调用抛异常」区分开。
                            callResults.at(method).push_back({"调用成功，但 ToString() 返回 null", result});
                        }
                    }
                    else
                    {
                        // 64 位 "%p" 需要 19 字节，char[16] 必然溢出
                        char resultStr[32]{0};
                        snprintf(resultStr, sizeof(resultStr), "%p", (const void *)result);
                        callResults.at(method).push_back({resultStr, result});
                    }
                }
                // savedSet 里没有 GCHandle 的条目，ResolveSaved 会**原样返回裸指针**
                // （见 ResolveSaved 的实现），于是持有者一旦被 GC 回收，
                // 后面 `object->klass` 就是解引用野指针。
                //
                // JSON 检视器那条路径一直是对的（SaveObjectWithRoot），这里
                // 却是裸 insert —— 调用结果绕过了整套 GC 保活机制。
                savedSet[resultType->getClass()].insert(result);
                SaveObjectWithRoot(result);
                // setJsonObject(result);
            }
            else if (!managedException)
            {
                // 走到这里说明：没有异常，result 就是真的 null。
                // （抛异常的情况已经在上面单独报过了。）
                callResults.at(method).push_back({"调用成功，返回 null", nullptr});
            }
        }
        else
        {
            LOGE("Not all params are set!");
        }
        if (arrayParams)
            delete[] arrayParams;
    }
    ImGui::PopStyleColor(3);

    // -----------------------------------------------------------------------
    // 参数预设
    //
    // 反复调同一个方法、只改一两个值时，用软键盘重敲一遍非常折磨。
    // 保存一次、之后一键载入。
    //
    // 只存**文本**。引用类型参数（对象）不存 —— 预设可能是几小时后才载入的，
    // 那个对象早被 GC 回收了，复原陈旧指针就是 use-after-free。
    // 载入时 object 一律清空，引用参数需要重新选一次（UI 上明确告知）。
    // -----------------------------------------------------------------------
    {
        auto &presets = methodPresets[method];
        auto &currentName = selectedPreset[method];

        ImGui::Separator();
        ImGui::TextUnformatted("参数预设");

        // 载入
        if (presets.empty())
        {
            ImGui::TextDisabled("还没有预设。填好参数后点「存为预设」即可。");
        }
        else
        {
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##preset", currentName.empty() ? "(未选择)" : currentName.c_str()))
            {
                for (const auto &p : presets)
                {
                    bool selected = (p.name == currentName);
                    // 预设名是用户输入，同样要防 "##" 破坏控件 ID。
                    std::string safeName;
                    safeName.reserve(p.name.size());
                    for (char c : p.name)
                    {
                        safeName.push_back(c == '#' ? '_' : c);
                    }
                    if (ImGui::Selectable((safeName + "##presetitem").c_str(), selected))
                    {
                        currentName = p.name;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("载入##presetload") && !currentName.empty())
            {
                const MethodPreset *found = nullptr;
                for (const auto &p : presets)
                {
                    if (p.name == currentName)
                    {
                        found = &p;
                        break;
                    }
                }
                if (found && paramMap)
                {
                    auto &target = (*paramMap)[method];
                    size_t applied = 0;
                    // 按稳定键（参数名 + 序号）回填，并换算成当前会话的
                    // paramMap key。详见「存为预设」处关于为什么不能用
                    // paramMap key 当预设键的说明。
                    for (int k = 0; k < paramsInfo.size(); k++)
                    {
                        const auto &[pname, ptype] = paramsInfo[k];
                        auto vIt = found->values.find(StableParamKey(pname, k));
                        if (vIt == found->values.end())
                        {
                            continue;
                        }
                        char liveKey[256]{0};
                        snprintf(liveKey, sizeof(liveKey), "%p%s%d", (const void *)method, pname, k);
                        auto &pv = target[liveKey];
                        pv.value = vIt->second;
                        // 关键：清掉 object。预设里根本没存它（可能早就
                        // 被 GC 回收了），留着旧的只会把野指针交给 VM。
                        pv.object = nullptr;
                        applied++;
                    }
                    if (applied == 0)
                    {
                        // 一定要说出来：静默什么都不发生，用户会以为功能坏了。
                        LOGW("预设 \"%s\" 没有匹配上任何参数（该方法签名可能已变）",
                             currentName.c_str());
                        ImGui::TextColored(ImVec4(1.f, 0.85f, 0.4f, 1.f),
                                           "该预设的参数名与当前方法不匹配（游戏版本可能已变），未应用任何值。");
                    }
                    else
                    {
                        LOGI("已载入参数预设 \"%s\"：%zu 个参数（引用类型需重新选择）",
                             currentName.c_str(), applied);
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("删除##presetdel") && !currentName.empty())
            {
                presets.erase(std::remove_if(presets.begin(), presets.end(),
                                             [&currentName](const MethodPreset &p)
                                             { return p.name == currentName; }),
                              presets.end());
                currentName.clear();
            }
        }

        // 新建 / 覆盖
        ImGui::SetNextItemWidth(-1.0f);
        char nameBuf[64]{0};
        // 预设名长度截断 —— 用户输入 + 软键盘，无界输入不能直接进固定缓冲。
        snprintf(nameBuf, sizeof(nameBuf), "%s", newPresetName.c_str());
        if (ImGui::InputTextWithHint("##presetname", "预设名（留空则用方法名）", nameBuf,
                                     sizeof(nameBuf)))
        {
            newPresetName = nameBuf;
        }
        ImGui::SameLine();
        if (ImGui::Button("存为预设##presetsave") && paramMap)
        {
            std::string name = newPresetName.empty() ? std::string(method->getName()) : newPresetName;
            if (name.empty())
            {
                name = "(未命名)";
            }
            MethodPreset preset;
            preset.name = name;
            // 按**稳定键**（参数名 + 序号）存，不按 paramMap 的 key。
            //
            // paramMap 的 key 形如 "%p%s%d"，里面嵌了 MethodInfo* —— 那是
            // 进程内的代码地址，**每次启动都不一样**（ASLR / il2cpp 布局变化）。
            // 用它当预设的键，预设在**第二次启动后就再也匹配不上任何参数**：
            // 用户选了预设、界面毫无反应、也不报错。而它在单次会话内是好的，
            // 所以极难被发现。
            for (int k = 0; k < paramsInfo.size(); k++)
            {
                const auto &[pname, ptype] = paramsInfo[k];
                char liveKey[256]{0};
                snprintf(liveKey, sizeof(liveKey), "%p%s%d", (const void *)method, pname, k);
                auto it = (*paramMap)[method].find(liveKey);
                if (it == (*paramMap)[method].end() || it->second.value.empty())
                {
                    continue;
                }
                // 只存文本。object 一律不存。
                preset.values[StableParamKey(pname, k)] = it->second.value;
            }
            auto &list = methodPresets[method];
            auto existing = std::find_if(list.begin(), list.end(),
                                         [&name](const MethodPreset &p) { return p.name == name; });
            if (existing != list.end())
            {
                *existing = std::move(preset); // 同名覆盖
            }
            else
            {
                list.push_back(std::move(preset));
            }
            selectedPreset[method] = name;
            newPresetName.clear();
            Tool::ConfigSave();
        }
    }
    if (!callResults.at(method).empty())
    {
        ImGui::Separator();
        ImGui::Text("Call Results:");
        for (auto [callResult, object] : callResults.at(method))
        {
            if (object)
            {
                if (ImGui::Button(callResult.c_str()))
                {
                    setJsonObject(object);
                }
            }
            else
            {
                ImGui::Text("%s", callResult.c_str());
            }
            ImGui::Separator();
        }
        // ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(200, 30, 25, 128));
        // ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(200, 30, 25,
        // 255)); ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(200, 30, 25,
        // 255)); if (ImGui::Button("Clear##CallResults",
        //                   ImVec2(ImGui::GetIO().DisplaySize.x / 3.f, 0)))
        // {
        //     callResults.at(method).clear();
        // }
        // ImGui::PopStyleColor(3);
    }
}

bool ClassesTab::isMethodHooked(MethodInfo *method)
{
    // hook 回调会在游戏线程持 hookerMtx 改这张表；无锁查询是数据竞争，
    // 而且 UI 可能正在遍历它（rehash 期间）。
    std::lock_guard guard(hookerMtx);
    return hookerMap.find(method->methodPointer) != hookerMap.end();
}

// 恢复被 patch 过的方法。
// 必须和 Patcher::patch() 走同样的流程：mprotect 成可写 → memcpy → 刷 I-cache →
// 恢复 R+X。旧代码只有裸 memcpy，既没改页保护（只读页上直接写会 SIGSEGV），
// 也没刷 I-cache（CPU 可能还在执行旧字节），等于恢复失败或跑飞。
static bool RestorePatchedMethod(MethodInfo *method, const std::vector<uint8_t> &originalBytes)
{
    if (!method || !method->methodPointer || originalBytes.empty())
    {
        return false;
    }
    auto *target = (void *)method->methodPointer;
    if (!KittyMemory::ProtectAddr(target, originalBytes.size(), PROT_READ | PROT_WRITE | PROT_EXEC))
    {
        LOGE("RestorePatchedMethod: mprotect(RWX) 失败");
        return false;
    }
    memcpy(target, originalBytes.data(), originalBytes.size());
    __builtin___clear_cache((char *)target, (char *)target + originalBytes.size());
    if (!KittyMemory::ProtectAddr(target, originalBytes.size(), PROT_READ | PROT_EXEC))
    {
        LOGE("RestorePatchedMethod: 恢复 R+X 失败");
    }
    return true;
}

void ClassesTab::PatcherView(Il2CppClass *klass, MethodInfo *method, const MethodParamList &paramsInfo,
                             Il2CppObject *thiz)
{
    {
        if (isMethodHooked(method))
        {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "Can't patch while hooked!");
            return;
        }
    }

    auto &o = oMap[method];
    auto type = method->getReturnType();
    // methodPointer 为空的方法（抽象/接口/泛型方法体）不能打补丁：
    // 旧代码只是把标签染成红色，按钮照样可点，点了就是往空地址写。
    if (method->methodPointer == nullptr)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 100, 100, 255));
        ImGui::TextWrapped("该方法没有可执行代码（抽象/接口/泛型方法体），无法打补丁");
        ImGui::PopStyleColor();
        ImGui::PopID();
        return;
    }
    if (strcmp(type->getName(), "System.Int16") == 0 || strcmp(type->getName(), "System.Int32") == 0 ||
        strcmp(type->getName(), "System.Int64") == 0 || strcmp(type->getName(), "System.UInt16") == 0 ||
        strcmp(type->getName(), "System.UInt32") == 0 || strcmp(type->getName(), "System.UInt64") == 0 ||
        strcmp(type->getName(), "System.Single") == 0 || strcmp(type->getName(), "System.Boolean") == 0 ||
        strcmp(type->getName(), "System.String") == 0 || type->isEnum())
    {
        if (ImGui::Button("Patch return value"))
        {
            ImGui::OpenPopup("HookReturnValuePopup");
        }
        if (!o.text.empty())
        {
            ImGui::SameLine();
            ImGui::Text("-> %s", o.text.c_str());
        }
    }
    else if (strcmp(type->getName(), "System.Void") == 0)
    {
        if (o.bytes.empty())
        {
            if (ImGui::Button("NOP"))
            {
                Patcher p{method};
                p.ret();
                auto patched = p.patch();
                if (patched.empty())
                {
                    LOGE("NOP 补丁失败");
                }
                else
                {
                    o.bytes = std::move(patched);
                }
            }
        }
        else
        {
            if (ImGui::Button("Restore"))
            {
                if (!RestorePatchedMethod(method, o.bytes))
                {
                    LOGE("恢复失败: %s", method->getName() ? method->getName() : "?");
                }
                o.bytes.clear();
                o.text.clear();
            }
        }
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
        ImGui::Text("Not supported!");
        ImGui::PopStyleColor();
    }
    if (ImGui::BeginPopup("HookReturnValuePopup"))
    {
        ImGui::Text("Change return value");
        ImGui::PushID(type);
        // 类型名长度不受控 —— 128 字节固定缓冲 + 无界 sprintf 就是栈溢出。
        // 顺带把 '#' 换掉：按钮标签同时是 ImGui ID，用户/元数据里出现
        // "##" 会让 ID 变化、交互错乱。
        char label[256]{0};
        if (!o.bytes.empty())
        {
            snprintf(label, sizeof(label), "Restore");
        }
        else
        {
            const char *typeName = type->getName() ? type->getName() : "?";
            for (size_t i = 0; i + 1 < sizeof(label) && typeName[i] != '\0'; i++)
            {
                label[i] = (typeName[i] == '#') ? '_' : typeName[i];
            }
        }
        if (ImGui::Button(label))
        {
            if (!o.bytes.empty())
            {
                // 走和打补丁一致的恢复流程（mprotect + memcpy + 刷 I-cache + 恢复 R+X），
                // 不能裸 memcpy：目标页此时通常是 R+X，直接写会 SIGSEGV。
                if (!RestorePatchedMethod(method, o.bytes))
                {
                    LOGE("恢复失败: %s", method->getName() ? method->getName() : "?");
                }
                o.bytes.clear();
                o.text.clear();
            }
            else
            {
                if (strcmp(type->getName(), "System.Int16") == 0 || strcmp(type->getName(), "System.Int32") == 0 ||
                    strcmp(type->getName(), "System.Int64") == 0 || strcmp(type->getName(), "System.UInt16") == 0 ||
                    strcmp(type->getName(), "System.UInt32") == 0 || strcmp(type->getName(), "System.UInt64") == 0 ||
                    strcmp(type->getName(), "System.Single") == 0 || strcmp(type->getName(), "System.Boolean") == 0 ||
                    strcmp(type->getName(), "System.String") == 0)
                {
                    if (strcmp(type->getName(), "System.Boolean") == 0)
                    {
                        poper.Open("BooleanSelector",
                                   [method](const std::string &b)
                                   {
                                       Patcher p{method};
                                       if (!p.valid())
                                       {
                                           LOGE("Patcher 初始化失败（该方法无 methodPointer?）");
                                           return;
                                       }
                                       p.movBool(b == "True");
                                       p.ret();

                                       if (!oMap[method].bytes.empty())
                                       {
                                           LOGE("oMap is not empty for %s", method->getName());
                                           return;
                                       }
                                       auto patched = p.patch();
                                       if (patched.empty())
                                       {
                                           LOGE("布尔补丁写入失败: %s", method->getName());
                                           return;
                                       }
                                       oMap[method].bytes = std::move(patched);
                                       oMap[method].text = b;
                                   });
                    }
                    else
                    {
                        auto typ = type;
                        auto m = method;
                        Keyboard::Open(
                            [typ, method = m](const std::string &text)
                            {
                                auto isString = strcmp(typ->getName(), "System.String") == 0;
                                if (text.empty())
                                    return;

                                auto type = typ;
                                // text 是用户从软键盘输进来的，stoi/stol/stof 遇到
                                // 非法文本或越界会抛。Keyboard::Update 虽然有异常边界，
                                // 但那只会 Reset 键盘并把异常吞掉，用户完全不知道自己
                                // 输入无效。这里就地解析，失败给出明确提示。
                                auto applyPatch = [&](const std::string &text) -> bool {
                                    Patcher p{method};
                                    if (!p.valid())
                                    {
                                        LOGE("Patcher 初始化失败（无 methodPointer?）");
                                        return false;
                                    }
                                    try
                                    {
                                        if (strcmp(type->getName(), "System.Int16") == 0)
                                        {
                                            p.movInt16(static_cast<int16_t>(std::stoi(text)));
                                        }
                                        else if (strcmp(type->getName(), "System.UInt16") == 0)
                                        {
                                            p.movUInt16(static_cast<uint16_t>(std::stoul(text)));
                                        }
                                        else if (strcmp(type->getName(), "System.Int32") == 0)
                                        {
                                            p.movInt32(std::stoi(text));
                                        }
                                        else if (strcmp(type->getName(), "System.UInt32") == 0)
                                        {
                                            p.movUInt32(static_cast<uint32_t>(std::stoul(text)));
                                        }
                                        else if (strcmp(type->getName(), "System.Int64") == 0)
                                        {
                                            p.movInt64(std::stoll(text));
                                        }
                                        else if (strcmp(type->getName(), "System.UInt64") == 0)
                                        {
                                            p.movUInt64(std::stoull(text));
                                        }
                                        else if (strcmp(type->getName(), "System.Single") == 0)
                                        {
                                            p.movFloat(std::stof(text));
                                        }
                                        else if (strcmp(type->getName(), "System.Boolean") == 0)
                                        {
                                            p.movBool(text == "True" || text == "true" || text == "1");
                                        }
                                        else if (isString)
                                        {
                                            // 这个托管字符串会被**写进方法体**，
                                            // 也就是说游戏每次调用这个方法都会拿到它。
                                            // 不加 GC 根的话，游戏随时可能回收并复用这块内存 ——
                                            // 用户的表现是「打上补丁之后，游戏在别的地方
                                            // 莫名其妙地读到乱码/崩掉」，而且崩溃点离
                                            // 真正的原因非常远。
                                            //
                                            // handle 放在 static 里**故意不释放**：
                                            // 这个字符串的生命周期必须和补丁一样长，
                                            // 而补丁可能被用户一直保留着。
                                            // 一个 GCHandle 占几个字节，可以接受。
                                            static std::vector<uint32_t> s_stringPatchHandles;
                                            Il2CppString *patchString = Il2cpp::NewString(text.c_str());
                                            if (patchString == nullptr)
                                            {
                                                LOGE("NewString 返回空，无法写入字符串补丁");
                                                return false;
                                            }
                                            uint32_t handle = Il2cpp::GC::NewHandle(patchString);
                                            if (handle == 0)
                                            {
                                                LOGE("字符串补丁加 GC 根失败，拒绝写入（否则可能被回收）");
                                                return false;
                                            }
                                            s_stringPatchHandles.push_back(handle);
                                            p.movPtr(patchString);
                                        }
                                        else
                                        {
                                            LOGE("不支持的补丁返回类型: %s", type->getName());
                                            return false;
                                        }
                                    }
                                    catch (const std::exception &e)
                                    {
                                        LOGE("返回值 \"%s\" 解析失败: %s", text.c_str(), e.what());
                                        return false;
                                    }
                                    p.ret();

                                    if (!oMap[method].bytes.empty())
                                    {
                                        LOGE("oMap is not empty for %s", method->getName());
                                        return false;
                                    }
                                    auto patched = p.patch();
                                    if (patched.empty())
                                    {
                                        LOGE("补丁写入失败: %s", method->getName());
                                        return false;
                                    }
                                    oMap[method].bytes = std::move(patched);
                                    oMap[method].text = text;
                                    return true;
                                };

                                applyPatch(text);
                            });
                    }
                }
                else if (type->isEnum())
                {
                    poper.Open(
                        "EnumSelector",
                        [method, type](const std::string &result)
                        {
                            auto *enumClass = type->getClass();
                            auto *field = enumClass ? enumClass->getField(result.c_str()) : nullptr;
                            if (!field)
                            {
                                LOGE("枚举 %s 找不到成员 %s", type->getName() ? type->getName() : "?", result.c_str());
                                return;
                            }
                            // 按真实底层宽度取值。旧代码固定 getStaticValue<int>()：
                            // 底层是 long/ulong 的枚举会让 il2cpp 往 4 字节变量写 8 字节。
                            auto raw = FieldInfo::getEnumStaticValue(field);
                            auto *baseType = Il2cpp::GetEnumBaseType(enumClass);
                            const char *baseName = baseType ? Il2cpp::GetTypeName(baseType) : nullptr;

                            Patcher p{method};
                            if (!p.valid())
                            {
                                LOGE("Patcher 初始化失败");
                                return;
                            }
                            // 返回值宽度同样要跟底层类型走。旧代码不管枚举多大
                            // 一律 movInt16：byte 底色的枚举写出 2 字节是对的，
                            // 但 long 底色的会被截断成 16 位。
                            if (baseName && (strcmp(baseName, "System.Int64") == 0 ||
                                             strcmp(baseName, "System.UInt64") == 0))
                            {
                                p.movInt64(static_cast<int64_t>(raw));
                            }
                            else
                            {
                                p.movInt32(static_cast<int32_t>(raw));
                            }
                            p.ret();

                            if (!oMap[method].bytes.empty())
                            {
                                LOGE("oMap is not empty for %s", method->getName());
                                return;
                            }
                            auto patched = p.patch();
                            if (patched.empty())
                            {
                                LOGE("枚举补丁写入失败: %s", method->getName());
                                return;
                            }
                            oMap[method].bytes = std::move(patched);
                            oMap[method].text = result;
                        },
                        type);
                }
            }
        }
        // if (ImGui::BeginPopup("BooleanSelector"))
        // {
        //     if (ImGui::Button("True"))
        //     {
        //         using namespace asmjit;
        //         Patcher p{method};
        //         p.movBool(true);
        //         p.ret();

        //         if (oMap[method].bytes.empty())
        //         {
        //             oMap[method].bytes = p.patch();
        //             oMap[method].text = "True";
        //         }
        //         else
        //         {
        //             LOGE("oMap is not empty for %s", method->getName());
        //         }

        //         ImGui::CloseCurrentPopup();
        //     }
        //     if (ImGui::Button("False"))
        //     {
        //         using namespace asmjit;
        //         Patcher p{method};
        //         p.movBool(false);
        //         p.ret();

        //         if (oMap[method].bytes.empty())
        //         {
        //             oMap[method].bytes = p.patch();
        //             oMap[method].text = "False";
        //         }
        //         else
        //         {
        //             LOGE("oMap is not empty for %s", method->getName());
        //         }
        //         ImGui::CloseCurrentPopup();
        //     }
        //     ImGui::EndPopup();
        // }
        // // ImGui::SetNextWindowSize(ImVec2(0, io.DisplaySize.y / 3.f));
        // ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0.f), ImVec2(-1, io.DisplaySize.y / 3.f));
        // if (ImGui::BeginPopup("EnumSelector")) // assume the current type is enum
        // {
        //     auto klass = type->getClass();
        //     for (auto field : klass->getFields())
        //     {
        //         auto fieldType = field->getType();
        //         if (Il2cpp::GetTypeIsStatic(fieldType) ||
        //             Il2cpp::GetFieldFlags(field) & FIELD_ATTRIBUTE_STATIC)
        //         {
        //             auto fieldName = field->getName();
        //             if (ImGui::Button(fieldName))
        //             {
        //                 int value = type->getClass()->getField(fieldName)->getStaticValue<int>();

        //                 using namespace asmjit;
        //                 Patcher p{method};
        //                 p.movInt16(value);
        //                 p.ret();

        //                 if (oMap[method].bytes.empty())
        //                 {
        //                     oMap[method].bytes = p.patch();
        //                     oMap[method].text = fieldName;
        //                 }
        //                 else
        //                 {
        //                     LOGE("oMap is not empty for %s", method->getName());
        //                 }
        //                 ImGui::CloseCurrentPopup();
        //             }
        //         }
        //     }
        //     ImGui::EndPopup();
        // }
        ImGui::PopID();
        ImGui::EndPopup();
    }
    // if (ImGui::Button("Patch"))
    // {
    //     using namespace asmjit;

    //     CodeHolder code;
    //     code.init(Environment(Arch::kAArch64));

    //     a64::Assembler assembler(&code);
    //     int a = 999;
    //     assembler.movz(a64::w0, 191);
    //     assembler.ret(a64::x30);

    //     std::vector<char> bytes;
    //     for (auto s : code.sections())
    //     {
    //         for (auto c : s->buffer())
    //         {
    //             LOGD("0x%X", c);
    //             bytes.push_back(c);
    //         }
    //     }
    //     auto rawBytes = bytes.data();
    //     auto protect = KittyMemory::ProtectAddr((void *)method->methodPointer,
    //                                             sizeof(method->methodPointer),
    //                                             PROT_READ | PROT_WRITE |
    //                                             PROT_EXEC);
    //     LOGINT(protect);
    //     auto src = method->methodPointer;
    //     memcpy((void *)src, (void *)rawBytes, sizeof(method->methodPointer));
    // }
}

void ClassesTab::HookerView(Il2CppClass *klass, MethodInfo *method, const MethodParamList &paramsInfo,
                            Il2CppObject *thiz)
{
    {
        bool patched = oMap[method].bytes.empty() == false;
        if (patched)
        {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "Can't hook while patched!");
            return;
        }
    }
    // 迭代器不能在锁外用。旧代码把 find() 的结果 `it` 一直带到下面读
    // it->second.hitCount —— 期间游戏线程的回调或后台的 ToggleHooker
    // 线程可能正在 rehash/erase 这张表。改成锁内取快照、锁外只用值。
    bool hooked;
    int hitCountSnapshot = 0;
    float cpsSnapshot = 0.f;
    {
        std::lock_guard guard(hookerMtx);
        auto it = hookerMap.find(method->methodPointer);
        hooked = it != hookerMap.end();
        if (hooked)
        {
            hitCountSnapshot = it->second.hitCount.load(std::memory_order_relaxed);
            cpsSnapshot = it->second.callsPerSecond;
        }
    }
    char label[16];
    if (!hooked)
    {
        snprintf(label, sizeof(label), "Trace");
    }
    else
    {
        snprintf(label, sizeof(label), "Restore");
        if (cpsSnapshot > 0.f)
        {
            ImGui::Text("调用 %d 次 (%.0f 次/秒)", hitCountSnapshot, cpsSnapshot);
        }
        else
        {
            ImGui::Text("调用 %d 次", hitCountSnapshot);
        }
        ImGui::Separator();
    }
    if (ImGui::Button(label))
    {
        Tool::ToggleHooker(method);
        std::lock_guard guard(hookerMtx);
        hooked = hookerMap.find(method->methodPointer) != hookerMap.end();
    }
    ImGui::Separator();
    if (hooked)
    {
        // 锁内一次性把要用的数据取出来，锁外画图。
        //
        // 不能整段持锁：这里是 ImGui 绘制，而游戏线程的 hook 回调要拿同一把锁
        // 记 hitCount —— 我们绘制多久，游戏就被卡多久。
        // 也不能持着迭代器出锁：后台的 ToggleHooker 线程随时可能 rehash/erase
        // 这张表，迭代器随即失效。锁内复制值是唯一安全的做法。
        std::vector<float> history;
        float cps = 0.f;
        bool backtracing = false;
        CircularBuffer<std::vector<std::string>> backtraced{10};
        {
            std::lock_guard guard(hookerMtx);
            auto found = hookerMap.find(method->methodPointer);
            if (found != hookerMap.end())
            {
                history = found->second.rateHistory;
                cps = found->second.callsPerSecond;
                backtracing = found->second.backtracing;
                backtraced = found->second.backtraced;
            }
        }

        // 调用频率曲线。
        //
        // 累计次数只能告诉你「它被调过」，告诉不了你「它有多热」。
        // 一个被调 100 次的方法可能只是启动时走了一遍；而 5000 次/秒意味着
        // 它在每帧的关键路径上 —— 这正是用户想知道的信息。
        //
        // 数据由 Tool::SampleHookRates() 在渲染线程按 250ms 采样，
        // 窗口约 30 秒。绝不放在 hook 回调里采样：那会让 UI 绘制变成
        // 游戏主路径上的额外开销。
        {
            if (history.empty())
            {
                ImGui::TextDisabled("等待采样…（每 250ms 一次，约 30 秒窗口）");
            }
            else
            {
                float maxRate = 0.f;
                for (float v : history)
                {
                    maxRate = std::max(maxRate, v);
                }
                // 纵轴下限给 1，避免「一直是 0」时曲线贴在底边看不出「无数据」
                ImGui::PlotLines("##calls", history.data(), (int)history.size(), 0, nullptr, 0.f,
                                 std::max(maxRate * 1.15f, 1.f), ImVec2(0, 40));
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("最近 %d 次采样（250ms 一次）\n峰值 %.0f 次/秒\n当前 %.0f 次/秒",
                                      (int)history.size(), maxRate, cps);
                }
            }
        }
        ImGui::Separator();

#ifdef USE_FRIDA
        if (!backtracing)
        {
            if (ImGui::Button("Backtrace"))
            {
                std::lock_guard guard(hookerMtx);
                auto found = hookerMap.find(method->methodPointer);
                if (found != hookerMap.end())
                {
                    found->second.backtracing = true;
                }
            }
        }
#else
        // 没有 USE_FRIDA 时这个按钮是个陷阱：点下去 backtracing 会被置成 true，
        // 但没有任何消费者（Frida::Init() 负责起 gummp 的 invocation listener，
        // 而它在 #ifdef USE_FRIDA 里），于是 backtraced 永远是空的 ——
        // 用户点了没反应也没报错，只能一直等。
        //
        // 宁可明确不可用，也不给一个看起来能用、实际什么都不做的按钮。
        ImGui::BeginDisabled();
        ImGui::Button("Backtrace");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("当前构建未启用 Frida 回溯。\n"
                              "它在 app/src/main/jni/Android.mk 的 LOCAL_CFLAGS 里，\n"
                              "加上 -DUSE_FRIDA 重新编译后可用。");
        }
#endif

        if (backtraced.empty())
        {
#ifdef USE_FRIDA
            ImGui::Text("Method has not been called");
#else
            ImGui::TextDisabled("回溯未启用（未定义 USE_FRIDA）");
#endif
        }
        else
        {
            ImGui::Text("Backtraced methods :");

            for (auto &result : backtraced)
            {
                for (auto &r : result)
                {
                    ImGui::Text("%s", r.c_str());
                }
                ImGui::PushStyleColor(ImGuiCol_Separator, IM_COL32(50, 255, 100, 255));
                ImGui::Separator();
                ImGui::PopStyleColor();
            }
        }
    }
}

bool ClassesTab::MethodViewer(Il2CppClass *klass, MethodInfo *method, const MethodParamList &paramsInfo,
                              Il2CppObject *thiz, bool includeInflated)
{
    bool zeroPointer = method->methodPointer == nullptr;

    if (callResults.find(method) == callResults.end())
    {
        callResults.emplace(method, (size_t)5);
    }

    bool methodIsStatic = Il2cpp::GetIsMethodStatic(method);

    // 512 字节对「返回类型 + 方法名 + 参数个数 + 前缀」来说不宽裕，
    // 而这些字符串全部来自 il2cpp 元数据（混淆过的名字可以很长），
    // 旧代码是无界 sprintf + 无容量 prepend，栈溢出只是时间问题。
    char treeLabel[512]{0};
    snprintf(treeLabel, sizeof(treeLabel), "%s %s(%zu)###", method->getReturnType()->getName(), method->getName(),
             paramsInfo.size());
    int pushedColor = 0;
    if (methodIsStatic)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 200, 100, 255));
        pushedColor++;
        Util::prependStringToBuffer(treeLabel, sizeof(treeLabel), "static ");
    }
    bool patched = oMap[method].bytes.empty() == false;
    // 必须持锁查。hookerMap 会被游戏线程的 hook 回调和后台的
    // ToggleHooker 线程改动（插入会触发 rehash），而这个函数对每个类的
    // 每个方法每帧都跑一次 —— 无锁 find 与并发 insert 相撞就是遍历已释放
    // 的桶数组。isMethodHooked 就是为这个场景写的，这里改用它。
    bool hooked = isMethodHooked(method);
    // sprintf(treeLabel, "%s##%p", treeLabel, method + j);
    if (zeroPointer)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 100, 100, 255));
        pushedColor++;
    }
    if (patched || hooked)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(25, 255, 125, 255));
        pushedColor++;
        if (hooked)
        {
            std::lock_guard guard(hookerMtx);
            int hitCount = hookerMap[method->methodPointer].hitCount.load(std::memory_order_relaxed);
            char hitLabel[96]{0};
            // 次/秒比累计次数更能说明问题：100 次可能是启动时调的，
            // 1000 次/秒 说明它在每帧的关键路径上。
            float cps = hookerMap[method->methodPointer].callsPerSecond;
            if (cps > 0.f)
            {
                snprintf(hitLabel, sizeof(hitLabel), "%.0f/s | 共 %d | ", cps, hitCount);
            }
            else
            {
                snprintf(hitLabel, sizeof(hitLabel), "共 %d 次 | ", hitCount);
            }
            Util::prependStringToBuffer(treeLabel, sizeof(treeLabel), hitLabel);
        }
        else if (patched)
        {
            auto text = oMap[method].text;
            if (!text.empty())
            {
                char buff[64]{0};
                snprintf(buff, sizeof(buff), "Returns %s | ", text.c_str());
                Util::prependStringToBuffer(treeLabel, sizeof(treeLabel), buff);
            }
        }
    }
    bool state = ImGui::TreeNode(treeLabel);
    if (state)
    {
        if (pushedColor)
        {
            ImGui::PopStyleColor(pushedColor);
            pushedColor = 0;
        }

        if (ImGui::BeginTabBar("##methoder"))
        {
            if (ImGui::BeginTabItem("Caller"))
            {
                CallerView(klass, method, paramsInfo, thiz);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Patcher"))
            {
                PatcherView(klass, method, paramsInfo, thiz);
                ImGui::EndTabItem();
            }
            // 标签保留 "Tracer"：它对应的正是 README 里写的「追踪（Trace）」功能 ——
            // 而追踪**就是**用 Dobby 的 DobbyInstrument 实现的，所以里面放
            // HookerView（"Hooker" 是代码里的实现名，"Tracer" 是功能名）。
            //
            // 我一度把它改成 "Hooker"，理由是「页签叫什么就该是什么」——
            // 但那会让**用户**看到的词和 README 里的功能名对不上：
            // 照着 README 找「追踪」的人会以为页签不见了。
            // 代价是代码名和页签名不一致，可以接受。
            //
            // 被删掉的是 Tool::Tracer()：**只有声明、没有定义、没有调用者**。
            // 它和这个页签没有关系（页签走的是 HookerView）。
            if (ImGui::BeginTabItem("Tracer"))
            {
                HookerView(klass, method, paramsInfo, thiz);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::TreePop();
    }
    poper.Update();
    ImGui::PopStyleColor(pushedColor);
    return state;
}

const ClassesTab::MethodParamList &ClassesTab::getCachedParams(MethodInfo *method)
{
    // 返回的是**引用**，所以需要一个生命周期足够长的对象来持有结果。
    //
    // 三个改进（相对旧实现）：
    // 1. **一次查找**。旧代码是 find() → operator[] → return operator[]，
    //    同一件事做了三遍哈希查找。而这个函数在渲染线程上、每个已 hook 的
    //    方法每帧都会调一次 —— 3N 次哈希 × 60fps。
    // 2. **有上限**。key 是 MethodInfo*，稳定不会失效，但会话里浏览过的方法
    //    会一直堆积，从不清理。给个上限，超了整体清空。
    // 3. **判空**。method 为空时返回空列表，而不是空指针解引用。
    static std::mutex cacheMutex;
    static std::unordered_map<MethodInfo *, MethodParamList> params;

    if (method == nullptr)
    {
        static const MethodParamList empty;
        return empty;
    }

    std::lock_guard guard(cacheMutex);
    auto it = params.find(method);
    if (it != params.end())
    {
        return it->second;
    }

    if (params.size() >= kParamCacheLimit)
    {
        LOGI("方法参数缓存达到上限 (%zu)，清空重建", params.size());
        params.clear();
    }
    auto inserted = params.emplace(method, method->getParamsInfo());
    return inserted.first->second;
}

// static std::unordered_map<Il2CppClass *, bool> states;
void ClassesTab::ClassViewer(Il2CppClass *klass)
{
    if (ImGui::Button("Inspect Objects"))
    {
        ImGui::OpenPopup("DumpPopup");
    }
    {
        ImGui::SameLine();
        bool &state = states[klass];
        char label[12]{0};
        if (!state)
            sprintf(label, "Trace all");
        else
            sprintf(label, "Restore");
        if (ImGui::Button(label))
        {
            if (!state)
            {
                ImGui::OpenPopup("ConfirmPopup");
            }
            else
            {
                state = false;
                for (auto &[method, paramsInfo] : methodMap[klass])
                {
                    if (!method->methodPointer && !Il2cpp::GetIsMethodInflated(method))
                        continue;

                    Tool::ToggleHooker(method, 0);
                }
            }
        }
        if (ImGui::BeginPopup("ConfirmPopup"))
        {
            ImGui::TextColored(ImVec4(0.8, 0.8, 0, 1), "WARNING: There's a high-risk of crash");
            if (ImGui::Button("Continue?"))
            {
                state = true;
                SpawnDetached(
                    [this, klass]
                    {
                        for (auto &[method, paramsInfo] : methodMap[klass])
                        {
                            if (!method->methodPointer && !Il2cpp::GetIsMethodInflated(method))
                                continue;

                            Tool::ToggleHooker(method, 1);
                        }
                    },
                    "TraceAll",
                    [this, klass]()
                    {
                        // 线程没起来。上面已经把 state 置成 true 了，
                        // 这里必须退回去，否则按钮会一直显示「Restore」，
                        // 而实际上一个方法都没 hook 上。
                        states[klass] = false;
                        LOGE("Trace all: 无法创建后台线程，state 已回滚");
                    });
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    // ImGui::SameLine();
    // if (ImGui::Button("Add to Tracer"))
    // {
    //     tracer.push_back(klass);
    // }
    if (ImGui::BeginPopup("DumpPopup"))
    {
        ImGuiObjectSelector(ImGui::GetID("ObjectSelector"), klass, "Inspect",
                            [this](Il2CppObject *object) { setJsonObject(object); });
        ImGui::EndPopup();
    }
    ImGui::PopID();
    ImGui::Separator();

    int j = 0;
    for (auto &[method, paramsInfo] : methodMap[klass])
    {
        ImGui::PushID(method + j++);
        MethodViewer(klass, method, paramsInfo);
        ImGui::Separator();
        ImGui::PopID();
    }
}

void ClassesTab::Draw(int index, bool closeable)
{
    static ImGuiIO &io = ImGui::GetIO();
    // 先认领后台算好的筛选结果。认领不到就保持上一帧的内容继续画 ——
    // 这样输入过程中界面不会闪成空白。
    PollFilterResult();
    char tabLabel[256];
    if (filter.empty())
    {
        if (index >= 0)
            snprintf(tabLabel, sizeof(tabLabel), "Classes [%d]", index + 1);
        else
            snprintf(tabLabel, sizeof(tabLabel), "Classes");
    }
    else
    {
        // filter 是用户输入，长度不受控 —— 旧代码 sprintf 进 256 字节，
        // 而中文是 UTF-8（字节数 ≈ 3× 字符数），很容易写穿。
        //
        // 顺带把 '#' 换成 '_'：tabLabel 同时是 ImGui 的控件 ID，
        // 用户输入里出现 "##" 会让 ID 随输入变化，交互直接错乱。
        std::string safe;
        safe.reserve(filter.size());
        for (char c : filter)
        {
            safe.push_back(c == '#' ? '_' : c);
        }
        snprintf(tabLabel, sizeof(tabLabel), "%s", safe.c_str());
    }

    if ((currentlyOpened = ImGui::BeginTabItem(tabLabel, closeable ? &opened : nullptr,
                                               setOpenedTab ? ImGuiTabItemFlags_SetSelected : 0)))
    {
        setOpenedTab = false;
        ImGui::BeginDisabled(includeAllImages);
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(-1, io.DisplaySize.y / 1.5f));
        if (ImGui::BeginCombo("Image##ImageSelector", selectedImage->getName()))
        {

            for (int i = 0; i < g_Images.size(); i++)
            {
                bool selected = selectedImageIndex == i;
                if (ImGui::Selectable(g_Images[i]->getName(), selected))
                {
                    selectedImage = g_Images[i];
                    selectedImageIndex = i;
                    FilterClasses(filter);
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Checkbox("All", &includeAllImages))
        {
            FilterClasses(filter);
        }

        // 搜索框。
        //
        // 旧实现是「一个显示当前 filter 的按钮 + 弹软键盘」：
        //   sprintf(filterBuffer, "Filter : %s | %zu of %zu", filter.c_str(), ...)
        //   → char filterBuffer[256]
        // filter 来自软键盘的用户输入，长度完全不受控 —— 这是栈溢出。
        // 而且中文是 UTF-8，字节数远大于字符数，更容易踩到。
        //
        // 现在给一个真正的输入框（能直接打字，比弹键盘快得多）+ 保留原按钮。
        // 标签用 snprintf 截断，并且**不含用户输入**：把用户文本放进
        // ImGui 的按钮标签还要额外考虑 "##" 这种 ID 分隔符会截断显示。
        {
            ImGui::SetNextItemWidth(-1.0f);
            char inputBuf[256] = {0};
            // filter 是 std::string，输入框需要 char*。长度截断到 255，
            // 保证 snprintf 一定有终止。
            snprintf(inputBuf, sizeof(inputBuf), "%s", filter.c_str());
            if (ImGui::InputText("##filterinput", inputBuf, sizeof(inputBuf),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
            {
                filter = inputBuf;
                FilterClasses(filter);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("直接输入即可筛选。留空显示全部。\n"
                                  "下方 Filter Options 可以限定搜索范围。");
            }
        }

        char filterBuffer[320];
        {
            // 按钮标签同时充当 ImGui 的控件 ID。ImGui 用 "##" 之后的内容
            // 作为 ID、把之前的内容作为显示文本 —— 所以 filter 里只要有
            // "##"，控件 ID 就会随输入变化，交互直接错乱。
            // 这里把 '#' 替换掉再拼进标签，显示效果不受影响。
            std::string safeFilter;
            safeFilter.reserve(filter.size());
            for (char c : filter)
            {
                safeFilter.push_back(c == '#' ? '_' : c);
            }
            snprintf(filterBuffer, sizeof(filterBuffer), "Filter : %s | %zu of %zu##filterbtn",
                     safeFilter.empty() ? "(none)" : safeFilter.c_str(), filteredClasses.size(),
                     classes.size());
        }
        // 筛选在后台跑。结果还没回来时明确标出来 —— 否则用户会以为
        // 「输入了没反应」或者「列表卡住了」。
        if (IsFilterPending())
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.f, 0.9f, 0.4f, 1.f), "筛选中…");
        }
        else if (const char *filterFailure = GetFilterFailure(); filterFailure != nullptr)
        {
            // 上一轮筛选失败了。明确说出来，而不是让用户对着一个
            // 空列表 + 一句「没有匹配」怀疑是自己关键字打错了。
            //
            // 「重试」另起一行：跟在失败原因后面的话，原因一长按钮就被
            // 顶出屏幕 —— 而它恰恰是用户此刻最想按的那个。
            ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "筛选失败: %s", filterFailure);
            if (ImGui::SmallButton("重试"))
            {
                FilterClasses(filter);
            }
        }
        if (ImGui::Button(filterBuffer, ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)) && !Keyboard::IsOpen())
        {
            Keyboard::Open(
                [this](const std::string &text)
                {
                    filter = text;
                    FilterClasses(filter);
                });
        }
        if (!Keyboard::IsOpen() && ImGui::IsItemHeld())
        {
            Keyboard::Open(filter.c_str(),
                           [this](const std::string &text)
                           {
                               filter = text;
                               FilterClasses(filter);
                           });
        }
        if (ImGui::Button("Filter Options"))
        {
            ImGui::OpenPopup("FilterOptions");
        }

        {
            static bool processing = false;
            // processing 是本函数内的 static —— lambda **不能按引用捕获它**
            // （没有自动存储期）。SpawnDetached 的失败回调需要复位它，
            // 所以把地址取出来传进回调。
            bool *processingFlag = &processing;
            ImGui::SameLine();
            char label[12]{0};
            if (!traceState)
                sprintf(label, "Trace all");
            else
                sprintf(label, "Restore");

            bool disabled = false;
            if (processing)
            {
                disabled = true; // creating variable because `processing` could be set to false and we don't call
                                 // `EndDisabled`
                ImGui::BeginDisabled();
            }
            if (ImGui::Button(label))
            {
                {
                    if (!traceState)
                    {
                        ImGui::OpenPopup("ConfirmPopup");
                    }
                    else
                    {
                        traceState = false;
                        SpawnDetached(
                            [this]
                            {
                                LOGD("Restoring all methods...");
                                processing = true;
                                maxProgress = tracedMethods.size();
                                for (auto method : tracedMethods)
                                {
                                    Tool::ToggleHooker(method);
                                }
                                LOGD("Restored %zu methods", tracedMethods.size());
                                tracedMethods.clear();
                                processing = false;
                                maxProgress = 0;
                                progress = 0;
                                LOGD("Done");
                            },
                            "RestoreAll",
                            [processingFlag]()
                            {
                                *processingFlag = false;
                            });
                    }
                }
            }

            if (disabled)
                ImGui::EndDisabled();

            if (ImGui::BeginPopup("ConfirmPopup"))
            {
                ImGui::TextColored(ImVec4(0.8, 0.8, 0, 1), "WARNING: There's a high-risk of crash");
                if (ImGui::Button("Continue?"))
                {
                    traceState = true;
                    SpawnDetached(
                        [this]
                        {
                            LOGD("Tracing all methods...");
                            for (auto &klass : filteredClasses)
                            {
                                for (auto &[method, paramsInfo] : methodMap[klass])
                                {
                                    if (!method->methodPointer && !Il2cpp::GetIsMethodInflated(method))
                                        continue;

                                    maxProgress++;
                                }
                            }
                            processing = true;
                            for (auto &klass : filteredClasses)
                            {
                                states[klass] = true;
                                for (auto &[method, paramsInfo] : methodMap[klass])
                                {
                                    if (!method->methodPointer && !Il2cpp::GetIsMethodInflated(method))
                                        continue;

                                    if (Tool::ToggleHooker(method, 1))
                                    {
                                        tracedMethods.push_back(method);
                                    }
                                    progress++;
                                }
                            }
                            LOGD("Traced %zu methods", tracedMethods.size());

                            processing = false;
                            maxProgress = 0;
                            progress = 0;
                            LOGD("Done");
                        },
                        "HookToggle",
                        [processingFlag]()
                        {
                            *processingFlag = false;
                        });
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            if (processing)
            {
                ImGui::SameLine();
                ImGui::Text("Processing %d of %d ...", progress, maxProgress);
            }
        }
        if (ImGui::BeginPopup("FilterOptions"))
        {
            if (ImGui::Checkbox("Case-Sensitive", &caseSensitive))
            {
                FilterClasses(filter);
            }
            ImGui::Text("搜索范围（可多选）");
            ImGui::SetItemTooltip("旧版本这三个是互斥单选，必须先决定搜什么才能输关键词。\n"
                                  "现在可以同时勾选：一次就能搜出「类名匹配 或 方法名匹配」的结果。\n"
                                  "都不勾 = 只按类名筛选。");
            ImGui::Checkbox("##byClass", &filterByClass);
            ImGui::SameLine();
            ImGui::TextUnformatted("类名");
            ImGui::SameLine();
            ImGui::Checkbox("##byMethod", &filterByMethod);
            ImGui::SameLine();
            ImGui::TextUnformatted("方法名");
            ImGui::SameLine();
            ImGui::Checkbox("##byField", &filterByField);
            ImGui::SameLine();
            ImGui::TextUnformatted("字段名");
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("搜索字段名");
            }

            if (ImGui::Button("应用搜索范围"))
            {
                if (!filterByClass && !filterByMethod && !filterByField)
                {
                    // 一个都不勾就没有任何东西可搜，会显示空列表，
                    // 用户会以为「搜索坏了」。落到最合理的默认：按类名。
                    filterByClass = true;
                }
                FilterClasses(filter);
            }
            if (ImGui::Checkbox("Show All Classes", &showAllClasses))
            {
                FilterClasses(filter);
            }
            ImGui::EndPopup();
        }
        ImGui::Separator();
        if (!filteredClasses.empty())
        {
            ImGui::BeginChild("Child", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
            // **只渲染前 N 个**，其余用「显示更多」逐步展开。
            //
            // 旧代码把**每一个**匹配到的类都画成一个 CollapsingHeader，
            // 每帧一次。代价是每个类都要：
            //   - klass->getFullName() —— 返回 std::string（**按值**），
            //     也就是一次堆分配 + 释放；全限定名普遍超过 SSO 的 15 字符
            //   - ImGui 的 ID 哈希 + 树节点入栈
            // 过滤器留空时是「显示全部」，一个正常规模的 Unity 游戏
            // 几万个类 —— 每帧几万次堆分配，界面会直接卡住。
            //
            // 逐个渲染上万个 ImGui 控件本来也没有意义：列表太长时
            // 可见区就那么几行，其余的都在屏幕外。
            static int renderLimit = 200;
            const int total = static_cast<int>(filteredClasses.size());
            // 换了搜索词就收回上限 —— 否则上一次「显示更多」到 2000 的状态
            // 会一直跟着，换个词还是渲染两千个。
            // （只按 total 判断是不够的：新查询结果数相近时不会触发。）
            static std::string lastFilterKey;
            const std::string filterKey = filter + "|" +
                                          (selectedImage ? selectedImage->getName() : "") + "|" +
                                          std::to_string(filterByClass) +
                                          std::to_string(filterByMethod) +
                                          std::to_string(filterByField) +
                                          std::to_string(showAllClasses);
            if (filterKey != lastFilterKey)
            {
                lastFilterKey = filterKey;
                renderLimit = 200;
            }
            const int shown = std::min(renderLimit, total);
            for (int i = 0; i < shown; i++)
            {
                auto klass = filteredClasses[i];
                if (klass == nullptr)
                {
                    continue;
                }

                // GetClassType 可能返回空（元数据被裁剪时）。
                auto *klassType = Il2cpp::GetClassType(klass);
                bool isValueType = klassType != nullptr && klassType->isValueType();
                int pushedColor = 0;
                if (isValueType)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(222, 222, 222, 255));
                    pushedColor++;
                }
                bool collapsingHeader = ImGui::CollapsingHeader(klass->getFullName().c_str());
                // 长按类名 → 开一个新 tab 并按这个类名筛选。
                // 只在「按类名搜索」时有意义：按方法/字段搜出来的结果，
                // 类名未必包含关键词，填进新 tab 只会得到空列表。
                if ((filterByClass || (!filterByMethod && !filterByField)) && ImGui::IsItemHeld(0.7f))
                {
                    auto name = Util::extractClassNameFromTypename(klass->getFullName().c_str());
                    auto &tab = Tool::OpenNewTab();
                    tab.filter = name;
                    tab.selectedImage = klass->getImage();
                    tab.FilterClasses(tab.filter);
                }
                if (collapsingHeader)
                {
                    if (pushedColor)
                    {
                        ImGui::PopStyleColor(pushedColor);
                        pushedColor = 0;
                    }
                    ImGui::PushID(i);
                    // TODO: Rename Dump (DumpPopup and other related functions used) to Inspect
                    ClassViewer(klass);
                }
                if (pushedColor)
                {
                    ImGui::PopStyleColor(pushedColor);
                    pushedColor = 0;
                }
            }
            if (shown < total)
            {
                ImGui::Separator();
                ImGui::TextDisabled("已显示 %d / %d 个类（渲染上万个控件会明显卡顿）", shown, total);
                if (ImGui::Button("显示更多"))
                {
                    renderLimit += 500;
                }
                ImGui::SameLine();
                if (ImGui::Button("全部展开（可能卡）"))
                {
                    renderLimit = total;
                }
            }
            ImGui::ScrollWhenDraggingOnVoid();
            ImGui::EndChild();
        }
        else if (!filter.empty())
        {
            // 空结果时明确说明是「没匹配上」，而不是让用户面对一片空白
            // 去猜是不是界面坏了。同时给出最可能的原因和下一步。
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "没有匹配 \"%s\" 的结果", filter.c_str());
            ImGui::BulletText("当前范围: %s",
                              (filterByMethod && filterByField) ? "方法名 / 字段名"
                              : filterByMethod                ? "方法名"
                              : filterByField                 ? "字段名"
                                                              : "类名");
            ImGui::BulletText("共扫描了 %zu 个类", classes.size());
            if (!caseSensitive)
            {
                ImGui::BulletText("当前不区分大小写");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("试试区分大小写"))
            {
                caseSensitive = !caseSensitive;
                FilterClasses(filter);
            }
        }
        ImGui::EndTabItem();
    }
}
void ClassesTab::DrawTabMap()
{
    for (auto it = tabMap.begin(); it != tabMap.end();)
    {
        auto &[object, visible] = *it;
        char buff[32]{0};
        // 64 位下 %p 最长 18 个十六进制字符 + "0x" + 两个方括号 + NUL = 23，
        // 32 够用；用 snprintf 明确表达「接受截断」而不是靠算。
        snprintf(buff, sizeof(buff), "[%p]", static_cast<void *>(object));

        if (!visible)
        {
            it = tabMap.erase(it);
            dataMap.erase(object);
        }
        else
        {
            if (ImGui::BeginTabItem(buff, &visible))
            {
                ImGuiJson(object);
                ImGui::EndTabItem();
            }
            ++it;
        }
    }
}

// paths 是**只读**的（原签名是 vector&，但函数体只做复制和读取，从不写入）。
// 这一点很重要：调用它的那些 lambda 之前用 &paths 捕获，而 paths 是
// dataMap[rootObj].second —— DrawTabMap 里 `dataMap.erase(object)`
// （用户关掉那个对象的标签页）会把它整个销毁，而 lambda 还躺在全局的
// Keyboard::lastCallback 里等下一帧触发 → 对已销毁的 vector 取引用。
//
// 让签名变成 const& 就是在类型层面禁止这种别名捕获；调用处按值捕获即可
// （一个几条短字符串的小 vector，每次交互复制一次，可忽略）。
void ensureIfValueType(Il2CppObject *currentObj, const std::vector<std::string> &paths,
                       Il2CppObject *rootObj)
{
    // 这条路径把「被就地改写的值类型」写回它在父对象里的字段槽位。
    // 注意：il2cpp_field_set_value(obj, f, ptr) 是从 ptr 拷贝 f 长度的那几个字节
    // 到 obj+f 的偏移处，所以对值类型字段直接传 unboxed 的载荷指针是对的，
    // 不需要自己再加偏移 —— 偏移是作用在目标端的。
    if (!currentObj || !rootObj || currentObj == rootObj)
        return;

    // GetClassType 可能返回空。
    auto *currentType = Il2cpp::GetClassType(currentObj->klass);
    bool isValueType = currentType != nullptr && currentType->isValueType();
    if (isValueType)
    {
        if (paths.size() > 1)
        {
            auto pathsButLast = std::vector(paths.begin(), paths.end() - 1);
            // dump() 在目标为空时会返回 {nullptr, ...}，旧代码直接
            // beforeObject->klass 就是空指针解引用。
            auto [beforeObject, j] = rootObj->dump(pathsButLast, true);
            if (!beforeObject || !beforeObject->klass)
            {
                LOGE("ensureIfValueType: 父对象为空，无法写回");
                return;
            }

            auto path = paths.rbegin();
            std::istringstream iss(path->c_str());
            std::string _, val;
            iss >> _ >> val;
            void *unboxed = Il2cpp::GetUnboxedValue(currentObj);
            if (!unboxed)
            {
                return;
            }
            // object->setField(val.c_str(), unboxed);
            // getField 是单类查找，继承来的字段会返回 null。
            auto f = beforeObject->klass->getFieldInHierarchy(val.c_str());
            if (!f)
            {
                LOGE("ensureIfValueType: 找不到字段 %s", val.c_str());
                return;
            }
            Il2cpp::SetFieldValue(beforeObject, f, unboxed);
            ensureIfValueType(beforeObject, pathsButLast, rootObj);
        }
        else
        {
            auto path = paths.begin();

            std::istringstream iss(path->c_str());
            std::string _, val;
            iss >> _ >> val;
            void *unboxed = Il2cpp::GetUnboxedValue(currentObj);
            if (!unboxed || !rootObj->klass)
            {
                return;
            }
            // object->setField(val.c_str(), unboxed);
            auto f = rootObj->klass->getFieldInHierarchy(val.c_str());
            if (!f)
            {
                LOGE("ensureIfValueType: 找不到字段 %s", val.c_str());
                return;
            }
            Il2cpp::SetFieldValue(rootObj, f, unboxed);
        }
    }
}

// 标记「这个对象需要在下一帧重新 dump」。
//
// 旧实现是一个函数内 `static bool doRefresh`，被 ImGuiJson 的**所有调用者**
// 共享。而 ImGuiJson 可以同时作用于多个对象（用户在几个对象的 JSON 视图
// 之间切换），于是改 A 的字段会让 B 也跟着重新 dump —— 用户看到
// 「我没动它，它的内容自己变了」。
//
// 做成文件级函数而不是局部变量：置位它的都是 JSON 表格里那些**无捕获
// lambda**（软键盘回调、枚举选择回调），那些 lambda 捕获不了局部变量。
static std::unordered_map<Il2CppObject *, bool> g_refreshRequests;

// 记一条「字段被改了」。给「改动记录」页用。
//
// 这里**不**去读旧值：写之前它就已经被覆盖了，而界面显示的 JSON 是
// 上一帧 dump 的快照，不一定是最新的。与其给一个可能不对的「原值 → 新值」，
// 不如只如实记「新值」。
static void RecordFieldChange(Il2CppObject *rootObj, const std::string &field,
                              const std::string &type, const std::string &value)
{
    if (!rootObj || !rootObj->klass || !rootObj->klass->getName())
    {
        return;
    }
    const std::string target = std::string(rootObj->klass->getName()) + "." + field;
    const std::string detail = type.empty() ? value : (type + " = " + value);
    ChangeLog::Record(ChangeLog::Kind::Field, target, detail);
}
static void RequestRefresh(Il2CppObject *object)
{
    if (object == nullptr)
    {
        return;
    }
    // 残留条目只可能来自「置位了但那一帧对象没被绘制」。数量以用户曾经
    // 查看过的对象数封顶，实践中很小；这里给个上限兜底。
    if (g_refreshRequests.size() > 256)
    {
        g_refreshRequests.clear();
    }
    g_refreshRequests[object] = true;
}

static bool ConsumeRefresh(Il2CppObject *object)
{
    auto it = g_refreshRequests.find(object);
    if (it == g_refreshRequests.end())
    {
        return false;
    }
    g_refreshRequests.erase(it);
    return true;
}

// ---------------------------------------------------------------------------
// 数值字段编辑的两个配套辅助
//
// 问题一：往返截断（静默破坏游戏状态）
//   旧代码对**所有**浮点字段都走 float，对**所有**整型字段都走 int：
//     ImGui::Text("%s = %f", key, value.get<float>());
//     Keyboard::Open(std::to_string(value.get<float>()).c_str(), ...)
//     ImGui::Text("%s = %d", key, value.get<int>());
//     Keyboard::Open(std::to_string(value.get<int>()).c_str(), ...)
//   Double 字段 0.1234567890123 会以 0.123457 显示并**预填进输入框**；
//   Int64/UInt64 超过 INT_MAX 的值会显示成负数（静默 static_cast，不抛）。
//   用户只要点「确认」而没改内容，截断后的值就被**写回游戏内存** ——
//   这比崩溃更糟，因为它不报错，只是悄悄把游戏改坏了。
//
// 问题二：解析异常会打断 ImGui 的 Begin/End 配对
//   std::stof/stod/stoi/stoul/stoll/stoull 对非数字输入全部抛
//   std::invalid_argument。这些 lambda 是在 Keyboard::Update() 里被调用的，
//   那是在 ImGui::Begin 之后、EndTabItem 之前。抛出去虽然有 Keyboard 的
//   try/catch 兜底（不会 std::terminate），但这一帧剩下的
//   EndTabItem/End 全被跳过 → ImGui 的 Begin/End 栈失配 → 下一帧撞上
//   IM_ASSERT → __builtin_trap() → **无声的 SIGILL**。
//   （对比：CallerView 里的同类解析早就包了 try/catch，注释也写了理由。）
// ---------------------------------------------------------------------------

// 按字段的**声明类型**把 JSON 值格式化成可编辑文本。
static std::string FormatFieldForEdit(const std::string &type, const nlohmann::ordered_json &value)
{
    if (type == "Double")
    {
        // 不能走 float。用 %.17g 保证往返无损。
        char buf[64];
        snprintf(buf, sizeof(buf), "%.17g", value.get<double>());
        return buf;
    }
    if (type == "Single")
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.9g", value.get<float>());
        return buf;
    }
    if (type == "Int64" || type == "UInt64")
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value.get<int64_t>()));
        return buf;
    }
    if (type == "Int32" || type == "UInt32")
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value.get<int32_t>()));
        return buf;
    }
    if (type == "Int16" || type == "UInt16")
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value.get<int16_t>()));
        return buf;
    }
    // 未知类型：走 float 旧路径，至少不崩
    return std::to_string(value.get<float>());
}

// 解析用户输入并按声明类型写回字段。
// 任何解析失败都返回 false，**绝不把异常抛进 ImGui 帧**。
static bool ParseAndSetNumericField(Il2CppObject *object, const std::string &type,
                                    const std::string &fieldName, const std::string &text)
{
    // 必须整串都吃掉。std::stoll / stod **接受部分输入**：
    // std::stoll("12abc") 静默返回 12，std::stod("1.5e") 静默返回 1.5。
    // 那意味着用户敲错一个字符，工具就把一个**不同的值**写进游戏内存，
    // 而界面上看不出任何异常 —— 比直接报错糟得多。
    auto allConsumed = [&text](size_t pos)
    {
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
        {
            ++pos;
        }
        return pos == text.size();
    };

    // 带**范围检查**的整数解析。
    //
    // 旧代码是 `static_cast<int16_t>(std::stoll(text))`：往 Int16 字段里
    // 填 40000，stoll 正常返回 40000，转 int16_t 变成 -25536，
    // **静默写进游戏**。用户看着自己填的 40000，字段却变成负数。
    //
    // 范围不合法时宁可拒绝写入：宁可用户知道「填不下」，
    // 也不能悄悄把一个错误的值写进别人的进程。
    auto parseInteger = [&text, &allConsumed](long long lo, long long hi, long long &out) -> bool
    {
        size_t pos = 0;
        long long v = 0;
        try
        {
            v = std::stoll(text, &pos, 10);
        }
        catch (...)
        {
            return false;
        }
        if (!allConsumed(pos) || v < lo || v > hi)
        {
            return false;
        }
        out = v;
        return true;
    };

    auto parseFloat = [&text, &allConsumed](double &out) -> bool
    {
        size_t pos = 0;
        double v = 0.0;
        try
        {
            v = std::stod(text, &pos);
        }
        catch (...)
        {
            return false;
        }
        if (!allConsumed(pos))
        {
            return false;
        }
        out = v;
        return true;
    };

    try
    {
        if (type == "Single" || type == "Double")
        {
            double d = 0.0;
            if (!parseFloat(d))
            {
                return false;
            }
            if (type == "Single")
            {
                // Single 只有约 7 位有效数字。填一个超出范围的值进去
                // 会被静默舍入成 inf，这里拦下来。
                const float f = static_cast<float>(d);
                if (!std::isfinite(f))
                {
                    LOGW("字段 %s: %g 超出 Single 可表示范围", fieldName.c_str(), d);
                    return false;
                }
                object->setField(fieldName.c_str(), f);
            }
            else
            {
                object->setField(fieldName.c_str(), d);
            }
        }
        else if (type == "UInt64")
        {
            // UInt64 上界超过 long long，单独走 stoull
            size_t pos = 0;
            unsigned long long u = 0;
            try
            {
                u = std::stoull(text, &pos, 10);
            }
            catch (...)
            {
                return false;
            }
            if (!allConsumed(pos))
            {
                return false;
            }
            object->setField(fieldName.c_str(), static_cast<uint64_t>(u));
        }
        else if (type == "Int16" || type == "UInt16" || type == "Int32" ||
                 type == "UInt32" || type == "Int64")
        {
            long long lo = 0, hi = 0, v = 0;
            if (type == "Int16")
            {
                lo = -32768; hi = 32767;
            }
            else if (type == "UInt16")
            {
                lo = 0; hi = 65535;
            }
            else if (type == "Int32")
            {
                lo = -2147483648LL; hi = 2147483647LL;
            }
            else if (type == "UInt32")
            {
                lo = 0; hi = 4294967295LL;
            }
            else // Int64
            {
                lo = std::numeric_limits<long long>::min();
                hi = std::numeric_limits<long long>::max();
            }
            if (!parseInteger(lo, hi, v))
            {
                return false;
            }
            if (type == "Int16")
                object->setField(fieldName.c_str(), static_cast<int16_t>(v));
            else if (type == "UInt16")
                object->setField(fieldName.c_str(), static_cast<uint16_t>(v));
            else if (type == "Int32")
                object->setField(fieldName.c_str(), static_cast<int32_t>(v));
            else if (type == "UInt32")
                object->setField(fieldName.c_str(), static_cast<uint32_t>(v));
            else
                object->setField(fieldName.c_str(), static_cast<int64_t>(v));
        }
        else
        {
            LOGW("未知的数值字段类型 %s，未写入", type.c_str());
            return false;
        }
    }
    catch (const std::exception &e)
    {
        // 关键：**就地消化**。让异常逃出去会打断这一帧的 ImGui Begin/End 配对。
        LOGE("字段 %s 的输入 \"%s\" 不是合法的 %s: %s", fieldName.c_str(), text.c_str(),
             type.c_str(), e.what());
        return false;
    }
    return true;
}

void ClassesTab::ImGuiJson(Il2CppObject *rootObj)
{
    // auto &paths = tool.dataMap[object].second;
    auto &paths = getJsonPaths(rootObj);

    int indentCounter = 0;
    auto currentObj = dataMap[rootObj].first.first;

    // currentObj 可能为 nullptr：dump() 在路径解析不出来时返回 {nullptr, ...}，
    // 而 dataMap[rootObj] 在 key 不存在时 operator[] 会默认构造出一个空 pair
    // （first.first 就是 nullptr）。
    //
    // 旧代码在 2413 行直接 `GetClassType(currentObj->klass)->isValueType()` ——
    // 判空检查（if (isLast && currentObj && ...)）排在它**后面**一行，
    // 也就是说检查根本没起到保护作用。下面的循环体里还有五六处
    // currentObj->klass / currentObj 的裸解引用。
    if (currentObj == nullptr)
    {
        ImGui::TextDisabled("对象已失效（可能已被 GC 回收），请重新 Inspect");
        return;
    }

    for (auto it = paths.begin() + (paths.size() > 3 ? paths.size() - 4 : 0); it != paths.end(); ++it)
    {
        const bool isLast = std::next(it) == paths.end();
        auto key = it->c_str();
        ImGui::PushID(indentCounter);

        bool buttonPressed = false;
        // GetClassType 同样可能返回空（元数据被裁剪时）。
        auto *currentType = Il2cpp::GetClassType(currentObj->klass);
        bool isValueType = currentType != nullptr && currentType->isValueType();
        // 关注按钮只给**叶子上的原始类型**。结构体/数组 dump 出来是
        // 一个对象，盯不出「值变了」—— 给个只会一直显示「<非标量>」的按钮
        // 比不给更糟。
        bool canWatch = isLast && currentType != nullptr && currentType->isPrimitive() &&
                        currentObj != nullptr;
        if (isLast && isValueType)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(222, 222, 222, 255));
        }
        if (isLast && currentObj && savedSet[currentObj->klass].count(currentObj) == 0)
        {
            // 三个按钮的宽度要先算出来，不能让最后一个「吃掉剩余宽度」——
            // 那样每加一个按钮都要改一遍布局算式。
            const ImGuiStyle &style = ImGui::GetStyle();
            const float wSave = ImGui::CalcTextSize("Save").x + style.FramePadding.x * 5.f;
            const float wWatch = canWatch ? ImGui::CalcTextSize("W").x + style.FramePadding.x * 3.f : 0.f;
            // 两个按钮的宽度都是固定像素，窄屏上加起来会超过可用宽度 ——
            // 减出来是负数，ImGui 会画出一个退化的、点不中的矩形。
            buttonPressed = ImGui::Button(key, ImVec2(AvailMinus(wSave + wWatch), 0));
            if (ImGui::IsItemHeld())
            {
                Tool::OpenNewTabFromClass(currentObj->klass);
                LOGD("OpenNewTabFromObject %p", currentObj);
            }
            ImGui::SameLine();
            char buttonLabel[32]{0};
            sprintf(buttonLabel, "Save");
            if (ImGui::Button(buttonLabel, ImVec2(wSave, 0)))
            {
                savedSet[currentObj->klass].insert(currentObj);
                SaveObjectWithRoot(currentObj);
                RecordFieldChange(currentObj, "(整个对象)", "保存", "已加入 GC 强根");
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%p", currentObj);
            }
            if (canWatch)
            {
                ImGui::SameLine();
                if (ImGui::Button("W", ImVec2(wWatch, 0)))
                {
                    const size_t depth = static_cast<size_t>(it - paths.begin()) + 1;
                    std::vector<std::string> watchPaths(dataMap[rootObj].second.begin(),
                                                       dataMap[rootObj].second.begin() + depth);
                    // 标签用「类名.最后两级路径」：只用叶子名（比如 "health"）
                    // 在关注列表里根本认不出是哪个对象的 —— 同一个类里
                    // 常常有好几个 health。也不适合用完整路径（可能十几级，
                    // 文本会溢出面板）。
                    std::string label =
                        (rootObj->klass && rootObj->klass->getName()) ? rootObj->klass->getName() : "?";
                    const size_t from = depth > 2 ? depth - 2 : 0;
                    for (size_t k = from; k < depth; k++)
                    {
                        label += ".";
                        label += watchPaths[k];
                    }
                    AddWatch(rootObj, watchPaths, label);
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("关注这个值：之后每 200ms 自动刷新，变了会变绿");
                }
            }
        }
        else
        {
            buttonPressed =
                ImGui::Button(key, ImVec2(AvailMinus(ImGui::GetStyle().FramePadding.x), 0));
            if (ImGui::IsItemHeld())
            {
                Tool::OpenNewTabFromClass(currentObj->klass);
                LOGD("OpenNewTabFromObject %p", currentObj);
            }
        }
        if (isLast && isValueType)
        {
            ImGui::PopStyleColor();
        }
        ImGui::Indent(10.f);
        indentCounter++;
        ImGui::PopID();

        if (buttonPressed)
        {
            paths.erase(it, paths.end());
            dataMap[rootObj].first = rootObj->dump(paths);
            break;
        }
    }
    if (indentCounter)
        ImGui::Unindent(10.f * indentCounter);

    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(100, 200, 20, 128));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(100, 200, 20, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(100, 200, 20, 255));
    // doRefresh 是**跨帧**标志：下面的 JSON 表格里，字段编辑的回调
    // （软键盘确认后写回字段、枚举选择等）会请求刷新，意思是「下一帧
    // 重新 dump 一次，把新值刷到界面上」。请求按对象分别记录。
    if (ImGui::Button("Refresh", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)))
    {
        RequestRefresh(rootObj);
    }
    if (ConsumeRefresh(rootObj))
    {
        dataMap[rootObj].first = rootObj->dump(paths);
        // 重新 dump 之后 currentObj 可能变了（甚至变成 nullptr ——
        // 路径指向的对象在这一瞬间被销毁了）。下一帧会重新取，
        // 但这一帧后面的代码还在用它，这里显式同步。
        currentObj = dataMap[rootObj].first.first;
        if (currentObj == nullptr)
        {
            ImGui::PopStyleColor(3);
            ImGui::TextDisabled("刷新时对象已失效，请重新 Inspect");
            return;
        }
    }
    ImGui::PopStyleColor(3);

    ImGui::PushStyleColor(ImGuiCol_Separator, IM_COL32(0, 255, 100, 255));
    ImGui::Separator();
    ImGui::PopStyleColor();
    static PopUpSelector poper;
    if (ImGui::BeginTable("sometable", 1))
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::BeginChild("ChildJson",
                          ImVec2(0, ImGui::GetContentRegionAvail().y - (ImGui::GetFontSize() * 1.8f) * 2.f), 0,
                          ImGuiWindowFlags_HorizontalScrollbar);

        const nlohmann::ordered_json &current = getJsonObject(rootObj);
        if (current.empty())
        {
            ImGui::Text("Empty");
        }

        for (auto &[key, value] : current.items())
        {
            // int count = 0;
            if (value.is_object() || value.is_array())
            {
                if (value.is_array() && value.size() == 0)
                {
                    ImGui::Text("%s = [Empty]", key.c_str());
                }
                else if (ImGui::Button(key.c_str(), ImVec2(key.length() <= 3
                                                                ? AvailMinus(ImGui::GetStyle().FramePadding.x)
                                                                : 0,
                                                            0)))
                {
                    try
                    {
                        paths.push_back(key);
                        // LOGD("%s", object->dump(paths).dump().c_str());
                        dataMap[rootObj].first = rootObj->dump(paths);
                        break;
                    }
                    catch (nlohmann::json::exception &e)
                    {
                        LOGE("Json exception %s", e.what());
                    }
                    catch (std::exception &e)
                    {
                        LOGE("Exception %s", e.what());
                    }
                }
            }
            else if (value.is_string())
            {
                auto text = value.get<std::string>();
                ImGui::Text("%s = %s", key.c_str(), text.c_str());
                if (ImGui::IsItemClicked())
                {
                    std::istringstream iss(key);
                    std::string type, val;
                    iss >> type >> val;
                    if (strcmp(type.c_str(), "String") == 0)
                    {
                        Keyboard::Open(
                            text.c_str(),
                            [type = std::move(type), val = std::move(val), currentObj,
                             rootObj](const std::string &value)
                            {
                                LOGD("%s", value.c_str());
                                auto f = currentObj ? currentObj->klass->getFieldInHierarchy(val.c_str()) : nullptr;
                                if (!f)
                                {
                                    LOGE("找不到字段 %s", val.c_str());
                                    return;
                                }
                                auto newStr = Il2cpp::NewString(value.c_str());
                                // il2cpp_field_set_value(obj, field, ptr) 是「从 ptr 指向的
                                // 地址拷贝 field 长度的那几个字节」，所以必须传 &newStr。
                                // 旧代码直接传 newStr（托管对象本身），等于把对象头
                                // （klass 指针 + monitor）当字段内容写进去，字段直接损坏。
                                // 引用类型字段也可以用 SetFieldValueObject 明确表达意图。
                                Il2cpp::SetFieldValue(currentObj, f, &newStr);
                                RecordFieldChange(rootObj, val, "字符串", value);
                                RequestRefresh(rootObj);
                            });
                    }
                    else
                    {
                        auto field = currentObj ? currentObj->klass->getFieldInHierarchy(val.c_str()) : nullptr;
                        if (!field)
                        {
                            LOGE("找不到字段 %s", val.c_str());
                        }
                        else
                        {
                            auto fieldType = field->getType();
                            if (fieldType && fieldType->isEnum())
                            {
                                poper.Open(
                                    "EnumSelector",
                                    [fieldType, currentObj, field, rootObj](const std::string &result)
                                    {
                                        auto *enumClass = fieldType->getClass();
                                        auto *enumField = enumClass ? enumClass->getField(result.c_str()) : nullptr;
                                        if (!enumField)
                                        {
                                            LOGE("枚举 %s 找不到成员 %s",
                                                 fieldType->getName() ? fieldType->getName() : "?", result.c_str());
                                            return;
                                        }
                                        // 读写两侧都必须按真实底层宽度。
                                        // 写侧同样危险：il2cpp 从源地址读
                                        // field 长度的那几个字节，4 字节的 int
                                        // 会被读成 8 字节，把栈上的相邻变量也读进去。
                                        auto raw = FieldInfo::getEnumStaticValue(enumField);
                                        auto *baseType = Il2cpp::GetEnumBaseType(enumClass);
                                        const char *baseName = baseType ? Il2cpp::GetTypeName(baseType) : nullptr;

                                        if (baseName && (strcmp(baseName, "System.Int64") == 0 ||
                                                         strcmp(baseName, "System.UInt64") == 0))
                                        {
                                            int64_t wide = raw;
                                            Il2cpp::SetFieldValue(currentObj, field, &wide);
                                        }
                                        else
                                        {
                                            int32_t narrow = static_cast<int32_t>(raw);
                                            Il2cpp::SetFieldValue(currentObj, field, &narrow);
                                        }
                                        RecordFieldChange(rootObj, field && field->getName() ? field->getName() : "?", "枚举", result);
                                        RequestRefresh(rootObj);
                                    },
                                    fieldType);
                            }
                        }
                    }
                }
            }
            else if (value.is_boolean())
            {
                ImGui::Text("%s = %s", key.c_str(), value.get<bool>() ? "True" : "False");
                if (ImGui::IsItemClicked())
                {
                    std::istringstream iss(key);
                    std::string _, val;
                    iss >> _ >> val;
                    poper.Open("BooleanSelector",
                               [currentObj, val, paths, rootObj](const std::string &value)
                               {
                                   bool b = value == "True";
                                   // split key by space
                                   currentObj->setField(val.c_str(), (int)b);
                                   RecordFieldChange(rootObj, val, "布尔", value);
                                   ensureIfValueType(currentObj, paths, rootObj);
                                   RequestRefresh(rootObj);
                               });
                }
            }
            else if (value.is_number_float())
            {
                {
                    std::istringstream iss(key);
                    std::string type, val;
                    iss >> type >> val;
                    // 显示也走类型正确的格式化，否则 Double 字段会被
                    // 当成 float 印出来（0.123457），用户完全看不出被截断了。
                    ImGui::Text("%s = %s", key.c_str(), FormatFieldForEdit(type, value).c_str());
                }

                if (ImGui::IsItemClicked())
                {
                    std::istringstream iss(key);
                    std::string type, val;
                    iss >> type >> val;
                    // 预填值同样必须按声明类型格式化：预填对了，
                    // 「原样点确认」才是无损的。
                    Keyboard::Open(FormatFieldForEdit(type, value).c_str(),
                                   [type, currentObj, val, paths, rootObj](const std::string &text)
                                   {
                                       if (currentObj == nullptr)
                                       {
                                           return;
                                       }
                                       if (ParseAndSetNumericField(currentObj, type, val, text))
                                       {
                                           RecordFieldChange(rootObj, val, type, text);
                                           ensureIfValueType(currentObj, paths, rootObj);
                                           RequestRefresh(rootObj);
                                       }
                                       else
                                       {
                                           // 旧代码失败时什么都不做 —— 用户填了值、点了确认，然后没有任何反应。
                                           // 他无法判断是「写不进去」还是「工具没收到」，只会反复重试。
                                           ReportFieldError(std::string(val) + ": 无法解析或超出 " + type +
                                                                    " 的取值范围（输入: " + text + "）");
                                       }
                                   });
                }
            }
            else if (value.is_number())
            {
                {
                    std::istringstream iss(key);
                    std::string type, val;
                    iss >> type >> val;
                    ImGui::Text("%s = %s", key.c_str(), FormatFieldForEdit(type, value).c_str());
                }
                if (ImGui::IsItemClicked())
                {
                    std::istringstream iss(key);
                    std::string type, val;
                    iss >> type >> val;
                    Keyboard::Open(FormatFieldForEdit(type, value).c_str(),
                                   [type, currentObj, val, paths, rootObj](const std::string &text)
                                   {
                                       if (currentObj == nullptr)
                                       {
                                           return;
                                       }
                                       if (ParseAndSetNumericField(currentObj, type, val, text))
                                       {
                                           RecordFieldChange(rootObj, val, type, text);
                                           ensureIfValueType(currentObj, paths, rootObj);
                                           RequestRefresh(rootObj);
                                       }
                                       else
                                       {
                                           // 旧代码失败时什么都不做 —— 用户填了值、点了确认，然后没有任何反应。
                                           // 他无法判断是「写不进去」还是「工具没收到」，只会反复重试。
                                           ReportFieldError(std::string(val) + ": 无法解析或超出 " + type +
                                                                    " 的取值范围（输入: " + text + "）");
                                       }
                                   });
                }
            }
            else
            {
                ImGui::Text("Unk %s %s", key.c_str(), value.type_name());
            }
            // constexpr ImU32 colors[8] = {
            //     IM_COL32(255,50,50,255),     IM_COL32(0, 255, 0, 255),   IM_COL32(0, 0, 255, 255),
            //     IM_COL32(255, 255, 0, 255),   IM_COL32(255, 0, 255, 255), IM_COL32(0, 255, 255, 255),
            //     IM_COL32(255, 255, 255, 255), IM_COL32(0, 0, 0, 255),
            // };
            // ImGui::PushStyleColor(ImGuiCol_Separator, colors[paths.size() % 8]);
            ImGui::Separator();
            // ImGui::PopStyleColor();
        }

        ImGui::ScrollWhenDraggingOnVoid();
        ImGui::EndChild();

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::BeginChild("bottom");
        if (ImGui::Button("Methods", ImVec2(AvailMinus(ImGui::GetStyle().FramePadding.x), 0)))
        {
            ImGui::OpenPopup("MethodPopup");
        }

        static ImGuiIO &io = ImGui::GetIO();
        ImGui::SetNextWindowSizeConstraints(ImVec2(io.DisplaySize.x / 1.2f, 0),
                                            ImVec2(io.DisplaySize.x / 1.2f, io.DisplaySize.y / 2));
        if (ImGui::BeginPopup("MethodPopup", ImGuiWindowFlags_HorizontalScrollbar))
        {
            auto text = currentObj->klass->getName();
            auto windowWidth = ImGui::GetWindowSize().x;
            auto textWidth = ImGui::CalcTextSize(text).x;
            ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5f);
            ImGui::Text("%s", text);

            ImGui::Separator();

            auto &methods = buildMethodMap(currentObj->klass);
            if (methods.empty())
            {
                ImGui::Text("No methods for class %s",
                            currentObj->klass && currentObj->klass->getName() ? currentObj->klass->getName() : "?");
            }
            else
            {
                for (auto &[method, paramsInfo] : methods)
                {
                    // ID 用方法指针本身。旧代码写的是 `method + j++` ——
                    // 那是对 MethodInfo* 做指针算术（越界指针，UB），而且
                    // 叠加一个随遍历变化的偏移：只要方法顺序有任何变化，
                    // 每个方法的 ImGui ID 就跟着变，展开状态/滚动位置全部错位。
                    // 方法指针本身是稳定的元数据地址，天然适合当 ID。
                    ImGui::PushID(static_cast<const void *>(method));
                    MethodViewer(currentObj->klass, method, paramsInfo, currentObj, true);
                    ImGui::Separator();
                    ImGui::PopID();
                }
            }
            ImGui::EndPopup();
        }

        if (ImGui::Button("Dump to file",
                          ImVec2(AvailMinus(ImGui::GetStyle().FramePadding.x), 0)))
        {
            ImGui::OpenPopup("ProceedPopUp");
        }
        if (ImGui::BeginPopup("ProceedPopUp"))
        {
            char fileName[256]{0};
            snprintf(fileName, sizeof(fileName), "dump_%s (%p).json", currentObj->klass->getName(), currentObj);
            ImGui::Text("File will be saved as %s", fileName);
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 50, 255));
            ImGui::Text("Note: This may take a while depending on the size of the object");
            ImGui::Text("Do not touch the screen if it's freezing!");
            ImGui::PopStyleColor();
            if (ImGui::Button("Proceed", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
            {
                ChangeMaxListArraySize(9999,
                                       [&object = currentObj]()
                                       {
                                           char fileName[256]{0};
                                           snprintf(fileName, sizeof(fileName), "dump_%s (%p).json",
                                                    object->klass->getName(), object);
                                           Util::FileWriter file(fileName);
                                           std::vector<uintptr_t> visited{};
                                           nlohmann::ordered_json j = object->dump(visited, 9999);
                                           file.write(j.dump(2, ' ').c_str());
                                           LOGD("Done save");
                                           ImGui::CloseCurrentPopup();
                                       });
            }
            ImGui::EndPopup();
        }
        // 关注列表**不在**这里画 —— 见 Tool::Draw 里的说明：
        // ImGuiJson 只在有打开的对象 tab 时才被调用，放在这里会让
        // 「把对象 tab 全关掉」直接让关注列表消失并停止刷新。
        poper.Update();
        ImGui::EndChild();
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------------------
// 后台筛选
//
// 为什么必须放到后台：一次筛选要做
//   1. image->getClasses()      —— 遍历该 assembly 的全部类
//   2. 每个类的 getMethods()    —— 分配一个 vector
//   3. 每个方法的 getParamsInfo()—— 再分配一个 vector
// 那是「所有 assembly × 所有类 × 所有方法」量级的元数据遍历。
// 旧代码在渲染线程上同步做完这一切，而触发时机是「每敲一个字符」——
// 用户每输入一个字母，游戏就卡一下。
//
// 设计要点
// - 请求按值传递，worker 不持有 tab 指针。tab 在请求处理期间被销毁也不会
//   碰到野指针（结果写进 shared_ptr 持有的 FilterState）。
// - 只保留**最新**请求：连续快速输入时旧请求直接被覆盖，不会排出一长串
//   已经过期的筛选任务。
// - 结果用代号（generation）标记，渲染线程只认领与当前请求代号一致的结果，
//   避免把过期结果画到界面上。
// ---------------------------------------------------------------------------

struct ClassesTab::FilterState
{
    std::mutex mutex;
    std::condition_variable cv;

    bool hasWork = false;
    bool shutdown = false;
    uint64_t requestGen = 0;

    // 请求（按值）
    std::string filter;
    bool caseSensitive = true;
    bool filterByClass = true;
    bool filterByMethod = false;
    bool filterByField = false;
    bool showAllClasses = false;
    Il2CppImage *selectedImage = nullptr;
    bool includeAllImages = false;
    std::vector<Il2CppImage *> images;

    // 结果
    bool hasResult = false;
    uint64_t resultGen = 0;
    // 本轮失败的原因；空 = 成功。
    //
    // 存在的意义是**失败也必须有出口**：attach 失败或抛异常时，
    // 如果直接 return 而不走到统一发布结果那段，hasResult 永远是 false、
    // resultGen 永远对不上 requestGen → IsFilterPending() 恒为 true
    // → 标签页**永久**显示「筛选中…」，列表永远是空的，界面上没有任何解释。
    const char *failure = nullptr;
    std::vector<Il2CppClass *> classes;
    std::vector<Il2CppClass *> filteredClasses;
    ClassesTab::ClassMethodMap methodMap;
};

namespace
{
    // worker 线程读写的「待办」状态集合。key 是 FilterState 的裸指针
    // （仅用于排序，实际数据通过 shared_ptr 访问）。
    std::mutex g_queueMutex;
    std::condition_variable g_queueCv;
    std::vector<std::weak_ptr<ClassesTab::FilterState>> g_queue;
    std::thread g_worker;
    bool g_workerRunning = false;

    // 在后台线程上执行真正耗时的筛选。
    //
    // 这些全是 il2cpp 调用（getClasses / getMethods / getParamsInfo），
    // 必须在已挂载的线程上跑 —— 后台线程是 foreign thread，不 attach 就是
    // 崩溃或静默错数据。用完必须 detach，否则 attached-thread 表里会留下
    // 已退出线程的悬空条目，GC 遍历线程表时踩到。
    struct FilterAttachGuard
    {
        bool attached = false;
        FilterAttachGuard()
        {
            attached = Il2cpp::EnsureAttached();
        }
        ~FilterAttachGuard()
        {
            if (attached)
            {
                Il2cpp::Detach();
            }
        }
    };

    void DoFilterWork(const std::shared_ptr<ClassesTab::FilterState> &st)
    {
        std::vector<Il2CppClass *> allClasses;
        std::vector<Il2CppClass *> filtered;
        ClassesTab::ClassMethodMap methodMap;
        // 失败原因。空 = 成功。非空 = 这一轮没能完成。
        const char *failure = nullptr;

        try
        {
            FilterAttachGuard attachGuard;
            if (!attachGuard.attached)
            {
                LOGE("后台筛选: 无法 attach 到 il2cpp VM");
                failure = "无法 attach 到 il2cpp VM（游戏可能仍在加载，或该线程未被支持）";
                // 注意：**不能直接 return**。
                // 下面统一发布结果的代码块才是「这一轮有结论了」的出口；
                // 从这里 return 的话 hasResult 永远为 false、resultGen 永远
                // 对不上 requestGen，于是 IsFilterPending() 恒为 true ——
                // 标签页会**永久**显示「筛选中…」，列表永远是空的，
                // 而界面上除了那行「筛选中」没有任何解释。
            }
            else
            {
            if (st->includeAllImages)
            {
                for (auto image : st->images)
                {
                    if (image == nullptr)
                    {
                        continue;
                    }
                    auto imageClasses = image->getClasses();
                    allClasses.insert(allClasses.end(), imageClasses.begin(), imageClasses.end());
                }
            }
            else if (st->selectedImage != nullptr)
            {
                allClasses = st->selectedImage->getClasses();
            }

            // 大小写不敏感包含匹配。
            //
            // 旧实现是 `auto newA = a; auto newB = b; transform(tolower); find`
            // —— 每比较一次就分配两个完整副本。这个比较跑在
            // 「每个类 × 每个方法 × 每个字段」上：5000 个类、平均 20 个方法
            // 就是十万次比较、二十万次堆分配。现在只转换**模式串**
            // （长度固定，几百字节）并预先算好，遍历 haystack 时逐字符
            // tolower 比较，全程零分配。
            std::string loweredFilter;
            if (!st->caseSensitive && !st->filter.empty())
            {
                loweredFilter.resize(st->filter.size());
                std::transform(st->filter.begin(), st->filter.end(), loweredFilter.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            }

            auto finder = [&](const char *haystack) -> bool
            {
                if (haystack == nullptr)
                {
                    return false;
                }
                if (st->caseSensitive)
                {
                    return std::strstr(haystack, st->filter.c_str()) != nullptr;
                }
                if (loweredFilter.empty())
                {
                    return true;
                }
                const size_t n = loweredFilter.size();
                for (const char *p = haystack; *p != '\0'; ++p)
                {
                    size_t i = 0;
                    while (i < n && p[i] != '\0' &&
                           static_cast<char>(std::tolower(static_cast<unsigned char>(p[i]))) == loweredFilter[i])
                    {
                        ++i;
                    }
                    if (i == n)
                    {
                        return true;
                    }
                }
                return false;
            };

            // 三个搜索范围是「或」关系：类名/方法名/字段名命中任意一个都算。
            // 都不勾时回退到只按类名筛（UI 上也会兜底，这里再兜一层，
            // 因为 ConfigSave/Load 能从旧配置里读出三者皆 false 的状态）。
            const bool searchClass = st->filterByClass || (!st->filterByMethod && !st->filterByField);
            const bool searchMethod = st->filterByMethod;
            const bool searchField = st->filterByField;

            const size_t limit = st->showAllClasses ? allClasses.size() : (size_t)MAX_CLASSES;

            for (size_t i = 0; i < allClasses.size() && filtered.size() < limit; i++)
            {
                auto klass = allClasses[i];
                if (klass == nullptr)
                {
                    continue;
                }
                if (Il2cpp::GetClassIsEnum(klass))
                {
                    continue;
                }

                bool found = false;
                // 命中的方法先攒着，确认命中后再填 methodMap ——
                // 否则没命中的类也白填一遍（那才是最贵的部分）。
                std::vector<MethodInfo *> matchedMethods;
                // 类名**本身**是否命中。只有靠方法名/字段名才捞出来的类，
                // 才应该只展示相关方法；类名命中的类是「整类都相关」，
                // 应当展示全部方法。
                bool classNameMatched = false;

                if (searchClass && finder(klass->getFullName().c_str()))
                {
                    found = true;
                    classNameMatched = true;
                }
                if (searchMethod)
                {
                    for (auto m : klass->getMethods())
                    {
                        if (finder(m->getName()))
                        {
                            found = true;
                            matchedMethods.push_back(m);
                        }
                    }
                }
                if (searchField && !found)
                {
                    for (auto f : klass->getFields())
                    {
                        if (finder(f->getName()))
                        {
                            found = true;
                            break;
                        }
                    }
                }
                if (!found)
                {
                    continue;
                }

                filtered.push_back(klass);

                // 什么时候只列命中的方法？
                //
                // 仅当这个类**是靠方法名捞出来的**（类名本身没命中）。
                // 那种情况下「按方法搜索」的语义成立：展开它只该看到相关方法。
                //
                // 而如果类名本身就命中了，整个类都是相关的 —— 哪怕恰好有
                // 几个方法名也含这个关键字，也应该展示**全部**方法。
                // 旧实现没区分这两种情况，于是「同时勾选类名+方法名」时，
                // 用户明明是奔着这个类去的，展开却只看到零星几个方法。
                const bool narrowToMatches = !matchedMethods.empty() && !classNameMatched;
                if (narrowToMatches)
                {
                    for (auto m : matchedMethods)
                    {
                        methodMap[klass].push_back({m, m->getParamsInfo()});
                    }
                }
                else
                {
                    for (auto m : klass->getMethods())
                    {
                        methodMap[klass].push_back({m, m->getParamsInfo()});
                    }
                }
            } // ← 关闭遍历类的 for
            }   // ← 关闭 `else`（attach 成功分支）
        }       // ← 关闭 try
        catch (const std::exception &e)
        {
            LOGE("后台筛选异常: %s", e.what());
            failure = "筛选时发生异常";
        }
        catch (...)
        {
            LOGE("后台筛选未知异常");
            failure = "筛选时发生未知异常";
        }

        // 无论成功还是失败，这里都是「这一轮有结论了」的出口。
        // 失败时**照样**发布结果（空的 + 带失败原因），这样 UI 不会永远
        // 卡在「筛选中…」，而是明确显示失败。
        std::lock_guard guard(st->mutex);        st->classes = std::move(allClasses);
        st->filteredClasses = std::move(filtered);
        st->methodMap = std::move(methodMap);
        st->resultGen = st->requestGen;
        st->failure = failure;
        st->hasResult = true;
    }

    void WorkerLoop()
    {
        for (;;)
        {
            std::shared_ptr<ClassesTab::FilterState> job;
            {
                std::unique_lock lock(g_queueMutex);
                g_queueCv.wait(lock, []
                                { return g_workerRunning == false || !g_queue.empty(); });
                if (!g_workerRunning && g_queue.empty())
                {
                    return;
                }
                // 取最后一个：连续快速输入时，中间那些请求已经过期了。
                // 保留它们只会让用户多等 —— 界面反正只显示最新结果。
                auto &last = g_queue.back();
                job = last.lock();
                g_queue.clear();
            }
            if (!job)
            {
                // tab 已经销毁，shared_ptr 过期。直接跳过。
                continue;
            }
            DoFilterWork(job);
        }
    }
} // namespace

void ClassesTabWorker::EnsureStarted()
{
    std::lock_guard guard(g_queueMutex);
    if (g_workerRunning)
    {
        return;
    }
    if (g_worker.joinable())
    {
        g_worker.join();
    }
    // **必须 try/catch**：std::thread 的构造函数在线程创建失败时抛异常，
    // 裸写就是 std::terminate → 整个游戏进程崩掉。而且这里在 init 路径上，
    // 抛出去会一路穿过 ImGui 初始化。
    try
    {
        g_workerRunning = true;
        g_worker = std::thread(WorkerLoop);
        LOGI("类筛选工作线程已启动");
    }
    catch (const std::exception &e)
    {
        // 线程没起来：g_workerRunning 必须复位，否则 Shutdown 会去 join
        // 一个根本没启动的线程。
        g_workerRunning = false;
        LOGE("类筛选工作线程启动失败: %s（搜索将退回同步模式，可能略有卡顿）", e.what());
    }
}

void ClassesTabWorker::Shutdown()
{
    std::thread worker;
    {
        std::lock_guard guard(g_queueMutex);
        g_workerRunning = false;
        g_queue.clear();
        worker = std::move(g_worker);
    }
    g_queueCv.notify_all();
    if (worker.joinable())
    {
        // 必须 join：全局 std::thread 析构时若仍 joinable 就是 std::terminate，
        // 而这是注入进别人游戏的库 —— 触发它等于让用户的游戏莫名崩掉。
        worker.join();
        LOGI("类筛选工作线程已停止");
    }
}

void ClassesTab::FilterClasses(const std::string &filterArg)
{
    if (!filterState)
    {
        filterState = std::make_shared<FilterState>();
    }
    {
        std::lock_guard guard(filterState->mutex);
        filterState->filter = filterArg;
        filterState->caseSensitive = caseSensitive;
        filterState->filterByClass = filterByClass;
        filterState->filterByMethod = filterByMethod;
        filterState->filterByField = filterByField;
        filterState->showAllClasses = showAllClasses;
        filterState->selectedImage = selectedImage;
        filterState->includeAllImages = includeAllImages;
        filterState->images = g_Images;
        // 先清结果标志：新请求还没算完，此刻 UI 不该继续显示旧结果
        // （那会让用户看到和输入框内容不匹配的列表）。
        filterState->hasResult = false;
        filterState->requestGen++;
    }
    {
        std::lock_guard guard(g_queueMutex);
        g_queue.push_back(filterState);
        const bool workerUp = g_workerRunning;
        if (!workerUp)
        {
            // 请求入队后立刻清掉 —— 下面要改成同步做，队列里留着它
            // 会让下一次 EnsureStarted 起来后重复处理一遍。
            g_queue.clear();
        }
        g_queueCv.notify_one();

        if (!workerUp)
        {
            // **工作线程没起来 → 在当前（渲染）线程上同步做。**
            //
            // 旧代码这里只入队就走。如果 EnsureStarted 失败（线程资源耗尽），
            // 就**没有任何人会去取这个队列**：
            //   hasResult 永远是 false、resultGen 永远对不上 requestGen
            //   → IsFilterPending() 恒为 true
            //   → 标签页**永久**显示「筛选中…」，列表永远是空的。
            //
            // 也就是第 31 轮修的那个「失败无出口」的 bug，从「线程没起来」
            // 这扇门又回来了。
            //
            // 同步跑会阻塞渲染线程（这就是日志里「可能略有卡顿」的由来），
            // 但功能可用**远好过**永久卡住 —— 而且 DoFilterWork 里
            // EnsureAttached 对已在 VM 上的渲染线程是幂等的。
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                LOGW("筛选工作线程未运行，搜索改为**在渲染线程同步执行**（可能卡顿）");
            }
            DoFilterWork(filterState);
        }
    }
}

bool ClassesTab::IsFilterPending()
{
    if (!filterState)
    {
        return false;
    }
    std::lock_guard guard(filterState->mutex);
    return !filterState->hasResult && filterState->resultGen != filterState->requestGen;
}

const char *ClassesTab::GetFilterFailure()
{
    if (!filterState)
    {
        return nullptr;
    }
    std::lock_guard guard(filterState->mutex);
    return filterState->failure;
}

bool ClassesTab::PollFilterResult()
{
    if (!filterState)
    {
        return false;
    }
    std::vector<Il2CppClass *> newClasses;
    std::vector<Il2CppClass *> newFiltered;
    ClassMethodMap newMethodMap;
    {
        std::lock_guard guard(filterState->mutex);
        // 只认领与**当前请求**同代的结果：算得再快也没用，
        // 代号对不上就说明用户已经又改过条件了。
        if (!filterState->hasResult || filterState->resultGen != filterState->requestGen)
        {
            return false;
        }
        newClasses = std::move(filterState->classes);
        newFiltered = std::move(filterState->filteredClasses);
        newMethodMap = std::move(filterState->methodMap);
        filterState->hasResult = false;
        filterState->classes.clear();
        filterState->filteredClasses.clear();
        filterState->methodMap.clear();
    }
    classes = std::move(newClasses);
    filteredClasses = std::move(newFiltered);
    methodMap = std::move(newMethodMap);
    // 持久化放在「认领结果」这里而不是每次按键：
    // 旧实现每敲一个字符就写一次配置文件，纯属无谓的 IO。
    Tool::ConfigSave();
    return true;
}

// 方法的稳定签名：类型全名 + 方法名 + 参数个数。
// MethodInfo* 每次运行都不一样，只有签名能跨启动定位。
// （to_json 在下面、定义在上面，所以需要先声明。）
static std::string MethodSignature(MethodInfo *method);

void to_json(nlohmann::ordered_json &j, const ClassesTab &p)
{
    j["filter"] = p.filter;
    j["filterByClass"] = p.filterByClass;
    j["filterByField"] = p.filterByField;
    j["filterByMethod"] = p.filterByMethod;
    j["showAllClasses"] = p.showAllClasses;
    j["includeAllImages"] = p.includeAllImages;
    j["caseSensitive"] = p.caseSensitive;
    j["selectedImage"] = p.selectedImage->getName();

    // 参数预设。
    //
    // **只写文本**，绝不写 ParamValue::object —— 那是托管对象指针，
    // 反序列化时它早就失效了，写进去等于埋一个 use-after-free。
    //
    // 用 method 签名（类型全名 + 名字 + 参数个数）而不是 MethodInfo* 当 key：
    // 指针在下次启动后没有意义，签名才有。
    nlohmann::ordered_json presetsJson = nlohmann::ordered_json::object();
    for (const auto &[method, list] : p.methodPresets)
    {
        if (list.empty() || method == nullptr)
        {
            continue;
        }
        nlohmann::ordered_json oneMethod = nlohmann::ordered_json::array();
        for (const auto &preset : list)
        {
            nlohmann::ordered_json entry;
            entry["name"] = preset.name;
            entry["values"] = preset.values; // map<string,string>，安全
            oneMethod.push_back(std::move(entry));
        }
        presetsJson[MethodSignature(method)] = std::move(oneMethod);
    }
    j["methodPresets"] = std::move(presetsJson);
}

// 方法的稳定签名：类型全名 + 方法名 + 参数个数。
// MethodInfo* 每次运行都不一样，只有签名能跨启动定位。
static std::string MethodSignature(MethodInfo *method)
{
    if (method == nullptr)
    {
        return {};
    }
    char buf[320]{0};
    const char *owner = method->getClass() ? method->getClass()->getFullName().c_str() : "?";
    // getParamsInfo() 会分配一个 vector（每个参数一个 pair）。
    // 预设数量通常很小，但 ConfigSave 每次存预设都会调到，
    // 所以这里没有额外开销可言。
    snprintf(buf, sizeof(buf), "%s::%s/%zu", owner, method->getName() ? method->getName() : "?", method->getParamsInfo().size());
    return buf;
}

// 按签名反查方法。读配置时用。
//
// **一次性建索引**，不是每个预设扫一遍。
//
// 旧实现是每查一个签名就把「所有 assembly × 所有类 × 所有方法」
// 走一遍（几千个类、几万到十万个方法）。而 from_json 是在配置循环里
// 对**每个预设**调一次的 —— 有 5 个预设就是 5 遍全量扫描。
//
// 而 from_json 跑在 Tool::ConfigLoad → Tool::Init → on_init 里，也就是
// **渲染线程上、同步**。于是「保存过预设」会让启动明显卡一下 ——
// 那正是我前面刚修掉的启动卡顿，这里又引入回来。
//
// 改成建一次「签名 → 方法」索引，之后都是 O(1) 查表。
static MethodInfo *FindMethodBySignature(const std::string &signature)
{
    const size_t sep = signature.rfind('/');
    const size_t colon = signature.rfind("::");
    if (sep == std::string::npos || colon == std::string::npos || colon > sep)
    {
        return nullptr;
    }
    const std::string className = signature.substr(0, colon);
    const std::string methodName = signature.substr(colon + 2, sep - colon - 2);
    const std::string argCount = signature.substr(sep + 1);

    // 先按类名筛，能避免把绝大多数类的方法都枚举一遍。
    for (auto image : g_Images)
    {
        if (image == nullptr)
        {
            continue;
        }
        for (auto klass : image->getClasses())
        {
            if (klass == nullptr || klass->getFullName() != className)
            {
                continue;
            }
            for (auto m : klass->getMethods())
            {
                if (m != nullptr && m->getName() == methodName &&
                    std::to_string(m->getParamsInfo().size()) == argCount)
                {
                    return m;
                }
            }
            return nullptr; // 类名唯一，类找得到但方法没匹配上就不用再找别的 image
        }
    }
    return nullptr;
}

void from_json(const nlohmann::ordered_json &j, ClassesTab &p)
{
    j.at("filter").get_to(p.filter);
    j.at("filterByClass").get_to(p.filterByClass);
    j.at("filterByField").get_to(p.filterByField);
    j.at("filterByMethod").get_to(p.filterByMethod);
    j.at("showAllClasses").get_to(p.showAllClasses);
    j.at("includeAllImages").get_to(p.includeAllImages);
    j.at("caseSensitive").get_to(p.caseSensitive);

    // 参数预设（可选字段，旧配置文件里没有 —— 用 find 而不是 at，
    // at 找不到会抛 out_of_range，而 from_json 是在配置加载路径上调用的）。
    if (auto it = j.find("methodPresets"); it != j.end() && it->is_object())
    {
        // 先把要还原的签名收集起来，**一次性**建索引，
        // 然后每个预设都变成 O(1) 查表。
        //
        // 否则就是「每个预设扫一遍全部类」× N，在渲染线程上同步跑 ——
        // 几万个类乘以预设数，启动会明显卡住。
        std::unordered_map<std::string, MethodInfo *> index;
        for (const auto &[signature, list] : it->items())
        {
            if (!list.is_array())
            {
                continue;
            }
            if (index.find(signature) != index.end())
            {
                continue; // 同一个签名只查一次
            }
            index[signature] = FindMethodBySignature(signature);
        }

        for (const auto &[signature, list] : it->items())
        {
            if (!list.is_array())
            {
                continue;
            }
            // 用签名反查方法。找不到就跳过 —— 游戏版本变了方法就没了，
            // 这时丢掉预设比留着一条指向不存在方法的记录干净。
            auto *method = index[signature];
            if (method == nullptr)
            {
                continue;
            }
            std::vector<ClassesTab::MethodPreset> parsed;
            for (const auto &entry : list)
            {
                ClassesTab::MethodPreset preset;
                if (auto n = entry.find("name"); n != entry.end() && n->is_string())
                {
                    preset.name = n->get<std::string>();
                }
                if (auto v = entry.find("values"); v != entry.end() && v->is_object())
                {
                    for (const auto &[k, text] : v->items())
                    {
                        if (text.is_string())
                        {
                            preset.values[k] = text.get<std::string>();
                        }
                    }
                }
                if (!preset.name.empty())
                {
                    parsed.push_back(std::move(preset));
                }
            }
            if (!parsed.empty())
            {
                p.methodPresets[method] = std::move(parsed);
            }
        }
    }

    std::string selectedImage = j.at("selectedImage").get<std::string>();
    if (selectedImage.ends_with(".dll"))
    {
        selectedImage.erase(selectedImage.size() - 4);
    }
    auto assembly = Il2cpp::GetAssembly(selectedImage.c_str());
    if (assembly)
    {
        auto image = assembly->getImage();
        if (image)
        {
            p.selectedImage = image;
        }
    }
}

std::unordered_map<Il2CppClass *, Il2cpp::GC::RootedObjectList> ClassesTab::objectMap;
std::unordered_map<Il2CppClass *, Il2cpp::GC::RootedObjectList> ClassesTab::newObjectMap;
std::unordered_map<Il2CppClass *, std::set<Il2CppObject *>> ClassesTab::savedSet;

size_t ClassesTab::SavedObjectCount()
{
    size_t total = 0;
    for (const auto &[klass, objects] : savedSet)
    {
        total += objects.size();
    }
    return total;
}
std::unordered_map<MethodInfo *, ClassesTab::OriginalMethodBytes> ClassesTab::oMap;
std::unordered_map<Il2CppClass *, bool> ClassesTab::states;
PopUpSelector ClassesTab::poper;
