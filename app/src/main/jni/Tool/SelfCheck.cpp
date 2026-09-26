#include "SelfCheck.h"
#include "Il2cpp/Il2cpp.h"
#include "Includes/Logger.h"
#include "ObjectDrawManager.h"
#include "Tool/Tool.h"
#include "Tool/Unity.h"
#include "imgui/imgui.h"

extern std::vector<Il2CppImage *> g_Images;

namespace SelfCheck
{
namespace
{
Result Ok(std::string name, std::string detail)
{
    return {std::move(name), Status::Ok, std::move(detail)};
}
Result Warn(std::string name, std::string detail)
{
    return {std::move(name), Status::Warn, std::move(detail)};
}
Result Fail(std::string name, std::string detail)
{
    return {std::move(name), Status::Fail, std::move(detail)};
}
Result Info(std::string name, std::string detail)
{
    return {std::move(name), Status::Info, std::move(detail)};
}
} // namespace

std::vector<Result> Collect()
{
    std::vector<Result> out;
    out.reserve(24);

    // ---- il2cpp 运行时 ----
    if (Il2cpp::ApiResolved())
    {
        out.push_back(Ok("il2cpp API", "已解析"));
    }
    else
    {
        out.push_back(Fail("il2cpp API", "未解析 —— 工具的核心功能都不可用"));
    }

    // ---- 程序集 / 类 ----
    {
        char buf[160]{0};
        snprintf(buf, sizeof(buf), "%zu 个程序集", g_Images.size());
        if (g_Images.empty())
        {
            out.push_back(Fail("程序集", "一个都没枚举到 —— 常见于游戏还没加载完，或 il2cpp 被裁剪"));
        }
        else
        {
            out.push_back(Ok("程序集", buf));
        }
    }

    // ---- 触摸输入 ----
    {
        const int touch = Unity::TouchOffsetCheck();
        if (touch < 0)
        {
            out.push_back(Fail("触摸偏移", "UnityEngine.Touch 字段偏移不正确 —— 菜单对触摸会完全无反应"));
        }
        else if (touch == 0)
        {
            out.push_back(Warn("触摸偏移", "尚未自检（需要你先点一下屏幕）"));
        }
        else
        {
            out.push_back(Ok("触摸偏移", "自检通过"));
        }
    }
    {
        if (Unity::g_uiContextAlive)
        {
            out.push_back(Ok("ImGui 上下文", "存活"));
        }
        else
        {
            out.push_back(Fail("ImGui 上下文", "已销毁 —— 菜单不可用"));
        }
    }

    // ---- ESP 前置条件 ----
    // 这些是 ESP 能不能工作的全部前提。任何一条不成立，现象都是
    // 「不画框」，而用户看不出是哪一环坏了。
    {
        if (ObjectDrawManager::showObjectManager)
        {
            if (ObjectDrawManager::WorldToScreenAvailable())
            {
                out.push_back(Ok("ESP 投影", "相机与 WorldToScreenPoint 均已就绪"));
            }
            else
            {
                out.push_back(Fail("ESP 投影", "相机或 WorldToScreenPoint 未就绪 —— ESP 不会画任何框"));
            }
            if (ObjectDrawManager::RendererBoundsAvailable())
            {
                out.push_back(Ok("ESP 包围盒", "GetComponent<Renderer> / get_bounds 已就绪"));
            }
            else
            {
                out.push_back(Ok("ESP 包围盒", "未就绪（会自动退回固定像素尺寸框）"));
            }
            out.push_back(Info("ESP 已选目标", std::to_string(ObjectDrawManager::DrawObjectCount())));
        }
        else
        {
            out.push_back(Info("ESP", "未启用（在「工具」页勾选）"));
        }
    }

    // ---- GC 根 ----
    // 句柄只增不减意味着游戏对象永远回收不掉（内存泄漏），
    // 这是加根机制最容易出的错，必须让它可见。
    {
        const size_t roots = ObjectDrawManager::TotalRootCount();
        char buf[160]{0};
        snprintf(buf, sizeof(buf), "持有 %zu 个 GC 强根（对象扫描 / 已绘制目标 / 已保存对象）", roots);
        // 几百个属正常（用户挑的目标）。上千说明可能有东西没释放。
        if (roots > 2000)
        {
            out.push_back(Warn("GC 根", buf));
        }
        else
        {
            out.push_back(Ok("GC 根", buf));
        }
    }

    // ---- 配置 ----
    {
        char buf[256]{0};
        snprintf(buf, sizeof(buf), "%s", Il2cpp::getDataPath().c_str());
        out.push_back(Info("数据目录", buf));
    }

    // ---- 已保存的对象数 ----
    // 这些是用户手动标记的，靠 GC 强根保活。数量异常偏大通常意味着
    // 「关掉了标签页但根没释放」——那会让游戏对象永远回收不掉。
    {
        out.push_back(Info("已保存对象", std::to_string(ClassesTab::SavedObjectCount())));
    }

    // ---- 构建信息 ----
    // 出问题时能一眼确认跑的是哪个版本，省掉一轮来回。
    {
        char buf[128]{0};
        snprintf(buf, sizeof(buf), "v0.9 / %s 构建", __DATE__);
        out.push_back(Info("构建", buf));
    }
    {
        const std::string unity = Il2cpp::getUnityVersion();
        const std::string game = Il2cpp::getGameVersion();
        char buf[256]{0};
        snprintf(buf, sizeof(buf), "Unity %s / 游戏 %s", unity.c_str(), game.c_str());
        // 读不到版本**不代表工具坏了** —— 有些游戏裁掉了这些属性，
        // 或者工具初始化得比游戏早。所以是警告不是失败。
        if (unity.empty() && game.empty())
        {
            out.push_back(Info("目标（版本）",
                               "读不到 Unity / 游戏版本 —— 可能游戏裁掉了这些属性，"
                               "也可能工具初始化早于游戏。不影响其他功能"));
        }
        else
        {
            out.push_back(Info("目标", buf));
        }
    }

    // ---- 构建期开关 ----
    //
    // 这一项的价值在于**诚实**：源码里能看到的特性，不一定在你的构建里。
    // 以前只有 README 里的一段文字说明，界面上完全看不出来 ——
    // 看到代码里有回溯功能、却始终没反应，很难想到是「根本没编进去」。
    // 现在直接列出来，并给出启用方法。
    {
        struct Feature
        {
            const char *name;
            const char *note;
            bool enabled;
        };
        const Feature features[] = {
#ifdef USE_FRIDA
            {"Frida 回溯", "已编译启用", true},
#else
            {"Frida 回溯", "未编译启用（Android.mk 的 LOCAL_CPPFLAGS 加 -DUSE_FRIDA 后重新构建）", false},
#endif
#ifdef LIB_INPUT
            {"JNI 触摸注入", "已编译启用", true},
#else
            {"JNI 触摸注入", "未编译启用（当前走 UnityEngine.Input hook，无需此项）", false},
#endif
#ifdef __DEBUG__
            {"D 级详细日志", "已启用（build.ps1 -Debug）", true},
#else
            {"D 级详细日志", "已静默（W/E/I 级始终输出；需要 D 级请用 build.ps1 -Debug）", false},
#endif
        };
        for (const auto &f : features)
        {
            out.push_back(Info(std::string("构建开关 · ") + f.name, f.note));
        }
    }

    return out;
}

void DrawUI()
{
    ImGui::TextDisabled(
        "自检把「哪些前置条件成立」一次性摊开。多数问题在这个项目里默认是**静默**的"
        "（ESP 拿不到相机就只是不画框，触摸偏移错了菜单就完全没反应），\n"
        "而这里能直接看出是哪一环坏了；具体原因去「日志」页看。");
    ImGui::Separator();

    // 每 2 秒重新采集一次。采集要读对象管理器的状态（会短暂加内部锁），
    // 缓存下来既避免每帧重复加锁，也让表格内容稳定不跳动。
    static std::vector<Result> cached;
    static double lastCollect = 0.0;
    auto now = ImGui::GetTime();
    if (cached.empty() || now - lastCollect > 2.0)
    {
        lastCollect = now;
        cached = Collect();
    }

    size_t fails = 0;
    size_t warns = 0;
    for (const auto &r : cached)
    {
        if (r.status == Status::Fail)
            fails++;
        else if (r.status == Status::Warn)
            warns++;
    }

    // 注意：这里不持有任何内部锁去调用 Collect —— Collect 会去读
    // 对象管理器的状态。（Collect 内部自己短暂加锁取快照。）
    if (fails > 0)
    {
        ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%zu 项失败", fails);
    }
    if (warns > 0)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.f, 0.85f, 0.4f, 1.f), "%zu 项警告", warns);
    }
    if (fails == 0 && warns == 0)
    {
        ImGui::TextColored(ImVec4(0.4f, 1.f, 0.5f, 1.f), "全部通过");
    }

    ImGui::Separator();
    if (ImGui::BeginTable("selfcheck", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("项目", ImGuiTableColumnFlags_WidthFixed, 120.f);
        ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthFixed, 60.f);
        ImGui::TableSetupColumn("详情");
        ImGui::TableHeadersRow();
        for (const auto &r : cached)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableNextColumn();
            switch (r.status)
            {
            case Status::Ok:
                ImGui::TextColored(ImVec4(0.4f, 1.f, 0.5f, 1.f), "通过");
                break;
            case Status::Warn:
                ImGui::TextColored(ImVec4(1.f, 0.85f, 0.4f, 1.f), "警告");
                break;
            case Status::Fail:
                ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "失败");
                break;
            default:
                ImGui::TextDisabled("信息");
                break;
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.detail.c_str());
        }
        ImGui::EndTable();
    }
}
} // namespace SelfCheck
