#include "Tool.h"
#include "Il2cpp/Il2cpp.h"
#include "Tool/Frida.h"
#include "Tool/Keyboard.h"
#include "Tool/Util.h"
#include "Tool/ObjectDrawManager.h"
#include "imgui/imgui.h"
#include <future>
#include <set>
#include <span>
#include <Tool/ClassesTab.h>

// __attribute__((visibility("default"))) float placeholder()
// {
//     return 99.99f;
// }
//extern "C" void hookerHandler(void *methodPtr, void *thiz, void **params);
extern ImVec2 initialScreenSize;
extern std::unordered_map<void *, HookerData> hookerMap;
extern std::mutex hookerMtx;

std::vector<Il2CppImage *> g_Images;

CircularBuffer<HookerTrace> HookerData::visited{50};
std::unordered_map<Il2CppClass *, std::set<Il2CppObject *>> HookerData::collectSet{};

namespace Tool
{
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

    std::vector<Il2CppClass *> tracer;

    std::vector<ClassesTab> classesTabs;

    void ConfigLoad()
    {
        LOGD(__FUNCTION__);
        try
        {
            Util::FileReader configFile("class_tabs.json");
            nlohmann::ordered_json j = nlohmann::ordered_json::parse(configFile.read());
            classesTabs = j.template get<std::vector<ClassesTab>>();
        }
        catch (nlohmann::json::exception &e)
        {
            LOGE("Failed to load class_tabs.json: %s", e.what());
            ConfigSave();
        }
    }
    void ConfigSave()
    {
        LOGD(__FUNCTION__);
        Util::FileWriter configFile("class_tabs.json");
        nlohmann::ordered_json j = classesTabs;
        configFile.write(j.dump(2, ' ').c_str());
    }
    void ConfigInit()
    {
        LOGD(__FUNCTION__);
        // check if class_tabs.json exists
        Util::FileReader config("class_tabs.json");
        if (config.exists())
        {
            ConfigLoad();
        }
        else
        {
            ConfigSave();
        }
    }

    void CalculateSomething()
    {
        constexpr auto *placeholder = "BRUH";
        int max = 10;

        for (int i = 0; i < 100; i++)
        {
            auto labelSize = ImGui::CalcTextSize(placeholder);
            ImVec2 labellPos{20, 150 + (labelSize.y * i)};
            if (labellPos.y >= ImGui::GetIO().DisplaySize.y)
            {
                max = i - 5;
                break;
            }
        }
        LOGINT(max);
        HookerData::visited = CircularBuffer<HookerTrace>(max);
    }

    void InitScreenSize()
    {
        auto Display = Il2cpp::FindClass("UnityEngine.Display");
        if (!Display)
        {
            LOGE("Failed to find class 'Display'");
            return;
        }
        auto mainDisplay = Display->invoke_static_method<Il2CppObject *>("get_main");
        if (!mainDisplay)
        {
            LOGE("Failed to get main display");
            return;
        }
        // public System.Int32 get_systemWidth(); // 0x2c2c768
        // public System.Int32 get_systemHeight(); // 0x2c2c860
        auto systemWidth = mainDisplay->invoke_method<int32_t>("get_systemWidth");
        auto systemHeight = mainDisplay->invoke_method<int32_t>("get_systemHeight");

        // auto renderingWidth = mainDisplay->invoke_method<int32_t>("get_renderingWidth");
        // auto renderingHeight = mainDisplay->invoke_method<int32_t>("get_renderingHeight");
        // if (renderingWidth && renderingHeight)
        // {
        //     float scaleX = static_cast<float>(renderingWidth) / systemWidth;
        //     float scaleY = static_cast<float>(renderingHeight) / systemHeight;
        //     float scale = std::min(static_cast<float>(renderingWidth) / systemWidth,
        //                            static_cast<float>(renderingHeight) / systemHeight);
        //     ImGui::GetStyle().ScaleAllSizes(scale);

        //     LOGD("Rendring size: %d x %d", renderingWidth, renderingHeight);
        //     LOGD("Scale: %f %f", scaleX, scaleY);
        // }

        if (systemWidth && systemHeight)
        {
            initialScreenSize.x = systemWidth;
            initialScreenSize.y = systemHeight;
            LOGI("Screen size: %d x %d", systemWidth, systemHeight);
        }
    }

    ClassesTab &GetFirstTab()
    {
        return classesTabs[0];
    }

    ClassesTab &OpenNewTab()
    {
        ClassesTab clone;
        for (auto &c : classesTabs)
        {
            if (c.currentlyOpened)
            {
                clone.selectedImage = c.selectedImage;
                break;
            }
        }
        return classesTabs.emplace_back(clone);
    }
    ClassesTab &OpenNewTabFromClass(Il2CppClass *klass)
    {
        auto &tab = OpenNewTab();
        tab.filter = klass->getFullName();
        tab.selectedImage = klass->getImage();
        tab.FilterClasses(tab.filter);
        return tab;
    }

