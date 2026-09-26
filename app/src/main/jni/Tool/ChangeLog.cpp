#include "ChangeLog.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace ChangeLog
{
namespace
{
// 上限和「已保存对象」同一个量级：够用，且不会让导出文本失控。
// 超出时丢弃**最早**的 —— 最近改的才是当下要看的。
constexpr size_t kMaxEntries = 512;
// 目标名和详情都是外部来的（类名可能很长、混淆名可能含控制字符），
// 固定上限 + snprintf 截断，不要用 %s 无界拷贝。
constexpr size_t kMaxTarget = 220;
constexpr size_t kMaxDetail = 220;

std::mutex &mutex()
{
    static std::mutex m;
    return m;
}

std::vector<Entry> &entries()
{
    static std::vector<Entry> e;
    return e;
}

std::string Truncate(const std::string &in, size_t maxLen)
{
    if (in.size() <= maxLen)
    {
        return in;
    }
    // 保留前缀（名字的前半段最能认出是哪个），尾部标出被截了多少。
    char buf[32]{0};
    snprintf(buf, sizeof(buf), "…(+%zu 字符)", in.size() - maxLen);
    return in.substr(0, maxLen) + buf;
}

const char *KindLabel(Kind k)
{
    switch (k)
    {
    case Kind::Field:
        return "字段";
    case Kind::Patch:
        return "补丁";
    case Kind::Hook:
        return "追踪";
    case Kind::Watch:
        return "关注";
    case Kind::Save:
        return "保存";
    }
    return "?";
}

ImVec4 KindColor(Kind k)
{
    switch (k)
    {
    case Kind::Field:
        return {0.55f, 0.80f, 1.0f, 1.f};
    case Kind::Patch:
        return {1.0f, 0.60f, 0.45f, 1.f};
    case Kind::Hook:
        return {0.75f, 1.0f, 0.60f, 1.f};
    case Kind::Watch:
        return {0.90f, 0.80f, 1.0f, 1.f};
    case Kind::Save:
        return {0.80f, 0.85f, 0.90f, 1.f};
    }
    return {1.f, 1.f, 1.f, 1.f};
}

// 导出成纯文本。用 ### 而不是 ## 做分隔：ImGui 里 ## 会被当 ID 前缀，
// 而目标名可能含 ##，那样会在界面上被裁掉一部分。
std::string ExportText(const std::vector<Entry> &list)
{
    std::string out;
    out.reserve(list.size() * 80);
    for (const auto &e : list)
    {
        out += "[";
        out += KindLabel(e.kind);
        out += "] ";
        out += e.target;
        if (!e.detail.empty())
        {
            out += "  →  ";
            out += e.detail;
        }
        out += "\n";
    }
    return out;
}
} // namespace

namespace {
// 条目 id 计数器。id 是条目的**唯一身份** ——
// 按内容（target+detail）定位条目是不安全的，见 UndoById 的说明。
uint64_t g_nextId = 1;
Restorer g_restorer = nullptr;
// 句柄释放器。由外部注入（和撤销器一样，ChangeLog 不认识 il2cpp）。
HandleReleaser g_handleReleaser = nullptr;

// 找到这条记录并就地修改。用 target+detail 定位：同一条记录
// （同样的字段、同样的值）**只会有一条**，因为每次改动都会记一条，
// 而「改回同样的值」也会单独记一条。所以按内容定位是够的。
Entry *FindById(uint64_t id)
{
    auto &v = entries();
    for (auto &e : v)
    {
        if (e.id == id)
        {
            return &e;
        }
    }
    return nullptr;
}

// 淘汰最老的 drop 条，**顺带释放它们持有的句柄**。
//
// 不释放的后果不是「多用一点内存」，而是那些对象**永远不会被回收** ——
// 而 512 条记录对应的可能是几百个游戏对象。整张表被 Clear() 时同理。
void DropOldestLocked(size_t drop)
{
    auto &v = entries();
    if (drop == 0 || drop > v.size())
    {
        return;
    }
    if (g_handleReleaser != nullptr)
    {
        for (size_t i = 0; i < drop; i++)
        {
            if (v[i].handle != 0)
            {
                g_handleReleaser(v[i].handle);
                v[i].handle = 0;
            }
        }
    }
    v.erase(v.begin(), v.begin() + drop);
}
} // namespace

void SetRestorer(Restorer fn)
{
    std::lock_guard guard(mutex());
    g_restorer = fn;
}

void SetHandleReleaser(HandleReleaser fn)
{
    std::lock_guard guard(mutex());
    g_handleReleaser = fn;
}

void RecordUndoable(Kind kind, const std::string &target, const std::string &oldValue,
                    const std::string &newValue, uint32_t handle,
                    const std::vector<std::string> &paths)
{
    if (target.empty())
    {
        // 早退的话 handle 就**没人还了** —— 泄漏，而且是在最不起眼的地方。
        // 不接管就不该由调用方交出来，所以这里直接释放。
        if (handle != 0 && g_handleReleaser != nullptr)
        {
            g_handleReleaser(handle);
        }
        return;
    }
    try
    {
        // detail 用「旧 -> 新」，和头文件里那句注释写的一致 ——
        // 之前注释写着 "100 -> 999" 而代码只记新值（第 68 轮那个
        // 「注释说的 vs 代码做的」）。现在先让它们一致。
        std::string detail = newValue.empty() ? oldValue : (oldValue + " -> " + newValue);
        std::lock_guard guard(mutex());
        auto &v = entries();
        v.push_back({.kind = kind, .target = Truncate(target, kMaxTarget),
                     .detail = Truncate(detail, kMaxDetail), .id = g_nextId++,
                     .oldValue = Truncate(oldValue, kMaxDetail), .paths = paths, .handle = handle});
        if (v.size() > kMaxEntries)
        {
            // 淘汰最老的，**连同它们持有的 GC 句柄**。
            DropOldestLocked(v.size() - kMaxEntries);
        }
    }
    catch (...)
    {
        // 记录失败但句柄已经收下了 → 同样要还回去。
        if (handle != 0 && g_handleReleaser != nullptr)
        {
            g_handleReleaser(handle);
        }
    }
}

// 撤销器有没有注入。刻意**不持锁**：g_restorer 只在 Init 时赋值一次，读一个
// 未初始化的 bool 无害；自检页在渲染线程，没必要为此和 ImGuiJson 抢锁。
bool UndoAvailable()
{
    return g_restorer != nullptr;
}

bool CanUndo(const Entry &entry)
{
    return entry.handle != 0 && !entry.oldValue.empty() && g_restorer != nullptr;
}

// UndoById 在**已经持锁**时用它 —— 锁不可重入，CanUndo 不能在里面再加一次。
static bool CanUndoLocked(const Entry &entry)
{
    return entry.handle != 0 && !entry.oldValue.empty() && g_restorer != nullptr;
}


// 按 id 撤销。**全程只用锁内的活数据**，不用界面传来的快照 ——
// 快照最多滞后 1 秒（DrawUI 的缓存），而句柄在这期间可能已经被释放。
//
// 为什么不能按 target+detail 找：同一个字段来回改几次就会出现两条
// 一模一样的记录（「100 -> 999」出现两次），按内容找只会命中**最新**
// 那条。于是用户点**旧**那条的「恢复」，被作废（并释放句柄）的是**新**
// 那条 —— 下一帧新那条的按钮还在，再点就是**用已释放的句柄**，
// 直接崩在用户的游戏里。
size_t UndoAll(size_t *skipped, size_t *failed)
{
    if (skipped) *skipped = 0;
    if (failed) *failed = 0;

    // 先把「要恢复哪些」在锁内定下来，**倒着**。
    // 不能一边遍历一边恢复：UndoById 会自己加锁，而锁不可重入。
    std::vector<uint64_t> ids;
    {
        std::lock_guard guard(mutex());
        for (size_t i = entries().size(); i-- > 0;)
        {
            const Entry &e = entries()[i];
            if (!CanUndoLocked(e))
            {
                // 两条不同的原因都在这里，而它们**都不该被当成错误**：
                //   1. 已经逐条恢复过 —— UndoById 把 handle 和 oldValue 都清了
                //   2. 当初就没记原值（比如只有记录、没有可回退的原字节）
                // 真正「对象被回收了」不在这里：那种情况 handle 仍然非 0，
                // 会进 ids 并在 UndoById 里失败，落到 **failed** 桶里。
                if (skipped) (*skipped)++;
                continue;
            }
            ids.push_back(e.id);
        }
    }

    size_t done = 0;
    for (uint64_t id : ids)
    {
        if (UndoById(id))
        {
            done++;
        }
        else if (failed)
        {
            (*failed)++;
        }
    }
    return done;
}
bool UndoById(uint64_t id)
{
    try
    {
        std::lock_guard guard(mutex());
        Entry *e = FindById(id);
        if (e == nullptr || !CanUndoLocked(*e))
        {
            return false;
        }
        // 先复制出来再放开调用：恢复器会去碰 il2cpp，不能在锁里做。
        const uint32_t handle = e->handle;
        const std::vector<std::string> paths = e->paths;
        const std::string oldValue = e->oldValue;
        const std::string target = e->target;
        Restorer fn = g_restorer;
        if (fn == nullptr)
        {
            return false;
        }
        Entry snapshot;
        snapshot.handle = handle;
        snapshot.paths = paths;
        snapshot.oldValue = oldValue;
        snapshot.target = target;
        if (!fn(snapshot))
        {
            return false;
        }
        // 只作废「可恢复」，条目本身留着 —— 它仍然是一条记录。
        //
        // **句柄必须归还**，不能只置零。置零 = 那个对象永远不会被回收，
        // 而用户每点一次「恢复」就漏一个 —— 这是第 73 轮引入的泄漏。
        if (e->handle != 0 && g_handleReleaser != nullptr)
        {
            g_handleReleaser(e->handle);
        }
        e->handle = 0;
        e->oldValue.clear();
        e->paths.clear();
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void Record(Kind kind, const std::string &target, const std::string &detail)
{
    if (target.empty())
    {
        return;
    }
    try
    {
        std::lock_guard guard(mutex());
        auto &v = entries();
        v.push_back({.kind = kind, .target = Truncate(target, kMaxTarget),
                     .detail = Truncate(detail, kMaxDetail), .id = g_nextId++});
        if (v.size() > kMaxEntries)
        {
            v.erase(v.begin(), v.begin() + (v.size() - kMaxEntries));
        }
    }
    catch (...)
    {
        // 记一条流水不该让工具出问题。分配失败时静默丢弃这一条即可 ——
        // 丢一条记录，远比从 ImGui 回调里抛出去把渲染线程带崩要好。
    }
}

std::vector<Entry> Snapshot()
{
    std::lock_guard guard(mutex());
    return entries();
}

// 只数、不拷。理由见头文件的说明。
size_t CountUndoable()
{
    std::lock_guard guard(mutex());
    size_t n = 0;
    for (const auto &e : entries())
    {
        if (CanUndoLocked(e))
        {
            n++;
        }
    }
    return n;
}
size_t Count()
{
    std::lock_guard guard(mutex());
    return entries().size();
}

void Clear()
{
    std::lock_guard guard(mutex());
    // 先把句柄全还回去再清表。直接 clear() 的话对象就**永远不会被回收** ——
    // 「清空改动记录」这个按钮会变成一个看不见的泄漏。
    DropOldestLocked(entries().size());
}

void DrawUI()
{
    // Snapshot() 是**深拷贝**：512 条 × 2 个 std::string = 1024 次堆分配。
    // 绘制是每帧调用的，所以直接每帧拷贝就是每帧 1024 次分配 ——
    // 这正是我给类列表加渲染上限（第 51 轮）时解决的**同一个问题**，
    // 换个文件又犯了一次。
    //
    // 便宜的判断：只在**条数变了**或者**过了 1 秒**时才重新拷。
    // 条数变更是 O(1) 的（Count() 只加一次锁取 size）。
    // 例外是「已满 512 条之后又加一条又丢一条」—— 条数不变，内容会滞后
    // 最多 1 秒。改动记录本来就不是逐帧刷新的实时数据，1 秒完全够。
    static std::vector<Entry> cached;
    static size_t cachedCount = 0;
    static double lastRefresh = 0.0;
    const double now = std::chrono::duration<double>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    if (now - lastRefresh > 1.0)
    {
        const size_t n = Count();
        if (n != cachedCount || cached.empty())
        {
            cached = Snapshot();
            cachedCount = n;
        }
        lastRefresh = now;
    }
    const auto &list = cached;

    // 注意：ImGui **不解析 markdown**（第 49 轮在自检页犯过一次）。
    // 写 **粗体** 会在界面上显示成 literally 带星号的文本。
    ImGui::TextDisabled(
        "记录本工具「改动了目标游戏的什么」。这些改动只存在于游戏进程内存里 ——\n"
        "游戏一关就全部消失，工具自己也不会留下任何痕迹。\n"
        "改了十几个字段之后想不起来「当时把哪个设成了多少」时，看这一页。");
    ImGui::Separator();

    if (list.empty())
    {
        ImGui::TextDisabled("还没有任何改动记录。");
        return;
    }

    char summary[128]{0};
    snprintf(summary, sizeof(summary), "共 %zu 条（最多保留 %zu 条，超出丢弃最早的）",
             list.size(), kMaxEntries);
    ImGui::TextDisabled("%s", summary);
    // 按钮另起一行。跟在 summary 后面用 SameLine 的话，摘要一长
    // 「复制全部 / 清空」就被顶出屏幕 —— 而这两个按钮正是这一页
    // 除了看列表之外唯一能做的事。
    if (ImGui::SmallButton("复制全部"))
    {
        const std::string text = ExportText(list);
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::SameLine();
    // 「全部恢复」：把这一轮做过的改动**一次性退回去**。
    //
    // 只有逐条「恢复」的时候，用户的真实场景是：连改了五处，想全部退干净。
    // 逐条点五次很烦，而更糟的是**漏点一条** —— 界面上不会告诉他
    // 「还有一条没退」，而他会以为游戏已经回到原样了。
    if (ImGui::SmallButton("全部恢复"))
    {
        size_t skipped = 0;
        size_t failed = 0;
        const size_t done = UndoAll(&skipped, &failed);
        // 立刻重取快照，否则列表上还要最多 1 秒才显示成恢复后的样子。
        lastRefresh = 0.0;
        cached.clear();
        cachedCount = 0;

        // 三种说法必须分开，而且**颜色要跟着语义走**（第 103 轮改）。
        //
        // 我第一版把「本来就不可恢复」说成「对象已失效跳过」，把
        // 「恢复失败」和它并列 —— 方向正好反了：
        //   · 已经逐条恢复过的条目（很常见）被说成「对象已失效」，
        //     用户会以为游戏在丢对象；
        //   · 真正的恢复失败（对象真的没了）反而和良性情况混在一起，
        //     一句「已恢复 N 条」就过去了。
        //
        // 唯独 failed > 0 时必须**用警告色**，并直说
        // 「游戏现在不是原样」—— 用户会据此决定要不要重开游戏。
        char msg[256]{0};
        if (done == 0 && skipped == 0 && failed == 0)
        {
            snprintf(msg, sizeof(msg), "没有可恢复的记录");
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "%s", msg);
        }
        else if (failed > 0)
        {
            snprintf(msg, sizeof(msg),
                     "已恢复 %zu 条；%zu 条恢复失败 —— 游戏现在【不是】原样。另有 %zu 条本来就不可恢复",
                     done, failed, skipped);
            ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "%s", msg);
        }
        else if (skipped > 0)
        {
            snprintf(msg, sizeof(msg),
                     "已恢复 %zu 条（另有 %zu 条本来就不可恢复：已经恢复过或没记原值）",
                     done, skipped);
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.f, 1.f), "%s", msg);
        }
        else
        {
            snprintf(msg, sizeof(msg), "已恢复 %zu 条", done);
            ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.6f, 1.f), "%s", msg);
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("清空"))
    {
        Clear();
        // 立刻让下一帧重新取一次，否则界面上还要再显示最多 1 秒
        // 才消失 —— 用户点了「清空」却看到内容还在，会以为按钮没生效。
        lastRefresh = 0.0;
        cached.clear();
        cachedCount = 0;
        return;
    }
    ImGui::Separator();

    // 新的在最上面：当下关心的永远是刚改的那几条。
    //
    // 三列固定布局。不用「标签 + SameLine + 值」：target 最多 220 字符
    // （类名 + 字段路径），SameLine 之后 detail（也就是**新值**，这一页
    // 真正要看的**东西**）会被顶到屏幕外。
    // 关注值那一页已经犯过一次，这里不重复犯。
    for (auto it = list.rbegin(); it != list.rend(); ++it)
    {
        ImGui::PushID(static_cast<int>(it - list.rbegin()));
        if (ImGui::BeginTable("##changelogrow", 4, ImGuiTableFlags_SizingStretchProp))
        {
            const float avail = ImGui::GetContentRegionAvail().x;
            ImGui::TableSetupColumn("kind", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::CalcTextSize("追踪").x + 8.0f);
            ImGui::TableSetupColumn("target", ImGuiTableColumnFlags_WidthFixed, avail * 0.34f);
            ImGui::TableSetupColumn("detail", ImGuiTableColumnFlags_WidthStretch);
            // 宽度按**最宽的那个词**算，而不是按「恢复」两个字。
            // 52 是给「恢复」留的；「不可恢复」四个中文字放不下，
            // 会被裁成「不可…」—— 而那正是最需要看清的一种状态（第 104 轮）。
            ImGui::TableSetupColumn("undo", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::CalcTextSize("不可恢复").x + 14.0f);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(KindColor(it->kind), "%s", KindLabel(it->kind));
            ImGui::TableNextColumn();
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", it->target.c_str());
            }
            ImGui::TextUnformatted(it->target.c_str());
            ImGui::TableNextColumn();
            if (it->detail.empty())
            {
                ImGui::TextDisabled("-");
            }
            else
            {
                ImGui::TextUnformatted(it->detail.c_str());
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", it->detail.c_str());
                }
            }

            ImGui::TableNextColumn();
            if (CanUndo(*it))
            {
                if (ImGui::SmallButton("恢复"))
                {
                    if (UndoById(it->id))
                    {
                        // 立刻重取快照，否则界面上这一条还会显示最多 1 秒，
                        // 用户会以为「点了没反应」。
                        lastRefresh = 0.0;
                        cached.clear();
                        cachedCount = 0;
                    }
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("把字段退回到改动前的值（%s）", it->oldValue.c_str());
                }
            }
            else
            {
                // 占位，保证三列/四列的行高一致 —— 少一个控件会让这一行
                // 比别的行矮一点，扫起来像有东西没加载出来。
                // 不可恢复的行**必须看得出来**（第 104 轮）。
                //
                // 旧代码这一格只放一个看不见的 Dummy，于是：
                //   · 逐条点了「恢复」，那一行在界面上**完全没变化**；
                //   · 点了「全部恢复」，整个列表看起来**一模一样**，
                //     只有上面那行文字说明发生了什么。
                //
                // 而用户真正要回答的问题是「**现在游戏里还挂着哪些改动**」。
                // 列表长一个样，这个问题就**没法回答** —— 只能回去一条条点，
                // 点到「恢复」按钮不见了才知道那条已经退过了。
                //
                // 三种状态要分开说：
                //   已经恢复过   退过了（按钮消失是因为 oldValue 被清空）
                //   本来就不可恢复  从来没有可回退的原值 / 撤销器没注入
                // 混成一句「不可恢复」是不够的 —— 前者是正常的，后者是问题。
                const bool reverted = it->handle == 0 && it->oldValue.empty();
                if (reverted)
                {
                    ImGui::TextDisabled("已恢复");
                }
                else
                {
                    ImGui::TextDisabled("不可恢复");
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip(
                            "这一条没有可回退的原值（或者撤销器没注入），"
                            "所以无法自动恢复。改动【仍然生效】。");
                    }
                }
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    }
}
} // namespace ChangeLog
