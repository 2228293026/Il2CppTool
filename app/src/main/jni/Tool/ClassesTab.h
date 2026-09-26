#pragma once

#include "Il2cpp/Il2cpp.h"
#include "Il2cpp/il2cpp-class.h"
#include "Includes/circular_buffer.h"
#include "Tool/PopUpSelector.h"
#include <set>
struct ClassesTab
{
    using Object = Il2CppObject *;
    using Class = Il2CppClass *;
    using Json = nlohmann::ordered_json;
    using Paths = std::vector<std::string>;
    using DataPair = std::pair<std::pair<Il2CppObject *, Json>, Paths>;

    using MethodParamList = std::vector<std::pair<const char *, Il2CppType *>>;
    using MethodList = std::vector<std::pair<MethodInfo *, MethodParamList>>;
    using ClassMethodMap = std::unordered_map<Class, MethodList>;

    struct CallData
    {
        MethodInfo *method;
        Il2CppObject *thiz;
        Il2CppArray<Il2CppObject *> *params;
    };

    struct ParamValue
    {
        std::string value;
        Il2CppObject *object;
    };

    std::unordered_map<Object, DataPair> dataMap{};

    bool caseSensitive = true;
    bool filterByClass = true;
    bool filterByMethod = false;
    bool filterByField = false;
    bool showAllClasses = false;
    bool includeAllImages = false;

    std::map<Il2CppObject *, bool> tabMap{};

    Il2CppImage *selectedImage = nullptr;

    std::vector<Class> classes{};
    std::vector<Class> filteredClasses{};
    // 原来的 tracer（std::vector<Il2CppClass *>）已删除：全项目没有任何
    // 读它的地方（唯一一处是注释掉的 "Add to Tracer" 按钮）。
    // 留着只会让人以为「按类追踪」这个功能存在。
    std::vector<MethodInfo *> tracedMethods;

    // 扫描结果缓存。
    //
    // 用 RootedObjectList 而不是裸 vector：这些对象会被 UI 长期持有（用户可能
    // 在列表里翻看几分钟），期间游戏侧完全可能销毁它们、GC 回收，裸指针就变野了。
    // RootedObjectList 给每个对象挂一个强句柄，条目存活期间 GC 不回收它。
    static std::unordered_map<Il2CppClass *, Il2cpp::GC::RootedObjectList> objectMap;
    static std::unordered_map<Il2CppClass *, Il2cpp::GC::RootedObjectList> newObjectMap;
    // 已保存（用户标记）的对象，同样要保活。
    static std::unordered_map<Il2CppClass *, std::set<Il2CppObject *>> savedSet;
    // 方法调用面板里「每个参数的当前输入值」。
    //
    // 用 shared_ptr 而不是直接持有：界面上填参数时用的是
    // `auto &param = params[paramKey]`，然后把这个引用**捕获进 lambda** 交给
    // 软键盘（存在全局的 lastCallback 里，跨帧才触发）。
    // 如果用户「打开软键盘的同时把标签页关掉」，tab 析构 → paramMap 析构 →
    // 回调里那个引用就是野的。shared_ptr 让回调持有一份，tab 先走也没事。
    std::shared_ptr<std::unordered_map<MethodInfo *, std::unordered_map<std::string, ParamValue>>> paramMap{};

    // 方法调用参数「预设」。反复调同一个方法、只改一两个值时，
    // 重新用软键盘敲一遍非常折磨；保存一次、之后一键载入。
    //
    // **只存文本，绝不存 Il2CppObject***。
    // ParamValue::object 指向托管对象，而预设可能很久之后才被载入 ——
    // 那个对象早被 GC 回收了。复原一个陈旧指针并交给 VM 就是
    // use-after-free，而且会崩在毫不相干的地方。载入时会把 object 清成
    // nullptr，引用类型参数需要用户重新选一次。这是有意的安全取舍：
    // 宁可多一步，也不能复活野指针。
    struct MethodPreset
    {
        std::string name;
        // paramKey → 用户输入的文本
        std::map<std::string, std::string> values;
    };
    // method → 该方法的若干预设（按保存顺序）
    std::map<MethodInfo *, std::vector<MethodPreset>> methodPresets;
    // 每个方法当前选中的预设名（供下拉框显示）
    std::map<MethodInfo *, std::string> selectedPreset;
    // UI 上正在输入的新预设名
    std::string newPresetName;

    // savedSet 里一共保存了多少个对象（自检页用）。
    // savedSet 是 static 的，但计数要走它所在的那个 TU。
    static size_t SavedObjectCount();

