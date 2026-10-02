#include "PopUpSelector.h"
#include "Il2cpp/Il2cpp.h"
#include "Il2cpp/il2cpp-tabledefs.h"
#include "Includes/Logger.h"
#include "imgui/imgui.h"

void PopUpSelector::Open(const std::string &type, const std::function<void(const std::string &)> &callback, void *data)
{
    LOGD("Open %s", type.c_str());
    lastCallback = callback;
    needOpen = type;
    userData = data;
}

void PopUpSelector::Update()
{
    static ImGuiIO &io = ImGui::GetIO();
    if (!needOpen.empty())
    {
        // 只有真的还有回调才开弹窗。
        // 否则会出现「弹窗打开了、里面什么都没有、又永远不会被
        // CloseCurrentPopup 关掉」——一个关不掉的空弹窗挡住界面。
        if (!lastCallback)
        {
            LOGW("PopUpSelector: 没有回调却请求打开 %s，已忽略", needOpen.c_str());
            needOpen = "";
        }
        else
        {
            ImGui::OpenPopup(needOpen.c_str());
            needOpen = "";
        }
    }
    if (lastCallback)
    {
        if (ImGui::BeginPopup("BooleanSelector"))
        {
            if (ImGui::Button("True"))
            {
                Do("True");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::Button("False"))
            {
                Do("False");
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0.f), ImVec2(-1, io.DisplaySize.y / 3.f));
        // userData 存的是枚举的 Il2CppType*。Open() 的 data 参数**默认是 nullptr**，
        // 而下面直接 `type->getClass()` —— 任何一次没传 data 的 EnumSelector
        // 打开就是空指针解引用。这里显式挡住。
        if (userData != nullptr)
        {
            Il2CppType *type = (Il2CppType *)userData;
            if (ImGui::BeginPopup("EnumSelector")) // assume the current type is enum
            {
                auto klass = type->getClass();
                if (klass)
                {
                    // getFields 是**不判空**的裸调用，符号为空就是空指针调用
                    // （第 126 轮）。这个下拉是在字段编辑里点出来的，
                    // 用户不会预期「点一下就崩」。
                    if (!Il2cpp::BrowserApiResolved())
                    {
                        ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f),
                                           "这个 Unity 版本缺一部分符号，枚举列表画不出来");
                        // **必须**把状态清掉（第 135 轮）。
                        //
                        // `lastCallback` 平时只在 `Do()` 里清空，而这里我直接
                        // return 了 —— 于是它一直是「已设置」状态。
                        // 后果不是这个弹窗关不掉（EndPopup 已经调了），
                        // 而是**下一次点任何字段都会先走进这个分支**：
                        // `Update()` 每帧都跑，`if (lastCallback)` 一直成立，
                        // 而 `Do()` 再也不会被调用 —— 那个「正在选值」的
                        // 交互状态永远退不出来，用户会以为界面卡住了。
                        //
                        // 顺序照抄 `Do()`（见头文件里那段注释）：
                        // 先把回调摘出来再清空，免得链式选择被抹掉。
                        auto callback = std::move(lastCallback);
                        lastCallback = nullptr;
                        userData = nullptr;
                        ImGui::CloseCurrentPopup();
                        ImGui::EndPopup();
                        LOGE("EnumSelector: 浏览路径符号不全，已取消这次选择");
                        (void)callback;
                        return;
                    }
                    for (auto field : klass->getFields())
                    {
                        if (field == nullptr)
                        {
                            continue;
                        }
                        auto fieldType = field->getType();
                        if (Il2cpp::GetTypeIsStatic(fieldType) || Il2cpp::GetFieldFlags(field) & FIELD_ATTRIBUTE_STATIC)
                        {
                            auto fieldName = field->getName();
                            if (fieldName != nullptr && ImGui::Button(fieldName))
                            {
                                Do(fieldName);
                                ImGui::CloseCurrentPopup();
                            }
                        }
                    }
                }
                ImGui::EndPopup();
            }
        }
        else if (needOpen == "EnumSelector" || lastCallback)
        {
            // userData 没了说明状态不完整。不要静默什么都不做。
            static bool warned = false;
            if (!warned)
            {
                LOGE("EnumSelector 缺少类型信息（userData 为空），已忽略");
                warned = true;
            }
        }
    }
}
