#include "ClassesTab.h"
#include "Il2cpp/Il2cpp.h"
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

extern std::vector<Il2CppImage *> g_Images;
extern Il2CppImage *g_Image;

// 「Find Objects」后台扫描的结果交接：
// 后台线程只往 g_pendingScanResults 写，真正并入 objectMap 由 UI 线程做。
// objectMap 里的 vector 在 UI 里是被边遍历边 erase 的，如果让后台线程直接
// objectMap[klass] = ...，UI 手上的引用会被整个换掉 → 迭代野指针 / UAF。
static std::mutex g_scanResultMutex;
static std::unordered_map<Il2CppClass *, std::vector<Il2CppObject *>> g_pendingScanResults;

constexpr int MAX_CLASSES = 500;

int maxLine{5};
std::unordered_map<void *, HookerData> hookerMap;
std::mutex hookerMtx;

#ifndef USE_FRIDA
void hookerHandler(void *address, DobbyRegisterContext *ctx)
{
    std::lock_guard guard(hookerMtx);
    if (hookerMap.find(address) != hookerMap.end())
    {
        auto &hookerData = hookerMap[address];
        hookerData.hitCount++;
        hookerData.time = 1.f;

        auto name = hookerData.method->getName();
        // 方法名长度不受控（混淆过的 il2cpp 元数据可以很长），
        // 128 字节固定缓冲 + 无界 sprintf 就是栈溢出。
        char buffer[512]{0};
        snprintf(buffer, sizeof(buffer), "%p | %s", (void *)hookerData.method->getAbsAddress(), name);
        if (!HookerData::visited.empty())
        {
            // auto &back = HookerData::visited.back();
            // if (back.name == name)
            // {
            //     back.goneTime = 10.f;
            //     back.time = 2.f;
            //     back.hitCount++;
            //     return;
            // }
            int i = 0;
            for (auto it = HookerData::visited.rbegin(); it != HookerData::visited.rend(); ++it)
            {
                if (i >= maxLine)
                {
                    break;
                }
                if (it->name == buffer)
                {
                    it->goneTime = 10.f;
                    it->time = 2.f;
                    it->hitCount++;
                    return;
                }
                i++;
            }
        }
        HookerData::visited.push_back({buffer, 2.f, 10.f, 0});
        // LOGD("%s", hookerData.method->getName());
    }
}
#endif

