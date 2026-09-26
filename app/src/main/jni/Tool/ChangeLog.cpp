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
    const auto list = Snapshot();

    ImGui::TextDisabled(
        "记录本工具**改动了目标游戏的什么**。这些改动只存在于游戏进程内存里 ——\n"
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
        return;
    }
    ImGui::Separator();

    // 新的在最上面：当下关心的永远是刚改的那几条。
    for (auto it = list.rbegin(); it != list.rend(); ++it)
    {
        ImGui::PushID(static_cast<int>(it - list.rbegin()));
        ImGui::TextColored(KindColor(it->kind), "%s", KindLabel(it->kind));
        ImGui::SameLine();
        ImGui::TextUnformatted(it->target.c_str());
        if (!it->detail.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("→  %s", it->detail.c_str());
        }
        ImGui::PopID();
    }
}
} // namespace ChangeLog