    void Init(Il2CppImage *image, std::vector<Il2CppImage *> images)
    {
        ConfigInit();
        InitScreenSize();
        
        g_Images = images;
        std::sort(g_Images.begin(), g_Images.end(),
                  [](Il2CppImage *img1, Il2CppImage *img2)
                  { return std::strcmp(img1->getName(), img2->getName()) < 0; });
        classesTabs.reserve(32);
        if (classesTabs.empty())
            OpenNewTab();

        for (ClassesTab &tab : classesTabs)
        {
            tab.FilterClasses(tab.filter);
        }
#ifdef USE_FRIDA
        Frida::Init();
#endif
    }

    void Draw()
    {
        // 对象绘制管理器：开关位于工具页顶部，仅在状态变化时初始化/关闭
        static bool objMgrRunning = false;
        ImGui::Checkbox("对象绘制管理器", &ObjectDrawManager::showObjectManager);
        if (ObjectDrawManager::showObjectManager && !objMgrRunning)
        {
            ObjectDrawManager::Initialize();
            objMgrRunning = true;
        }
        else if (!ObjectDrawManager::showObjectManager && objMgrRunning)
        {
            ObjectDrawManager::Shutdown();
            objMgrRunning = false;
        }

        [[maybe_unused]] static auto _ = []
        {
            CalculateSomething();
            return true;
        }();
        static auto lastUpdate = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate);
        
