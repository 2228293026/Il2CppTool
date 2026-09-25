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

    // 采样各 hook 的调用频率，供 UI 画曲线。
    //
    // 每帧在**渲染线程**调用，绝不能放进 hook 回调里 —— 那是游戏的执行路径，
    // 采样意味着遍历整张 hookerMap，那会直接变成每帧一次的游戏卡顿来源。
    //
    // hitCount 用 relaxed 原子就是为这里准备的：采样读它不需要拿互斥锁，
    // 读到的值可能略滞后一两次更新，对「次/秒」这种统计量毫无影响。
    void SampleHookRates()
    {
        // 采样间隔太短曲线会很抖，太长则丢掉尖峰。250ms 是个平衡点。
        constexpr double kSampleInterval = 0.25;
        // 保留的历史点数。120 * 0.25s = 30 秒的窗口。
        constexpr size_t kHistorySize = 120;

        const auto now = std::chrono::steady_clock::now();
        const double nowSec = std::chrono::duration<double>(now.time_since_epoch()).count();

        std::lock_guard guard(hookerMtx);
        // 按 key 排序后再处理：hookerMap 的 key 是函数地址（指针），
        // unordered_map 的遍历顺序对同一份内容只是「碰巧稳定」，换个插入
        // 顺序就变。UI 列表依赖这个顺序做稳定排序，顺序不稳会让同频率的
        // 方法在列表里上下跳动。
        std::vector<void *> addresses;
        addresses.reserve(hookerMap.size());
        // NOLINTNEXTLINE 盖不住这条：诊断落在 for-range 语句的 range 上，
        // 跨行，所以改成同行标注。
        // 见上面注释：这次遍历只收集 key，真正决定顺序的是紧接着的 sort。
        for (auto &[addr, data] : hookerMap) // NOLINT(bugprone-nondeterministic-pointer-iteration-order)
        {
            addresses.push_back(addr);
        }
        std::sort(addresses.begin(), addresses.end());

        for (void *addr : addresses)
        {
            auto it = hookerMap.find(addr);
            // 理论上不会发生（刚持锁取的 key），但 find 失败时必须跳过，
            // 不能假设 find 一定成功。
            if (it == hookerMap.end())
            {
                continue;
            }
            auto &data = it->second;
            if (data.lastSampleTime == 0.0)
            {
                // 第一次见到：只记录基线，不产生一个假的尖峰
                data.lastSampleTime = nowSec;
                data.sampledHitCount = data.hitCount.load(std::memory_order_relaxed);
                continue;
            }
            const double dt = nowSec - data.lastSampleTime;
            if (dt < kSampleInterval)
            {
                continue;
            }
            const int current = data.hitCount.load(std::memory_order_relaxed);
            const int delta = current - data.sampledHitCount;
            data.sampledHitCount = current;
            data.lastSampleTime = nowSec;
            // 负数只可能来自计数器回绕/重置，夹到 0
            data.callsPerSecond = delta > 0 ? static_cast<float>(delta / dt) : 0.f;

            if (data.rateHistory.size() >= kHistorySize)
            {
                data.rateHistory.erase(data.rateHistory.begin());
            }
            data.rateHistory.push_back(data.callsPerSecond);
        }
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
        // 筛选工作线程在 tab 构造前就要就绪（构造里会投递第一个筛选请求）。
        ClassesTabWorker::EnsureStarted();
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
        // 采样放在 UI 之前：这一帧画出来的曲线要用这一帧刚采到的数据。
        SampleHookRates();

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
        // Dumper 的共享状态。
        //
        // 旧实现把进度放在渲染线程读、dump 线程写的 std::string 上 ——
        // 并发读写 std::string 会破坏堆（realloc 时可能一边释放一边被
        // 另一侧解引用）。
        //
        // 另一个旧问题：整个 Dumper 靠一堆函数内 static 来维持状态
        // （static bool dumping / static future dump / static char outFile），
        // 结果是**只能 dump 一次** —— 第一次跑完后 future 已经被消费，
        // 再点 DUMP 什么也不会发生，但按钮看上去还是可点的。
        // 现在换成显式的状态机。
        enum class DumpState
        {
            Idle,
            Running,
            Done,
            Failed,
            Cancelled,
        };

        struct DumpStatus
        {
            std::mutex mutex;
            DumpState state = DumpState::Idle;
            std::string currentAssembly;
            int current = 0;
            int total = 0;
            std::string message;
            std::string outputPath;
            // 渲染线程点「取消」时置位；dump 线程在每个类的进度回调里检查
            std::atomic<bool> cancelRequested{false};
            std::thread worker;

            // **故意不定义析构函数。**
            //
            // std::thread 的析构函数在仍 joinable 时直接 std::terminate。
            // 而它原本是命名空间作用域的 static：会被注册进 atexit 列表，
            // 在 exit() 时由 __cxa_finalize 析构。
            //
            // 正常路径没问题 —— lib_cleanup（.fini_array）会 join 掉 worker，
            // bionic 是逆序执行 fini_array 的，所以 __cxa_finalize 最后跑。
            // 但 **exit() 路径**不一样：__cxa_finalize 只跑 atexit 列表上的东西，
            // 而 lib_cleanup 是 .fini_array 函数、**不在**那个列表里。
            // 于是「dump 跑到一半进程 exit()」= SIGABRT。
            //
            // 改成不析构：worker 的回收交给 lib_cleanup 显式做。
            // 万一走到 exit() 且没 join，也只是泄漏一个线程句柄，
            // 不会 abort 掉用户的游戏。
            ~DumpStatus() = default;
        };

        // 这个 Dump() 同样**故意泄漏**：见上面 DumpStatus 的注释。
        // 进程生命周期内一直有效，退出时由内核回收。
        DumpStatus &Dump()
        {
            static DumpStatus *d = new DumpStatus();
            return *d;
        }

    } // namespace

    // 启动一次 dump。返回 false 表示已经有一次在跑。
    static bool StartDump(const std::string &outPath)
    {
        // 上一次已经结束的话，先把旧线程 join 掉再开新的。
        // std::thread 析构时若仍 joinable 会直接 terminate —— 重复 dump
        // 时这条路径必经。
        if (Dump().worker.joinable())
        {
            Dump().worker.join();
        }

        {
            std::lock_guard guard(Dump().mutex);
            Dump().state = DumpState::Running;
            Dump().currentAssembly.clear();
            Dump().current = 0;
            Dump().total = 0;
            Dump().message = "正在统计类数量…";
            Dump().outputPath = outPath;
        }
        Dump().cancelRequested.store(false, std::memory_order_relaxed);

        // std::thread 的构造函数**会抛** std::system_error（线程创建失败，
        // 例如达到线程数上限、内存吃紧）。而 StartDump 是从 Tool::Dumper()
        // → draw_thread 调进来的，那是**游戏的渲染线程**。
        // 异常一路逃出去 = std::terminate = abort，用户看到的现象是
        // 「点了一下 Dump，游戏直接没了」。
        //
        // 这里就地兜住：失败就报状态，UI 上显示「无法启动导出线程」。
        try
        {
            Dump().worker = std::thread(
                [](const std::string &path)
            {
                auto setState = [](DumpState s, const std::string &msg)
                {
                    std::lock_guard guard(Dump().mutex);
                    Dump().state = s;
                    Dump().message = msg;
                };

                // dump 线程是 il2cpp 的 foreign thread：il2cpp_dump 会遍历
                // domain / assembly / class，不 attach 到 VM 就是崩溃或错数据。
                // 用完必须 detach，否则 attached-thread 表里留悬空条目。
                if (!Il2cpp::EnsureAttached())
                {
                    setState(DumpState::Failed, "无法 attach 到 il2cpp VM");
                    return;
                }
                struct DetachGuard
                {
                    ~DetachGuard() { Il2cpp::Detach(); }
                } detachGuard;

                try
                {
                    bool wasCancelled = false;
                    bool ok = il2cpp_dump(
                        path.c_str(),
                        [](const char *name, int current, int total) -> bool
                        {
                            // 返回 false = 请求中止。
                            if (Dump().cancelRequested.load(std::memory_order_relaxed))
                            {
                                return false;
                            }
                            std::lock_guard guard(Dump().mutex);
                            Dump().currentAssembly = name ? name : "";
                            Dump().current = current;
                            Dump().total = total;
                            // 每 64 个类刷一次状态即可：进度回调是每个类
                            // 都跑的，在里面抢锁没必要那么频繁。
                            if (current % 64 == 0 || current == total)
                            {
                                Dump().message = "正在导出…";
                            }
                            return true;
                        },
                        &wasCancelled);

                    if (ok)
                    {
                        setState(DumpState::Done, "完成");
                    }
                    else if (wasCancelled)
                    {
                        setState(DumpState::Cancelled, "已取消（临时文件 .part 已丢弃，之前的导出未受影响）");
                    }
                    else
                    {
                        // 旧实现这里和「用户取消」共用一句话，于是磁盘满、
                        // 权限不足、路径打不开全都告诉用户「你按了取消」。
                        // 用户会一直重试，而不去腾空间/换路径。
                        setState(DumpState::Failed, "导出失败：无法写入文件。请检查存储空间与目录权限。");
                    }
                }
                catch (const std::exception &e)
                {
                    setState(DumpState::Failed, std::string("导出失败: ") + e.what());
                }
                catch (...)
                {
                    setState(DumpState::Failed, "导出失败（未知异常）");
                }
            },
            outPath);
        }
        catch (const std::system_error &e)
        {
            std::lock_guard guard(Dump().mutex);
            Dump().state = DumpState::Failed;
            Dump().message = std::string("无法启动导出线程: ") + e.what();
            LOGE("StartDump: 创建 dump 线程失败: %s", e.what());
            return false;
        }
        catch (const std::exception &e)
        {
            std::lock_guard guard(Dump().mutex);
            Dump().state = DumpState::Failed;
            Dump().message = std::string("启动导出失败: ") + e.what();
            LOGE("StartDump: %s", e.what());
            return false;
        }

        return true;
    }

    void ShutdownDumper()
    {
        // dump 线程必须 join：std::thread 析构时如果仍 joinable 会直接
        // std::terminate —— 重复 dump 时这条路径必经。
        // 如果用户在导出中途关掉工具，这就是一条必崩的路径。
        Dump().cancelRequested.store(true, std::memory_order_relaxed);
        if (Dump().worker.joinable())
        {
            LOGI("等待 dump 线程退出…");
            Dump().worker.join();
        }
    }

    void Dumper()
    {
        DumpState state;
        std::string currentAssembly;
        std::string message;
        std::string outputPath;
        int current = 0;
        int total = 0;
        {
            std::lock_guard guard(Dump().mutex);
            state = Dump().state;
            currentAssembly = Dump().currentAssembly;
            message = Dump().message;
            outputPath = Dump().outputPath;
            current = Dump().current;
            total = Dump().total;
        }

        switch (state)
        {
        case DumpState::Idle:
            if (ImGui::Button("导出 .cs (DUMP)"))
            {
                // 路径长度不受控（包名 + 数据目录 + 版本号），用 std::string
                // 让它自然增长 —— 固定缓冲 + 无界 sprintf 就是栈溢出。
                std::string outPath = Il2cpp::getDataPath() + "/" + Il2cpp::getPackageName() + "_" +
                                      Il2cpp::getGameVersion() + ".cs";
                if (outPath.find("unknown_") != std::string::npos)
                {
                    // 拿不到包名/版本时会退化成 unknown_*，那会写出一个
                    // 用户根本认不出来的文件名。直接说明，不产出垃圾文件。
                    LOGE("无法确定输出文件名（包名/版本读取失败）");
                    std::lock_guard guard(Dump().mutex);
                    Dump().state = DumpState::Failed;
                    Dump().message = "无法确定输出文件名：读不到包名或版本号";
                }
                else
                {
                    StartDump(outPath);
                }
            }
            break;

        case DumpState::Running:
        {
            if (total > 0)
            {
                ImGui::ProgressBar((float)current / (float)total, ImVec2(-1, 0));
                ImGui::Text("%d / %d 类 (%.1f%%)", current, total, 100.0f * current / (float)total);
            }
            else
            {
                // 统计类数量本身也要走一遍元数据，这段期间 total 还是 0
                ImGui::ProgressBar(0.f, ImVec2(-1, 0));
            }
            if (!currentAssembly.empty())
            {
                ImGui::TextDisabled("%s", currentAssembly.c_str());
            }
            if (!message.empty())
            {
                ImGui::TextDisabled("%s", message.c_str());
            }
            if (ImGui::Button("取消"))
            {
                Dump().cancelRequested.store(true, std::memory_order_relaxed);
            }
            break;
        }

        case DumpState::Done:
            ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "导出完成");
            if (ImGui::Button("复制路径"))
            {
                Keyboard::Open(outputPath.c_str(), nullptr);
            }
            ImGui::SameLine();
            if (ImGui::Button("再导出一次"))
            {
                std::lock_guard guard(Dump().mutex);
                Dump().state = DumpState::Idle;
            }
            break;

        case DumpState::Cancelled:
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.3f, 1.f), "已取消：%s", message.c_str());
            ImGui::TextDisabled("文件里是**不完整**的内容，不适合直接使用");
            if (ImGui::Button("再试一次"))
            {
                std::lock_guard guard(Dump().mutex);
                Dump().state = DumpState::Idle;
            }
            break;

        case DumpState::Failed:
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "导出失败：%s", message.c_str());
            if (ImGui::Button("重试"))
            {
                std::lock_guard guard(Dump().mutex);
                Dump().state = DumpState::Idle;
            }
            break;
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
            auto &data = hookerMap[method->methodPointer];
            // 装钩子前先归零：否则复用同一地址的旧条目会带着上一轮的计数和
            // 曲线过来，UI 上看起来像「刚装上就已经被调了几千次」。
            data.hitCount.store(0, std::memory_order_relaxed);
            data.sampledHitCount = 0;
            data.callsPerSecond = 0.f;
            data.rateHistory.clear();
            data.lastSampleTime = 0.0;
            data.method = method;
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