ClassesTab::MethodList &ClassesTab::buildMethodMap(Il2CppClass *klass)
{
    static Il2CppClass *lastClass = nullptr;
    static MethodList methodList;

    if (lastClass != klass)
    {
        methodList.clear();
        auto methods = klass->getMethods();
        LOGD("Rebuilding %s | %lu methods", klass->getName(), methods.size());
        for (auto method : methods)
        {
            auto paramsInfo = method->getParamsInfo();
            methodList.push_back({method, paramsInfo});
        }
        LOGD("Rebuilt %lu methods", methodList.size());
        lastClass = klass;
    }
    return methodList;
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
    // g_Image 为空时（il2cpp 还没就绪 / 没有可用 assembly）直接 getClasses() 是空指针解引用。
    // 这里留空列表，让 UI 后续自己判空，而不是在构造期就崩。
    classes = selectedImage ? selectedImage->getClasses() : std::vector<Il2CppClass *>{};
    filteredClasses = classes;
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

void ClassesTab::ImGuiObjectSelector(int id, Il2CppClass *klass, const char *prefix,
                                     std::function<void(Il2CppObject *)> onSelect, bool canNew)
{
    ImGui::PushID(id);
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
        std::thread(
            [keepAlive](Il2CppClass *klass)
            {
                // 后台线程是 il2cpp 的 foreign thread：GC::FindObjects 会 stop_gc_world
                // 并遍历 GC 结构，不 attach 就是崩溃/静默错数据；用完必须 detach，
                // 否则 il2cpp 的 attached-thread 表里会留下悬空条目。
                if (Il2cpp::EnsureAttached())
                {
                    auto objs = Il2cpp::GC::FindObjects(klass);
                    {
                        std::lock_guard<std::mutex> lock(g_scanResultMutex);
                        g_pendingScanResults[klass] = std::move(objs);
                    }
                    Il2cpp::Detach();
                }
                keepAlive->store(false);
            },
            klass)
            .detach();
    }
    ImGui::PopID();

    // 把后台线程的结果并进来（objectMap 只在 UI 线程被改动）
    {
        std::lock_guard<std::mutex> lock(g_scanResultMutex);
        if (!g_pendingScanResults.empty())
        {
            for (auto &[pendingKlass, pending] : g_pendingScanResults)
            {
                objectMap[pendingKlass] = std::move(pending);
            }
            g_pendingScanResults.clear();
        }
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
                if (objects.size() > 100)
                {
                    ImGui::Text("Showing 100 of %zu objects", objects.size());
                }
                for (auto it = objects.begin(); it != (objects.size() > 100 ? objects.begin() + 100 : objects.end());)
                {
                    auto object = *it;
                    char buff[64];
                    sprintf(buff, "%s [%p]", prefix, object->klass->getName());
                    auto size = ImGui::GetWindowSize();
                    if (ImGui::Button(buff, ImVec2(size.x / 1.5, 0)))
                    {
                        onSelect(object);
                    }
                    ImGui::SetItemTooltip("%s", object->klass->getName());
                    ImGui::SameLine();
                    ImGui::PushID(buff);
                    if (ImGui::Button("Remove"))
                    {
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
        char buffer[128];
        sprintf(buffer, "Inherited from %s", klass->getName());
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
                            auto object = *it;
                            char buff[64];
                            sprintf(buff, "%s [%p]", setKlass->getName(), object);
                            auto size = ImGui::GetWindowSize();
                            if (ImGui::Button(buff, ImVec2(size.x / 1.5, 0)))
                            {
                                onSelect(object);
                            }
                            ImGui::SetItemTooltip("%s", object->klass->getName());
                            ImGui::SameLine();
                            ImGui::PushID(buff);
                            if (ImGui::Button("Remove"))
                            {
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
                        auto object = *it;
                        char buff[64];
                        sprintf(buff, "%s [%p]", klass->getName(), object);
                        auto size = ImGui::GetWindowSize();
                        if (ImGui::Button(buff, ImVec2(size.x / 1.5, 0)))
                        {
                            onSelect(object);
                        }
                        ImGui::SetItemTooltip("%s", object->klass->getName());
                        ImGui::SameLine();
                        ImGui::PushID(buff);
                        if (ImGui::Button("Remove"))
                        {
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
                        auto object = *it;
                        char buff[64];
                        sprintf(buff, "%s [%p]", klass->getName(), object);
                        auto size = ImGui::GetWindowSize();
                        if (ImGui::Button(buff, ImVec2(size.x / 1.5, 0)))
                        {
                            onSelect(object);
                        }
                        ImGui::SetItemTooltip("%s", object->klass->getName());
                        ImGui::SameLine();
                        ImGui::PushID(buff);
                        if (ImGui::Button("Remove"))
                        {
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
                    for (auto it = objects.begin(); it != objects.end();)
                    {
                        auto object = *it;
                        char buff[64];
                        sprintf(buff, "%s [%p]", prefix, object);
                        auto size = ImGui::GetWindowSize();
                        if (ImGui::Button(buff, ImVec2(size.x / 1.5, 0)))
                        {
                            onSelect(object);
                        }
                        ImGui::SetItemTooltip("%s", object->klass->getName());
                        ImGui::SameLine();
                        ImGui::PushID(buff);
                        if (ImGui::Button("Remove"))
                        {
                            it = objects.erase(it);
                        }
                        else
                        {
                            ++it;
                        }
                        ImGui::PopID();
                    }
                }
                if (canNew)
                {
                    if (ImGui::Button("New"))
                    {
                        auto newObject = klass->New();
                        newObjectMap[klass].push_back(newObject);
                        if (Il2cpp::GetClassType(klass)->isValueType())
                        {
                            // Il2cpp::GC::KeepAlive(newObject);
                            newObject = (Il2CppObject *)Il2cpp::GetUnboxedValue(newObject);
                        }
                        onSelect(newObject);
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

void ClassesTab::CallerView(Il2CppClass *klass, MethodInfo *method, const MethodParamList &paramsInfo,
                            Il2CppObject *thiz)
{
    static ImGuiIO &io = ImGui::GetIO();
    bool methodIsStatic = Il2cpp::GetIsMethodStatic(method);
    auto &params = paramMap[method];
    if (!methodIsStatic && !thiz)
    {
        auto &thisParam = params["this"];
        // 类名长度不受控，param.value 又是用户输入；固定 128 字节缓冲 + 无界 sprintf 会栈溢出。
        // 另外旧写法 sprintf(dst, "%s = %s", dst, ...) 把 dst 同时当源和目标，是未定义行为。
        std::string thisLabel = std::string(Il2cpp::GetClassType(klass)->getName()) + " this";
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
            ImGuiObjectSelector(
                ImGui::GetID("ThisObjectSelector"), klass, "this",
                [&thisParam](Il2CppObject *object)
                {
                    // 64 位下 "%p" 要 "0x" + 16 位十六进制 + '\0' = 19 字节，
                    // 旧的 char[16] 必然溢出 3 字节。
                    char objStr[32]{0};
                    snprintf(objStr, sizeof(objStr), "%p", (const void *)object);
                    thisParam.value = objStr;
                    thisParam.object = object;
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
                    poper.Open("BooleanSelector", [&param](const std::string &result) { param.value = result; });
                }
                else
                {
                    Keyboard::Open(
                        [&param, &isString](const std::string &text)
                        {
                            if (isString)
                            {
                                param.object = Il2cpp::NewString(text.c_str());
                            }
                            param.value = text;
                        });
                }
            }
            else if (type->isEnum())
            {
                poper.Open(
                    "EnumSelector", [&param](const std::string &result) { param.value = result; }, type);
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
            ImGuiObjectSelector(ImGui::GetID("ParamObjectSelector"), type->getClass(), name,
                                [&param](Il2CppObject *object)
                                {
                                    // 同上：64 位 %p 需要 19 字节，char[16] 必溢出
                                    char objStr[32]{0};
                                    snprintf(objStr, sizeof(objStr), "%p", (const void *)object);
                                    param.value = objStr;
                                    param.object = object;
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
        auto params = paramMap[method];
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
                auto parse = [&](auto tag, auto fn) {
                    using T = decltype(tag);
                    T raw{};
                    try
                    {
                        raw = fn(param.value);
                    }
                    catch (const std::exception &e)
                    {
                        LOGE("参数 %s 的值 \"%s\" 无法解析为数值: %s", name, param.value.c_str(), e.what());
                        parseFailed = true;
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
                    parse(int{}, [](const std::string &s) { return std::stoi(s); });
                else if (strcmp(type->getName(), "System.Int64") == 0)
                    parse(int64_t{}, [](const std::string &s) { return std::stoll(s); });
                else if (strcmp(type->getName(), "System.UInt32") == 0)
                    parse(uint32_t{}, [](const std::string &s) { return (uint32_t)std::stoul(s); });
                else if (strcmp(type->getName(), "System.UInt64") == 0)
                    parse(uint64_t{}, [](const std::string &s) { return std::stoull(s); });
                else if (strcmp(type->getName(), "System.Single") == 0)
                    parse(float{}, [](const std::string &s) { return std::stof(s); });
                else if (strcmp(type->getName(), "System.Double") == 0)
                    parse(double{}, [](const std::string &s) { return std::stod(s); });
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
                        arrayParams[k] = field->getStaticValue<ValueType<int>>().box(enumClass);
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
            if (strcmp(method->getName(), ".ctor") != 0 && thisParam &&
                Il2cpp::GetClassType(thisParam->klass)->isValueType())
            {
                auto thizz = Il2cpp::GetUnboxedValue(thisParam);
                result = Il2cpp::RuntimeInvokeConvertArgs(method, thizz, arrayParams, paramsInfo.size());
            }
            else
            {
                result = Il2cpp::RuntimeInvokeConvertArgs(method, thisParam, arrayParams, paramsInfo.size());
            }
            LOGPTR(result);
            if (result && strcmp(method->getName(), ".ctor") != 0)
            {
                auto resultType = Il2cpp::GetClassType(result->klass);
                if (resultType->isPrimitive())
                {
                    std::vector<uintptr_t> visited;
                    auto j = result->dump(visited, 1);
                    callResults.at(method).push_back(std::pair{j.begin().value().dump(), nullptr});
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
                            callResults.at(method).push_back({"the call returned null", result});
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
                savedSet[resultType->getClass()].insert(result);
                // setJsonObject(result);
            }
            else
            {
                callResults.at(method).push_back({"the call returned null", nullptr});
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
    return hookerMap.find(method->methodPointer) != hookerMap.end();
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
                o.bytes = p.patch();
            }
        }
        else
        {
            if (ImGui::Button("Restore"))
            {
                memcpy(method->methodPointer, o.bytes.data(), o.bytes.size());
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
        char label[64]{0};
        if (!o.bytes.empty())
        {
            sprintf(label, "%s", "Restore");
        }
        else
        {
            sprintf(label, "%s", type->getName());
        }
        if (ImGui::Button(label))
        {
            if (!o.bytes.empty())
            {
                memcpy(method->methodPointer, o.bytes.data(), o.bytes.size());
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
                                       using namespace asmjit;
                                       Patcher p{method};
                                       bool value = b == "True";
                                       p.movBool(value);
                                       p.ret();

                                       if (oMap[method].bytes.empty())
                                       {
                                           oMap[method].bytes = p.patch();
                                           oMap[method].text = b;
                                       }
                                       else
                                       {
                                           LOGE("oMap is not empty for %s", method->getName());
                                       }
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
                                Patcher p{method};
                                // auto &assembler = p.assembler;
                                if (strcmp(type->getName(), "System.Int16") == 0)
                                {
                                    int16_t value = std::stoi(text);
                                    p.movInt16(value);
                                }
                                else if (strcmp(type->getName(), "System.UInt16") == 0)
                                {
                                    unsigned short value = std::stoi(text);
                                    p.movUInt16(value);
                                }
                                else if (strcmp(type->getName(), "System.Int32") == 0)
                                {
                                    int value{std::stoi(text)};
                                    p.movInt32(value);
                                }
                                else if (strcmp(type->getName(), "System.UInt32") == 0)
                                {
                                    unsigned int value{static_cast<unsigned int>(std::stoul(text))};
                                    p.movUInt32(value);
                                }
                                else if (strcmp(type->getName(), "System.Int64") == 0)
                                {
                                    long value{std::stol(text)};
                                    p.movInt64(value);
                                }
                                else if (strcmp(type->getName(), "System.UInt64") == 0)
                                {
                                    unsigned long value{std::stoul(text)};
                                    p.movUInt64(value);
                                }
                                else if (strcmp(type->getName(), "System.Single") == 0)
                                {
                                    float value = std::stof(text);
                                    p.movFloat(value);
                                }
                                else if (isString)
                                {
                                    p.movPtr(Il2cpp::NewString(text.c_str()));
                                }

                                p.ret();

                                if (oMap[method].bytes.empty())
                                {
                                    oMap[method].bytes = p.patch();
                                    oMap[method].text = text;
                                }
                                else
                                {
                                    LOGE("oMap is not empty for %s", method->getName());
                                }
                            });
                    }
                }
                else if (type->isEnum())
                {
                    poper.Open(
                        "EnumSelector",
                        [method, type](const std::string &result)
                        {
                            int value = type->getClass()->getField(result.c_str())->getStaticValue<int>();

                            using namespace asmjit;
                            Patcher p{method};
                            p.movInt16(value);
                            p.ret();

                            if (oMap[method].bytes.empty())
                            {
                                oMap[method].bytes = p.patch();
                                oMap[method].text = result;
                            }
                            else
                            {
                                LOGE("oMap is not empty for %s", method->getName());
                            }
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
    auto it = hookerMap.find(method->methodPointer);
    bool hooked = it != hookerMap.end();
    char label[16];
    if (!hooked)
    {
        sprintf(label, "Trace");
    }
    else
    {
        sprintf(label, "Restore");
        auto value = it->second.hitCount;
        ImGui::Text("Hit Count %d", value);
        ImGui::Separator();
    }
    if (ImGui::Button(label))
    {
        Tool::ToggleHooker(method);
        it = hookerMap.find(method->methodPointer);
        hooked = it != hookerMap.end();
    }
    ImGui::Separator();
    if (hooked)
    {
        auto &backtraced = it->second.backtraced;
        if (!it->second.backtracing)
        {
            if (ImGui::Button("Backtrace"))
            {
                it->second.backtracing = true;
            }
        }

        if (backtraced.empty())
        {
            ImGui::Text("Method has not been called");
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

    char treeLabel[512]{0};
    sprintf(treeLabel, "%s %s(%zu)###", method->getReturnType()->getName(), method->getName(), paramsInfo.size());
    int pushedColor = 0;
    if (methodIsStatic)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 200, 100, 255));
        pushedColor++;
        Util::prependStringToBuffer(treeLabel, "static ");
    }
    bool patched = oMap[method].bytes.empty() == false;
    bool hooked = hookerMap.find(method->methodPointer) != hookerMap.end();
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
            int hitCount = hookerMap[method->methodPointer].hitCount;
            char hitLabel[64]{0};
            sprintf(hitLabel, "Hit Count %d | ", hitCount);
            Util::prependStringToBuffer(treeLabel, hitLabel);
        }
        else if (patched)
        {
            auto text = oMap[method].text;
            if (!text.empty())
            {
                char buff[64]{0};
                sprintf(buff, "Returns %s | ", text.c_str());
                Util::prependStringToBuffer(treeLabel, buff);
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
    static std::unordered_map<MethodInfo *, MethodParamList> params;
    if (params.find(method) == params.end())
    {
        params[method] = method->getParamsInfo();
    }
    return params[method];
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
                std::thread hookThread(
                    [this, klass]
                    {
                        for (auto &[method, paramsInfo] : methodMap[klass])
                        {
                            if (!method->methodPointer && !Il2cpp::GetIsMethodInflated(method))
                                continue;

                            Tool::ToggleHooker(method, 1);
                        }
                    });
                hookThread.detach();
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
    char tabLabel[256];
    if (filter.empty())
    {
        sprintf(tabLabel, "Classes");
        if (index >= 0)
            sprintf(tabLabel, "Classes [%d]", index + 1);
    }
    else
    {
        sprintf(tabLabel, "%s", filter.c_str());
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

        char filterBuffer[256];
        sprintf(filterBuffer, "Filter : %s | %zu of %zu", filter.empty() ? "(none)" : filter.c_str(),
                filteredClasses.size(), classes.size());
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
                        std::thread hookThread(
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
                            });
                        hookThread.detach();
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
                    std::thread hookThread(
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
                        });
                    hookThread.detach();
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
            ImGui::Text("Filter by ");
            ImGui::SameLine();
            if (ImGui::RadioButton("Class", filterByClass == true))
            {
                filterByClass = true;
                filterByMethod = false;
                filterByField = false;
                FilterClasses(filter);
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Method", filterByMethod == true))
            {
                filterByClass = false;
                filterByMethod = true;
                filterByField = false;
                FilterClasses(filter);
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Field", filterByField == true))
            {
                filterByClass = false;
                filterByMethod = false;
                filterByField = true;
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
            for (int i = 0; i < filteredClasses.size(); i++)
            {
                auto klass = filteredClasses[i];

                bool isValueType = Il2cpp::GetClassType(klass)->isValueType();
                int pushedColor = 0;
                if (isValueType)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(222, 222, 222, 255));
                    pushedColor++;
                }
                bool collapsingHeader = ImGui::CollapsingHeader(klass->getFullName().c_str());
                if (filterByMethod && ImGui::IsItemHeld(0.7f))
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
            ImGui::ScrollWhenDraggingOnVoid();
            ImGui::EndChild();
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
        sprintf(buff, "[%p]", object);

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

void ensureIfValueType(Il2CppObject *currentObj, std::vector<std::string> &paths, Il2CppObject *rootObj)
{
    if (currentObj == rootObj)
        return;
    bool isValueType = Il2cpp::GetClassType(currentObj->klass)->isValueType();
    if (isValueType)
    {
        if (paths.size() > 1)
        {
            auto pathsButLast = std::vector(paths.begin(), paths.end() - 1);
            auto [beforeObject, j] = rootObj->dump(pathsButLast, true);

            auto path = paths.rbegin();
            std::istringstream iss(path->c_str());
            std::string _, val;
            iss >> _ >> val;
            void *unboxed = Il2cpp::GetUnboxedValue(currentObj);
            // object->setField(val.c_str(), unboxed);
            auto f = beforeObject->klass->getField(val.c_str());
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
            // object->setField(val.c_str(), unboxed);
            auto f = rootObj->klass->getField(val.c_str());
            Il2cpp::SetFieldValue(rootObj, f, unboxed);
        }
    }
}

void ClassesTab::ImGuiJson(Il2CppObject *rootObj)
{
    // auto &paths = tool.dataMap[object].second;
    auto &paths = getJsonPaths(rootObj);

    int indentCounter = 0;
    auto currentObj = dataMap[rootObj].first.first;
    for (auto it = paths.begin() + (paths.size() > 3 ? paths.size() - 4 : 0); it != paths.end(); ++it)
    {
        const bool isLast = std::next(it) == paths.end();
        auto key = it->c_str();
        ImGui::PushID(indentCounter);

        bool buttonPressed = false;
        bool isValueType = Il2cpp::GetClassType(currentObj->klass)->isValueType();
        if (isLast && isValueType)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(222, 222, 222, 255));
        }
        if (isLast && currentObj && savedSet[currentObj->klass].count(currentObj) == 0)
        {
            auto width = ImGui::CalcTextSize("Save").x + ImGui::GetStyle().FramePadding.x * 5.f;
            buttonPressed = ImGui::Button(key, ImVec2(ImGui::GetContentRegionAvail().x - width, 0));
            if (ImGui::IsItemHeld())
            {
                Tool::OpenNewTabFromClass(currentObj->klass);
                LOGD("OpenNewTabFromObject %p", currentObj);
            }
            ImGui::SameLine();
            char buttonLabel[32]{0};
            sprintf(buttonLabel, "Save");
            if (ImGui::Button(buttonLabel,
                              ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x, 0)))
            {
                savedSet[currentObj->klass].insert(currentObj);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%p", currentObj);
            }
        }
        else
        {
            buttonPressed =
                ImGui::Button(key, ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x, 0));
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
    static bool doRefresh = false;
    if (ImGui::Button("Refresh", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)))
    {
        doRefresh = true;
    }
    if (doRefresh)
    {
        doRefresh = false;
        dataMap[rootObj].first = rootObj->dump(paths);
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
                else if (ImGui::Button(key.c_str(), ImVec2(key.length() <= 3 ? ImGui::GetContentRegionAvail().x -
                                                                                   ImGui::GetStyle().FramePadding.x
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
                            [type = std::move(type), val = std::move(val), currentObj](const std::string &value)
                            {
                                LOGD("%s", value.c_str());
                                auto f = currentObj->klass->getField(val.c_str());
                                auto newStr = Il2cpp::NewString(value.c_str());
                                // Il2cpp::SetFieldValueObject(currentObj, f, newStr);
                                Il2cpp::SetFieldValue(currentObj, f, newStr);
                                doRefresh = true;
                            });
                    }
                    else
                    {
                        auto field = currentObj->klass->getField(val.c_str());
                        auto fieldType = field->getType();
                        if (fieldType->isEnum())
                        {
                            poper.Open(
                                "EnumSelector",
                                [fieldType, currentObj, field](const std::string &result)
                                {
                                    int value = fieldType->getClass()->getField(result.c_str())->getStaticValue<int>();
                                    Il2cpp::SetFieldValue(currentObj, field, &value);
                                    doRefresh = true;
                                },
                                fieldType);
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
                               [currentObj, val, &paths, rootObj](const std::string &value)
                               {
                                   bool b = value == "True";
                                   // split key by space
                                   currentObj->setField(val.c_str(), (int)b);
                                   ensureIfValueType(currentObj, paths, rootObj);
                                   doRefresh = true;
                               });
                }
            }
            else if (value.is_number_float())
            {
                ImGui::Text("%s = %f", key.c_str(), value.get<float>());

                if (ImGui::IsItemClicked())
                {
                    std::istringstream iss(key);
                    std::string type, val;
                    iss >> type >> val;
                    Keyboard::Open(std::to_string(value.get<float>()).c_str(),
                                   [type, currentObj, val, &paths, rootObj](const std::string &text)
                                   {
                                       if (strcmp(type.c_str(), "Single") == 0)
                                       {
                                           float value = std::stof(text);
                                           currentObj->setField(val.c_str(), value);
                                       }
                                       else if (strcmp(type.c_str(), "Double") == 0)
                                       {
                                           double value = std::stod(text);
                                           currentObj->setField(val.c_str(), value);
                                       }
                                       ensureIfValueType(currentObj, paths, rootObj);
                                       doRefresh = true;
                                   });
                }
            }
            else if (value.is_number())
            {
                ImGui::Text("%s = %d", key.c_str(), value.get<int>());
                if (ImGui::IsItemClicked())
                {
                    std::istringstream iss(key);
                    std::string type, val;
                    iss >> type >> val;
                    Keyboard::Open(std::to_string(value.get<int>()).c_str(),
                                   [type, currentObj, val, &paths, rootObj](const std::string &text)
                                   {
                                       if (strcmp(type.c_str(), "Int16") == 0)
                                       {
                                           int16_t value = std::stoi(text);
                                           currentObj->setField(val.c_str(), value);
                                       }
                                       else if (strcmp(type.c_str(), "UInt16") == 0)
                                       {
                                           uint16_t value = std::stoi(text);
                                           currentObj->setField(val.c_str(), value);
                                       }
                                       else if (strcmp(type.c_str(), "Int32") == 0)
                                       {
                                           int32_t value = std::stoi(text);
                                           currentObj->setField(val.c_str(), value);
                                       }
                                       else if (strcmp(type.c_str(), "UInt32") == 0)
                                       {
                                           uint32_t value = std::stoul(text);
                                           currentObj->setField(val.c_str(), value);
                                       }
                                       else if (strcmp(type.c_str(), "Int64") == 0)
                                       {
                                           int64_t value = std::stoll(text);
                                           currentObj->setField(val.c_str(), value);
                                       }
                                       else if (strcmp(type.c_str(), "UInt64") == 0)
                                       {
                                           uint64_t value = std::stoull(text);
                                           currentObj->setField(val.c_str(), value);
                                       }
                                       ensureIfValueType(currentObj, paths, rootObj);
                                       doRefresh = true;
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
        if (ImGui::Button("Methods", ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x, 0)))
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
                ImGui::Text("No methods for class %s", currentObj->klass->getName());
            }
            else
            {
                int j = 0;
                for (auto &[method, paramsInfo] : methods)
                {
                    ImGui::PushID(method + j++);
                    MethodViewer(currentObj->klass, method, paramsInfo, currentObj, true);
                    ImGui::Separator();
                    ImGui::PopID();
                }
            }
            ImGui::EndPopup();
        }

        if (ImGui::Button("Dump to file",
                          ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x, 0)))
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
        poper.Update();
        ImGui::EndChild();
        ImGui::EndTable();
    }
}

void ClassesTab::FilterClasses(const std::string &filter)
{
    filteredClasses.clear();
    classes.clear();
    methodMap.clear(); // is this necessary?
    if (includeAllImages)
    {
        for (auto image : g_Images)
        {
            auto imageClasses = image->getClasses();
            classes.insert(classes.end(), imageClasses.begin(), imageClasses.end());
        }
    }
    else
    {
        classes = selectedImage->getClasses();
    }

    auto finderCaseSensitive = [](const std::string &a, const std::string &b)
    { return a.find(b) != std::string::npos; };

    auto finderCaseInsensitive = [](const std::string &a, const std::string &b)
    {
        auto newA = a;
        auto newB = b;
        std::transform(newA.begin(), newA.end(), newA.begin(), ::tolower);
        std::transform(newB.begin(), newB.end(), newB.begin(), ::tolower);
        return newA.find(newB) != std::string::npos;
    };

    auto finder = caseSensitive ? finderCaseSensitive : finderCaseInsensitive;

    for (int i = 0; i < classes.size() && filteredClasses.size() < (showAllClasses ? classes.size() : MAX_CLASSES); i++)
    {
        auto klass = classes[i];

        if (/* Il2cpp::GetClassIsStatic(klass) || */ Il2cpp::GetClassIsEnum(klass))
            continue;

        if (filterByClass)
        {
            // if (klass->getFullName().find(filter) != std::string::npos)
            if (finder(klass->getFullName(), filter))
            {
                filteredClasses.push_back(klass);
                for (auto m : klass->getMethods())
                {
                    auto paramsInfo = m->getParamsInfo();
                    methodMap[klass].push_back({m, paramsInfo});
                }
            }
        }
        else if (filterByMethod)
        {
            bool found = false;
            for (auto m : klass->getMethods())
            {
                // if (std::string(m->getName()).find(filter) != std::string::npos)
                if (finder(m->getName(), filter))
                {
                    found = true;
                    auto paramsInfo = m->getParamsInfo();
                    methodMap[klass].push_back({m, paramsInfo});
                }
            }
            if (found)
                filteredClasses.push_back(klass);
        }
        else if (filterByField)
        {
            bool found = false;
            for (auto f : klass->getFields())
            {
                // if (std::string(f->getName()).find(filter) != std::string::npos)
                if (finder(f->getName(), filter))
                {
                    found = true;
                }
            }
            if (found)
            {
                filteredClasses.push_back(klass);
                for (auto m : klass->getMethods())
                {
                    auto paramsInfo = m->getParamsInfo();
                    methodMap[klass].push_back({m, paramsInfo});
                }
            }
        }
    }
    Tool::ConfigSave();
}

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

std::unordered_map<Il2CppClass *, std::vector<Il2CppObject *>> ClassesTab::objectMap;
std::unordered_map<Il2CppClass *, std::vector<Il2CppObject *>> ClassesTab::newObjectMap;
std::unordered_map<Il2CppClass *, std::set<Il2CppObject *>> ClassesTab::savedSet;
std::unordered_map<MethodInfo *, ClassesTab::OriginalMethodBytes> ClassesTab::oMap;
std::unordered_map<Il2CppClass *, bool> ClassesTab::states;
PopUpSelector ClassesTab::poper;
