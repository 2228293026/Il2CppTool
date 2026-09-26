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
        v.push_back({kind, Truncate(target, kMaxTarget), Truncate(detail, kMaxDetail)});
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

size_t Count()
{
    std::lock_guard guard(mutex());
    return entries().size();
}

void Clear()
{
    std::lock_guard guard(mutex());
    entries().clear();
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
    ImGui::SameLine();
    if (ImGui::SmallButton("复制全部"))
    {
        const std::string text = ExportText(list);
        ImGui::SetClipboardText(text.c_str());
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
        if (ImGui::BeginTable("##changelogrow", 3, ImGuiTableFlags_SizingStretchProp))
        {
            const float avail = ImGui::GetContentRegionAvail().x;
            ImGui::TableSetupColumn("kind", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::CalcTextSize("追踪").x + 8.0f);
            ImGui::TableSetupColumn("target", ImGuiTableColumnFlags_WidthFixed, avail * 0.42f);
            ImGui::TableSetupColumn("detail", ImGuiTableColumnFlags_WidthStretch);
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
            ImGui::EndTable();
        }
        ImGui::PopID();
    }
}
} // namespace ChangeLog
