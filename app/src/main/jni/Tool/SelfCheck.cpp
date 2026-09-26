#include "SelfCheck.h"
#include "ChangeLog.h"
#include "Il2cpp/Il2cpp.h"
#include "Includes/Logger.h"
#include "ObjectDrawManager.h"
#include "Tool/Tool.h"
#include "Tool/Keyboard.h"
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
        snprintf(buf, sizeof(buf), "%zu 个程序集（每 3 秒自动复查，数量变化会刷新列表）",
                 g_Images.size());
        if (g_Images.empty())
        {
            out.push_back(Fail("程序集", "一个都没枚举到 —— 工具初始化早于游戏，尚未恢复"));
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
    // ---- 软键盘 ----
    // 这是**整个工具的文本输入**的唯一入口：搜索框、参数输入、字段编辑、
    // 预设名全靠它。它不可用时用户只会看到「点了没反应」，
    // 而不会有任何报错 —— 所以必须在自检里显式列出。
    {
        if (Keyboard::IsAvailable())
        {
            out.push_back(Ok("软键盘", "TouchScreenKeyboard 可用"));
        }
        else
        {
            out.push_back(Fail("软键盘", "TouchScreenKeyboard 不可用 ——「所有文本输入都无法使用」"));
        }
    }

    // ---- ImGui 上下文 ----
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
                // 框**仍然会画**，只是退回固定像素尺寸 —— 所以不是失败。
                // 但也不该报「通过」：用户看到的框和实际物体大小对不上，
                // 而界面上没有任何提示。降级要如实说是降级。
                out.push_back(Warn("ESP 包围盒", "未就绪 —— 框仍会画，但退回固定像素尺寸（与物体实际大小不符）"));
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

    // ---- 改动记录 · 撤销 ----
    //
    // 撤销器没注入的话，**所有「恢复」按钮会集体消失**，而界面不给任何原因：
    // 记录在、按钮没了，用户分不清是功能没做、对象被回收、还是被关掉了。
    //
    // 这正是自检页存在的意义 —— 把「默认静默」的环节变成一条看得见的结论。
    // （第 78/80 轮那三个 bug 全部属于「不报错、只是没生效」这一类。）
    {
        const size_t records = ChangeLog::Count();
        char buf[200]{0};
        if (!ChangeLog::UndoAvailable())
        {
            snprintf(buf, sizeof(buf), "撤销器未注入（记录 %zu 条）——「恢复」按钮不会出现", records);
            out.push_back(Fail("改动记录 · 撤销", buf));
        }
        else if (records == 0)
        {
            out.push_back(Info("改动记录 · 撤销", "撤销器已就绪（还没有任何改动记录）"));
        }
        else
        {
            // 统计有多少条还能撤销 —— 全都不可撤销通常意味着句柄没拿到。
            size_t undoable = 0;
            for (const auto &e : ChangeLog::Snapshot())
            {
                if (ChangeLog::CanUndo(e))
                {
                    undoable++;
                }
            }
            snprintf(buf, sizeof(buf), "共 %zu 条，其中 %zu 条可撤销", records, undoable);
            if (undoable == 0)
            {
                out.push_back(Warn("改动记录 · 撤销", buf));
            }
            else
            {
                out.push_back(Ok("改动记录 · 撤销", buf));
            }
        }
    }

    // ---- 配置 ----
    {
        char buf[256]{0};
        snprintf(buf, sizeof(buf), "%s", Il2cpp::getDataPath().c_str());
        out.push_back(Info("数据目录", buf));
    }


    // ---- 配置读失败过吗 ----
    //
    // ConfigLoad 的 catch 会直接 ConfigSave() 覆盖掉那份坏文件，
    // 于是「全部配置丢失」在界面上**一点痕迹都没有** ——
    // 标签页回到默认、预设没了，而工具看起来一切正常。
    {
        if (Tool::ConfigLoadFailed())
        {
            out.push_back(Fail("配置读取",
                               "本次启动配置解析失败，已重置为默认（损坏的副本留在 "
                               "class_tabs.json.corrupt）"));
        }
    }

    // ---- 数据目录**可写**吗 ----
    //
    // 上面那行只证明路径取到了，不证明能写。真机上路径可能取不到、或者
    // 目录权限不对，fopen 会失败 —— 而配置的读写**全部**静默失败
    //（ConfigSave 里没有任何出口）。表现是「参数预设设了、重开就没了」，
    // 用户完全不知道是保存失败还是根本没保存。
    //
    // 真的写一个探针再删掉：这是唯一能证明「可写」的方式，
    // 而这一页存在的意义就是把静默的环节摊开。
    {
        const std::string probe = Il2cpp::getDataPath() + "/.il2cptool_write_probe";
        FILE *f = fopen(probe.c_str(), "wb");
        if (f == nullptr)
        {
            out.push_back(Fail("数据目录可写",
                               "无法写入该目录 —— 参数预设和配置都不会被保存，"
                               "而保存失败没有任何提示"));
        }
        else
        {
            const bool wrote = fputs("ok", f) >= 0;
            fclose(f);
            remove(probe.c_str());
            if (wrote)
            {
                out.push_back(Ok("数据目录可写", "可写（已实际写入并删除一个探针文件）"));
            }
            else
            {
                out.push_back(Fail("数据目录可写", "文件能打开但写入失败 —— 配置不会保存"));
            }
        }
    }
    // ---- 补丁 · 原字节还在吗 ----
    //
    // 补丁是本工具里唯一改**可执行代码**的操作，而它的原字节是
    // **唯一的退路** —— 原字节没了就等于这个补丁永远退不回去。
    // 方法体被重新加载时 methodPointer 会变，那份原字节对应的已经不是
    // 当前代码了（写回去会破坏），所以这两者要分开报。
    {
        size_t patched = 0;
        size_t restorable = 0;
        ClassesTab::PatchStats(&patched, &restorable);
        if (patched > 0)
        {
            char buf[176]{0};
            if (restorable < patched)
            {
                snprintf(buf, sizeof(buf),
                         "%zu 个方法已打补丁，其中 %zu 个无法恢复（方法体已重新加载）", patched,
                         patched - restorable);
                out.push_back(Warn("补丁 · 原字节", buf));
            }
            else
            {
                snprintf(buf, sizeof(buf), "%zu 个方法已打补丁，全部可恢复", patched);
                out.push_back(Ok("补丁 · 原字节", buf));
            }
        }
    }

    // ---- 关注值 ----
    // 关注项各自持有一个 GC 强根。数量失控 = 强根失控 = 游戏对象永远回收不掉，
    // 和「已保存对象」是同一类泄漏，所以放在一起看。
    {
        const size_t watched = ClassesTab::WatchCount();
        if (watched > 0)
        {
            char buf[128]{0};
            snprintf(buf, sizeof(buf), "%zu 项（每项一个 GC 强根，每 200ms 重读一次）", watched);
            out.push_back(Info("关注值", buf));
        }
    }


    // ---- 冻结值 ----
    //
    // 冻结是**每帧**写回，而写回循环过去挂在 DrawWatches() 里 ——
    // 那个函数只在 `CollapsingHeader("关注值")` 展开时才被调用。
    // 于是「折叠关注值」会让所有冻结**静默失效**，而按钮还显示「解冻」。
    // 第 83 轮把循环挪到了 ApplyFreezes()，无条件执行。
    //
    // 这里把「有几个正冻着」摊开：冻结了却一个都没冻住，和根本没冻结过，
    // 在界面上看起来是一样的。
    {
        size_t total = 0;
        size_t frozen = 0;
        size_t frozenInvalid = 0;
        ClassesTab::WatchStats(&total, &frozen, &frozenInvalid);
        if (frozen > 0)
        {
            char buf[192]{0};
            if (frozenInvalid > 0)
            {
                // ApplyFreezes 每帧先跑，正常情况下这里恒为 0。
                snprintf(buf, sizeof(buf),
                         "%zu / %zu 项正在冻结，其中 %zu 项对象已失效（下一帧自动解冻）", frozen, total,
                         frozenInvalid);
                out.push_back(Warn("冻结值", buf));
            }
            else
            {
                snprintf(buf, sizeof(buf), "%zu / %zu 项正在冻结（每帧写回）", frozen, total);
                out.push_back(Ok("冻结值", buf));
            }
        }
        else if (total > 0)
        {
            out.push_back(Info("冻结值", "当前没有冻结项"));
        }
    }
    // ---- 已保存的对象数 ----    // 这些是用户手动标记的，靠 GC 强根保活。数量异常偏大通常意味着
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
    // 注意：ImGui **不解析 markdown**。写 **粗体** 会在界面上显示成
    //  literally 带星号的文本。想强调就用「」或【】。
    ImGui::TextDisabled(
        "自检把「哪些前置条件成立」一次性摊开。多数问题在这个项目里默认是「静默」的"
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
