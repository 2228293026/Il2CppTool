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
        // 必须夹紧：旧代码直接用 i-5，小屏/低分辨率时 i 很小 → max 变成 0 甚至负数。
        // CircularBuffer(0) 之后内部 (_tail+1)%_max_size 直接除零崩掉；
        // 负数转成 size_t 更是天文数字大小的分配。
        max = std::clamp(max, 5, 200);

        // 重建会整体换掉底层数组，而 hookerHandler 正在另一个线程往 visited 里 push。
        // CircularBuffer 的移动赋值完全没加锁，并发重建就是 use-after-free。
        // hookerHandler 持 hookerMtx，这里也持有即可与之串行化。
        std::lock_guard guard(hookerMtx);
        if (HookerData::visited.capacity() == static_cast<size_t>(max))
            return; // 容量没变就别重建
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

    namespace
    {
        // Dumper 的共享状态。旧实现里 currentDump 是渲染线程读、dump 线程写的
        // std::string —— 并发读写 std::string 会破坏堆（realloc 时可能一边
        // 释放一边被另一侧解引用）。
        std::mutex g_dumpMutex;
        std::string g_dumpProgress;
        std::string g_dumpOutputPath;
    } // namespace

    void Dumper()
    {
        {
            std::lock_guard guard(g_dumpMutex);
            if (!g_dumpProgress.empty())
            {
                ImGui::Text("Dumping %s", g_dumpProgress.c_str());
            }
        }

        static bool dumping = false;
        if (ImGui::Button("DUMP"))
        {
            if (dumping)
            {
                std::lock_guard guard(g_dumpMutex);
                g_dumpProgress = "are in progress or finished!";
            }
            else
            {
                dumping = true;
            }
        }
        if (dumping)
        {
            // 路径长度不受控（包名 + 数据目录 + 版本号），固定缓冲 + 无界
            // sprintf 就是栈溢出。std::string 让它自然增长。
            static char outFile[1024];
            snprintf(outFile, sizeof(outFile), "%s/%s_%s.cs", Il2cpp::getDataPath().c_str(),
                     Il2cpp::getPackageName().c_str(), Il2cpp::getGameVersion().c_str());

            static bool dumped = false;
            static std::future<void> dump = std::async(std::launch::async, [](const char *outPath) {
                // dump 线程是 il2cpp 的 foreign thread：il2cpp_dump 会遍历
                // domain / assembly / class，不 attach 到 VM 就是崩溃或错数据。
                // 用完必须 detach，否则 attached-thread 表里留悬空条目。
                if (!Il2cpp::EnsureAttached())
                {
                    std::lock_guard guard(g_dumpMutex);
                    g_dumpProgress = "failed to attach to il2cpp VM";
                    return;
                }
                struct DetachGuard
                {
                    ~DetachGuard() { Il2cpp::Detach(); }
                } detachGuard;

                try
                {
                    il2cpp_dump(outPath, [](const char *name, int i, int size) {
                        std::lock_guard guard(g_dumpMutex);
                        g_dumpProgress = name ? name : "";
                    });
                }
                catch (const std::exception &e)
                {
                    std::lock_guard guard(g_dumpMutex);
                    g_dumpProgress = std::string("dump failed: ") + e.what();
                }
                catch (...)
                {
                    std::lock_guard guard(g_dumpMutex);
                    g_dumpProgress = "dump failed (unknown)";
                }
            },
                                               outFile);
            if (!dumped && dump.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                {
                    std::lock_guard guard(g_dumpMutex);
                    g_dumpProgress = "Done";
                }
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
            // 不要用 getMethods(...)[1] 盲取下标：Camera.WorldToScreenPoint 有多个重载，
            // 取错签名后 invoke 会把隐藏的 MethodInfo* 喂到枚举参数位置 → 栈/寄存器错乱。
            auto M = cam ? cam->klass->getMethod("WorldToScreenPoint", 1) : nullptr;
            LOGPTR(M);
            if (M)
                LOGPTR(M->methodPointer);
            else
                LOGE("找不到 WorldToScreenPoint(Vector3)");
            return M;        }();

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
            // 每行 16 字节 = 48 个字符，加结尾 '\0'。用 size_t 做偏移算术，
            // 避免 int 乘法溢出/隐式加宽到 ptrdiff_t 的歧义。
            char buffer[16 * 3 + 1]{0};
            const auto *base = (const uint8_t *)ptr + (size_t)i * 16;
            for (size_t j = 0; j < 16; j++) {
                snprintf(buffer + j * 3, 4, "%02X ", base[j]);
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

        // 旧代码在两个分支里都 return 了，后面那句 disable 是死代码：
        // 短函数（开头就是 ret）一旦开过 near-branch trampoline 就再也关不掉，
        // 会一直影响后续 hook 的性能和正确性。用 RAII 保证两条路径都恢复。
        struct NearBranchGuard
        {
            bool active;
            ~NearBranchGuard()
            {
                if (active)
                    dobby_disable_near_branch_trampoline();
            }
        } nearBranchGuard{shortFunction};

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