    // ---- 关注值（Watch）----
    //
    // JSON 检视器显示的是**一次性快照**：改了值要点 Refresh 才刷新。
    // 但这个工具最常见的用法是盯着**一个数**（血量、弹药、分数、开关标志）
    // 随游戏实时变化 —— 为此每次都点 Refresh 既繁琐又容易看错。
    //
    // 所以：把某个叶子字段钉进关注列表，之后每 200ms 自动重读一次，
    // 值变了就高亮。这样「看一个数」从「反复手动刷新」变成「盯着看」。
    //
    // **冻结**（可选）：把当前值钉住，每帧写回。
    // 没有它的话，改完的值经常撑不过一帧 —— 游戏自己的 Update 里
    // `health -= damage` 或者从服务器同步，下一帧就覆盖回去了。
    // 于是用户的体验是「我明明改了，它弹回来了」，却查不出原因。
    // 冻结之后值真正立住，这才算改成功。
    //
    // 安全边界：只有**原始类型**（int/float/bool/string 引用）的叶子字段
    // 才会出现在关注列表里（见 canWatch），所以每帧写回的是
    // 「一个已加 GC 根的对象上的一个原始类型字段」—— 不会写到野内存，
    // 也不会写坏对象结构。字符串字段写回的是**托管字符串指针**，
    // 而 NewString 出来的对象由 GC 托管，引用计数由运行时维护。
    struct Watch
    {
        Il2CppObject *object;             // 原始指针，仅用于显示和去重
        uint32_t handle;                  // GC 强根：必须，否则对象被回收后就是野指针
        std::vector<std::string> paths;   // 从根对象到这个叶子字段的路径
        std::string label;                // 显示用，如 "Player.health"
        std::string lastValue;            // 上一次读到的值（用于判断是否变化）
        bool changed;                     // 本次轮询里值变了
        bool invalid;                     // 对象已被 GC 或路径失效
        bool frozen = false;              // 冻结中：每帧把 frozenValue 写回去
        std::string frozenValue;          // 冻结时钉住的那个值（文本形式）
    };
    static void AddWatch(Il2CppObject *object, const std::vector<std::string> &paths,
                         const std::string &label);
    static void RemoveWatchAt(size_t index);
    static void ClearWatches();
    static size_t WatchCount();

    // 关注值/冻结的统计，给自检页用。
    // 冻结项的对象一旦被回收会自动解冻（第 66 轮），但「有多少个正冻着」
    // 只有用户自己知道 —— 让它在自检页可见。
    static void WatchStats(size_t *total, size_t *frozen, size_t *frozenInvalid);
    // 在 JSON 检视器末尾画关注列表。由 ImGuiJson 调用。
    // 把「撤销器」和「句柄释放器」注入改动记录。
    // 冻结的每帧写回。由 Tool::Draw() **无条件**调用 ——
    // 不能放在 CollapsingHeader("关注值") 里面，否则折叠时冻结会静默失效。
    static void ApplyFreezes();
    // 类筛选工作线程在不在。线程没起来的话，每次搜索都要在**渲染线程**
    // 同步跑完（界面卡住），而这件事只会在你搜索的时候才被发现。
    static bool FilterWorkerRunning();
    // 被打了补丁的方法数，以及**还能不能恢复**的数。
    // 补丁是唯一改可执行代码的操作，而它的原字节是唯一的退路 ——
    // 原字节没了就等于永远退不回去，所以这两个数要能被看见。
    static void PatchStats(size_t *patched, size_t *restorable);


    // 必须在任何 RecordUndoable 之前调用，否则记录表没有恢复能力。
    static void InitChangeLogUndo();
    static void DrawWatches();
    // 显示「上一次写入失败」的原因。挂在工具页而不是对象检视器里 ——
    // 设置它的回调可能来自 CallerView（另一个界面），而对象检视器
    // 只在有打开的对象 tab 时才绘制。
    static void DrawFieldErrorBanner();

    std::unordered_map<MethodInfo *, CircularBuffer<std::pair<std::string, Il2CppObject *>>> callResults{};