        if (ImGui::BeginTabBar("tabber", ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_FittingPolicyScroll |
                                             ImGuiTabBarFlags_TabListPopupButton))
        {
            if (ImGui::TabItemButton("+", ImGuiTabItemFlags_NoTooltip | ImGuiTabItemFlags_Leading))
            {
                auto &tab = OpenNewTab();
                tab.FilterClasses(tab.filter);
            }
            // ClassesTab();
            int i = 0;
            auto it = std::begin(classesTabs);
            while (it != std::end(classesTabs))
            {
                if (!it->opened)
                {
                    LOGD("Closing %d", i);
                    it = classesTabs.erase(it);
                    if (classesTabs.empty())
                    {
                        auto &tab = OpenNewTab();
                        tab.FilterClasses(tab.filter);
                        break;
                    }
                    ConfigSave();
                }
                else
                {
                    ImGui::PushID(i);
                    it->Draw(i, true);
                    it->DrawTabMap();
                    ImGui::PopID();
                    ++it;
                    i++;
                }
            }
            ImGui::EndTabBar();
        }
    }

    void Dumper()
    {
        static std::string currentDump = "";
        if (!currentDump.empty())
        {
            ImGui::Text("Dumping %s", currentDump.c_str());
        }

        static bool dumping = false;
        if (ImGui::Button("DUMP"))
        {
            if (dumping)
            {
                currentDump = "are in progress or finished!";
            }
            dumping = true;
        }
        if (dumping)
        {

            static char outFile[256];
            sprintf(outFile, "%s/%s_%s.cs", Il2cpp::getDataPath().c_str(), Il2cpp::getPackageName().c_str(),
                    Il2cpp::getGameVersion().c_str());
            static bool dumped = false;
            static std::future<void> dump =
                std::async(std::launch::async,
                           [] { il2cpp_dump(outFile, [](const char *name, int i, int size) { currentDump = name; }); });
            if (!dumped && dump.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                currentDump = "Done";
                dumped = true;
            }
            if (dumped)
            {
                if (ImGui::Button("复制路径"))
                {
                    Keyboard::Open(outFile, nullptr);
                }
            }
        }
    }

    struct Vec3
    {
        float x, y, z;
    };

    void GameObjects()
    {
        static std::vector<Il2CppObject *> GameObjects = []()
        {
            auto GO = Il2cpp::FindClass("UnityEngine.GameObject");
            return Il2cpp::GC::FindObjects(GO);
        }();
        static std::vector<Il2CppObject *> Transforms = []()
        {
            auto t = Il2cpp::FindClass("UnityEngine.Transform");
            return Il2cpp::GC::FindObjects(t);
        }();
        static Il2CppObject *cam = []()
        {
            auto Camera = Il2cpp::FindClass("UnityEngine.Camera");
            auto cam = Camera->invoke_static_method<Il2CppObject *>("get_current");
            LOGPTR(cam);
            return cam;
        }();
        static MethodInfo *WorldToScreenPoint = []()
        {
            // public UnityEngine.Vector3 WorldToScreenPoint(UnityEngine.Vector3 position); // 0x28c04bc
            auto M = cam->klass->getMethods("WorldToScreenPoint")[1];
            LOGPTR(M);
            LOGPTR(M->methodPointer);
            return M;
        }();

        static MethodInfo *IsNativeObjectAlive = []()
        {
            // private static System.Boolean IsNativeObjectAlive(UnityEngine.Object o); // 0x28c8058
            auto UnityObject = Il2cpp::FindClass("UnityEngine.Object");
            return UnityObject->getMethod("IsNativeObjectAlive");
        }();

        ImGui::Text("GameObjects %zu", GameObjects.size());
        // if (ImGui::Button("CC"))
        // {
        // std::vector<Vec3> vecs;
        // for (auto go : GameObjects)
        // {
        //     auto transform = go->invoke_method<Il2CppObject *>("get_transform");
        //     auto position = transform->invoke_method<ValueType<Vec3>>("get_position");
        //     // auto screen = WorldToScreenPoint->invoke_static<Vec3>(cam, position);
        //     auto arrParam = new Il2CppObject *[1];
        //     arrParam[0] = position.box(Il2cpp::FindClass("UnityEngine.Vector3"));
        //     auto screenObj = Il2cpp::RuntimeInvokeConvertArgs(WorldToScreenPoint, cam, arrParam, 1);
        //     delete[] arrParam;
        //     auto screen = Il2cpp::GetUnboxedValue<Vec3>(screenObj);
        //     vecs.push_back(screen);
        //     // LOGD("%.2f %.2f %.2f | %.2f %.2f %.2f", position.value.x, position.value.y, position.value.z,
        //     // screen.x,
        //     //      screen.y, screen.z);
        // }
        // LOGD("%d", vecs.size());
        auto drawList = ImGui::GetForegroundDrawList();
        static auto center = ImVec2(ImGui::GetIO().DisplaySize.x / 2, ImGui::GetIO().DisplaySize.y / 2);
        for (auto go : GameObjects)
        {
            static bool (*IsAlive)(void *) = (decltype(IsAlive))IsNativeObjectAlive->methodPointer;
            // if (!IsAlive(go))
            // {
            //     continue;
            // }
            if (IsNativeObjectAlive->invoke_static<bool>(go) == false)
            {
                continue;
            }
            auto transform = go->invoke_method<Il2CppObject *>("get_transform");
            auto position = transform->invoke_method<Vec3>("get_position");
            // LOGD("%.2f %.2f %.2f", position.x, position.y, position.z);
            auto screen = WorldToScreenPoint->invoke_static<Vec3>(cam, position);
            // static Vec3 (*WTS)(void *, Vec3) = (decltype(WTS))WorldToScreenPoint->methodPointer;
            // auto screen = WTS(cam, position);
            ImVec2 pos = ImVec2(screen.x, screen.y);
            drawList->AddLine(center, pos, IM_COL32(255, 50, 50, 255));

            // LOGD("%.2f %.2f %.2f | %.2f %.2f %.2f", position.x, position.y, position.z, screen.x, screen.y,
            //      screen.z);
        }
        // }
    }

    //-1 = Auto
    // 0 = Off
    // 1 = On
    bool ToggleHooker(MethodInfo *method, int state) {
    bool patched = ClassesTab::oMap[method].bytes.empty() == false;
    if (patched) {
        LOGE("Can't hook while patched!");
        return false;
    }

    static auto printHex = [](void *ptr, int row = 1) {
        if (row < 1) row = 1;
        for (int i = 0; i < row; i++) {
            char buffer[512]{0};
            std::span<uint8_t> bytes((uint8_t *)ptr + i * 16, 16);
            for (int j = 0; j < bytes.size(); j++) {
                sprintf(buffer + j * 3, "%02X ", bytes[j]);
            }
            LOGD("%s", buffer);
        }
    };

    auto it = hookerMap.find(method->methodPointer);
    bool hooked = it != hookerMap.end();

    auto EnableHooker = [&method]() -> bool {
        LOGD("%s", method->getName());
        printHex(method->methodPointer);
        std::span<uint8_t> originalBytes((uint8_t *)method->methodPointer, (uint8_t *)method->methodPointer + 8);
#ifdef __aarch64__
        constexpr std::array<uint8_t, 4> ret = {0xC0, 0x03, 0x5F, 0xD6};
        auto it = std::search(originalBytes.begin(), originalBytes.end(), ret.begin(), ret.end());
#else
        constexpr std::array<uint8_t, 4> bxLr = {0x1E, 0xFF, 0x2F, 0xE1};
        auto it = std::search(originalBytes.begin(), originalBytes.end(), bxLr.begin(), bxLr.end());
#endif
        auto shortFunction = it != originalBytes.end();
        if (shortFunction) {
            dobby_enable_near_branch_trampoline();
        }

if (DobbyInstrument((void *)method->methodPointer, (dobby_instrument_callback_t)hookerHandler) == 0) {
            printHex(method->methodPointer);
            std::lock_guard guard(hookerMtx);
            hookerMap[method->methodPointer].hitCount = 0;
            hookerMap[method->methodPointer].method = method;
            return true;
        } else {
            LOGE("Failed to instrument %s", method->getName());
            return false;
        }

        if (shortFunction) {
            dobby_disable_near_branch_trampoline();
        }
    };

    auto DisableHooker = [&method]() -> bool {
        if (DobbyDestroy(method->methodPointer) == 0) {
            std::lock_guard guard(hookerMtx);
            hookerMap.erase(method->methodPointer);
            return true;
        } else {
            LOGE("Failed to restore %s", method->getName());
            return false;
        }
    };

    if (state == -1) {
        if (!hooked) {
            return EnableHooker();
        } else {
            return DisableHooker();
        }
    } else if (state == 0) {
        if (hooked) {
            return DisableHooker();
        }
    } else if (state == 1) {
        if (!hooked) {
            return EnableHooker();
        }
    }
    return false;
}
}
