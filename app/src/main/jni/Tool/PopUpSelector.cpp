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
        ImGui::OpenPopup(needOpen.c_str());
        needOpen = "";
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
        if (ImGui::BeginPopup("EnumSelector")) // assume the current type is enum
        {
            Il2CppType *type = (Il2CppType *)userData;
            auto klass = type->getClass();
            for (auto field : klass->getFields())
            {
                auto fieldType = field->getType();
                if (Il2cpp::GetTypeIsStatic(fieldType) || Il2cpp::GetFieldFlags(field) & FIELD_ATTRIBUTE_STATIC)
                {
                    auto fieldName = field->getName();
                    if (ImGui::Button(fieldName))
                    {
                        Do(fieldName);
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
            ImGui::EndPopup();
        }
    }
}