    MethodList &buildMethodMap(Il2CppClass *klass);
    // 按类缓存的方法列表。只在渲染线程访问。
    //
    // 旧实现是函数内 static 的**单项**缓存且所有 tab 共享：缓存命中率
    // 接近 0（弹窗在「对每个对象遍历」的循环里打开，相邻对象的类几乎
    // 必然不同），而且返回的引用会在别处触发重建时当场失效。
    // 改为按类缓存，命中任意类都直接返回。
    std::unordered_map<Il2CppClass *, MethodList> methodCache;
    // 缓存条目上限。方法列表是稳定元数据、命中率很高，但游戏可以动态
    // 加载 assembly，无界增长迟早吃掉内存。
    static constexpr size_t kMethodCacheLimit = 256;

    ClassMethodMap methodMap{};
    ClassesTab();

    Paths &getJsonPaths(Il2CppObject *object);

    void setJsonObject(Il2CppObject *object);

    Json &getJsonObject(Il2CppObject *object);

    void ImGuiObjectSelector(int id, Il2CppClass *klass, const char *prefix,
                             std::function<void(Il2CppObject *)> onSelect, bool canNew = false);

    void CallerView(Il2CppClass *klass, MethodInfo *method, const MethodParamList &paramsInfo,
                    Il2CppObject *thiz = nullptr);

    bool isMethodHooked(MethodInfo *method);

    void PatcherView(Il2CppClass *klass, MethodInfo *method, const MethodParamList &paramsInfo,
                     Il2CppObject *thiz = nullptr);

    void HookerView(Il2CppClass *klass, MethodInfo *method, const MethodParamList &paramsInfo,
                    Il2CppObject *thiz = nullptr);

    const MethodParamList &getCachedParams(MethodInfo *method);
    // 方法参数缓存条目上限（见 getCachedParams 的实现注释）。
    static constexpr size_t kParamCacheLimit = 4096;

    struct OriginalMethodBytes
    {
        std::vector<uint8_t> bytes;
        std::string text;
    };
    static std::unordered_map<MethodInfo *, OriginalMethodBytes> oMap;
    static PopUpSelector poper; // still a prototype!
    bool MethodViewer(Il2CppClass *klass, MethodInfo *method, const MethodParamList &paramsInfo,
                      Il2CppObject *thiz = nullptr, bool includeInflated = false);

    static std::unordered_map<Il2CppClass *, bool> states;
    void ClassViewer(Il2CppClass *klass);

    std::string filter = "";
    int selectedImageIndex = -1;
    bool traceState = false;

    int maxProgress = 0;
    int progress = 0;
    void Draw(int index = -1, bool closeable = false);

    bool opened = true;
    bool currentlyOpened = false;
    bool setOpenedTab = false;
    void DrawTabMap();

    void ImGuiJson(Il2CppObject *object);

    // 触发一次筛选。
    //
    // 旧实现是**同步**的：调用方（每敲一个字符、每切一次选项）都要同步
    // 跑完 getClasses() + 每个类的 getMethods() + getParamsInfo()。
    // 那是「所有 assembly × 所有类 × 所有方法」量级的元数据遍历，
    // 全部压在渲染线程上 —— 用户每敲一个字符，游戏就卡一下。
    //
    // 现在只把请求丢给后台线程就立刻返回；结果算好后由渲染线程在
    // Draw 里认领。连续快速输入时只保留最新的一次请求（旧的直接丢弃），
    // 不会排出一长串过期的筛选任务。
    void FilterClasses(const std::string &filter);
    // 渲染线程调用：若后台已完成与当前请求匹配的筛选，就把结果取回来。
    // 返回 true 表示本次有新结果落地。
    bool PollFilterResult();
    // 是否还有未完成的筛选请求。UI 用它显示「筛选中…」。
    bool IsFilterPending();
    // 上一轮筛选的失败原因；nullptr = 成功/未跑过。
    // UI 用它把「空列表」和「筛选失败」区分开。
    const char *GetFilterFailure();
    // 后台筛选用的共享状态。用 shared_ptr 是为了让工作线程即使在 tab
    // 被销毁之后才跑完，也仍然持有一份有效内存 —— 否则就是 use-after-free。
    struct FilterState;
    std::shared_ptr<FilterState> filterState;
};

// 全局筛选工作线程的启动/停止。停止时必须 join，否则全局 std::thread
// 析构时 terminate —— 而这是注入进别人游戏的库，触发它等于让用户的
// 游戏莫名崩掉。
namespace ClassesTabWorker
{
void EnsureStarted();
void Shutdown();
} // namespace ClassesTabWorker

void to_json(nlohmann::ordered_json &j, const ClassesTab &p);
void from_json(const nlohmann::ordered_json &j, ClassesTab &p);
void hookerHandler(void *address, DobbyRegisterContext *ctx);
