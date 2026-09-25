#include <pthread.h> // for pthread_create
#include <thread>
#include <unistd.h> // for sleep
#include "Il2cpp/Il2cpp.h" // for EnsureAttached, Init
#include "Il2cpp/il2cpp-class.h" // for Il2CppImage, Il2CppObject
#include "Includes/Logger.h" // for LOGD, LOGI
#include "Includes/Utils.h" // for isGameLibLoaded, isLibraryLoaded
#include "Includes/obfuscate.h" // for make_obfuscator, OBFUSCATE
#include "Menu/ImGui.h"
#include "Tool/Keyboard.h"
#include "Tool/Tool.h"
#include "Tool/Util.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "sstream"
#include "Tool/Unity.h"
#include "Tool/ObjectDrawManager.h"

void logcatJson(nlohmann::ordered_json &json)
{
    auto str = json.dump(4, '#');
    std::istringstream iss(str);
    std::string line;
    while (std::getline(iss, line))
    {
        usleep(100);
        LOGD("%s", line.c_str());
    }
}

template <typename T>
void ConfigSet(const char *key, T value);

// Target lib here
#define targetLibName OBFUSCATE("libil2cpp.so")

Il2CppImage *g_Image = nullptr;
std::vector<MethodInfo *> g_Methods;
extern std::unordered_map<void *, HookerData> hookerMap;
// hook 回调（游戏线程）与 UI（渲染线程）共享 hookerMap / HookerData::visited，
// 两侧都必须持这把锁：ClassesTab.cpp 里定义，hookerHandler 也是靠它串行化的。
extern std::mutex hookerMtx;
extern int maxLine;

// 初始化状态由 Menu/ImGui.cpp 定义（INIT_PENDING / INIT_READY / INIT_FAILED）。
// on_init 的各个失败点把它置成 INIT_FAILED；依赖没就绪时置 INIT_PENDING，
// 由 setupMenu 在后续帧重试。setupMenu 据此决定要不要继续建 UI。
extern int g_initState;

extern ImVec2 initialScreenSize;

// config
bool collapsed = false;
bool fullScreen = false;
bool resetWindow = false;
int selectedScale = 3;

bool g_fontFullRangeRequested = false;

bool doChangeScale = false;

// 用户改了字形范围但还没重启。图集在第一帧就烘焙完了，运行中改不动。
static bool g_fontRangeDirty = false;

constexpr std::array<const char *, 7> possibleScale = {
    "最小", "更小", "小", "默认", "大", "更大", "最大",
};
constexpr std::array<float, 7> scaleFactors = {0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f};

ImGuiStyle initialStyle;

const char *title = OBFUSCATE("Il2CppTool v0.9 | HitMargin");
void draw_thread()
{
    static ImVec2 lastSize = ImVec2(0, 0);
    static ImVec2 lastPos = ImVec2(0, 0);

    if (resetWindow)
    {
        resetWindow = false;
        if (fullScreen)
        {
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            auto screenSize = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowSize(screenSize);
        }
        else
        {
            ImGui::SetNextWindowPos(lastPos);
            ImGui::SetNextWindowSize(lastSize);
        }
    }
    if (fullScreen)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, ImGui::GetFrameHeight()));
    }
    int i = 0;
    auto drawList = ImGui::GetBackgroundDrawList();

    // hookerHandler 在游戏线程持 hookerMtx 往 visited 里 push_back；
    // 这里无锁遍历并且还在写元素（v.time -= dt / v.name = ""），
    // CircularBuffer 的迭代器/取元素都不是全程持锁的，并发下就是迭代野指针 +
    // std::string 被一边写一边比 → 崩。持 hookerMtx 与回调串行化。
    //
    // 注意：guard 必须用独立作用域包住循环本身。Tool::ToggleHooker 和
    // CalculateSomething 也会去锁同一把非递归 mutex，函数级 lock_guard 会自锁死机。
    {
        std::lock_guard visitedGuard(hookerMtx);
        for (auto &v : HookerData::visited)
        {
            if (v.name.empty())
                continue;
            char label[256]{0};
            snprintf(label, sizeof(label), "%s", v.name.c_str());
            if (v.hitCount > 0)
            {
                // 不能拿 label 自己当源又当目标 sprintf（重叠未定义行为），
                // 也不能 memcpy 定长 —— 内容够长时 label 不会被 NUL 结尾，
                // 后面 CalcTextSize(label) 就会读过界。用 snprintf 截断。
                char withCount[288]{0};
                snprintf(withCount, sizeof(withCount), "%s (%dx)", label, v.hitCount);
                snprintf(label, sizeof(label), "%s", withCount);
            }
            auto labelSize = ImGui::CalcTextSize(label);
            ImVec2 labellPos{20, 150 + (labelSize.y * i)};

            auto dt = ImGui::GetIO().DeltaTime;
            constexpr ImVec4 GREEN = {0.f, 1.f, 0.f, 1.f};
            ImColor color = ImColor(1.f, 1.f, 1.f, 1.f);
            if (v.time > 0.f)
            {
                v.time -= dt;
                auto t = v.time;
                color = ImColor(ImLerp(color.Value.x, GREEN.x, t), ImLerp(color.Value.y, GREEN.y, t),
                                ImLerp(color.Value.z, GREEN.z, t), 1.f);
            }
            v.goneTime -= dt;
            if (v.goneTime > 0.f && v.goneTime <= 1.f)
            {
                auto t = v.goneTime;
                color.Value.w = ImLerp(0.f, color.Value.w, t);
            }
            if (v.goneTime <= 0.f)
            {
                v.name = "";
            }

            drawList->AddRectFilled(labellPos, {labellPos.x + labelSize.x, labellPos.y + labelSize.y},
                                    IM_COL32(0, 0, 0, 100));
            drawList->AddText(labellPos, color, label);
            i++;
        }
    }
    collapsed = !ImGui::Begin(title, nullptr, (fullScreen ? ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove : 0));
    if (fullScreen)
    {
        ImGui::PopStyleVar();
    }

#ifdef __DEBUG__
    static bool showDemoWindow = false;
#endif
    Keyboard::Update();
    static bool changeToToolsTab = false;
    if (ImGui::BeginTabBar("mainTabber"))
    {
        if (ImGui::BeginTabItem("工具", nullptr, changeToToolsTab ? ImGuiTabItemFlags_SetSelected : 0))
        {
            changeToToolsTab = false;
            if (ImGui::Checkbox("全屏", &fullScreen))
            {
                if (fullScreen)
                {
                    lastSize = ImGui::GetWindowSize();
                    lastPos = ImGui::GetWindowPos();
                }
                resetWindow = true;
            }
            Tool::Draw();
            ImGui::EndTabItem();
        }

        // empty() 也是无锁读：追踪工作线程会持 hookerMtx 往里 insert/erase，
        // 并发下就是数据竞争（rehash 期间读 size 是 UB）。
        bool hasHooks = false;
        {
            std::lock_guard guard(hookerMtx);
            hasHooks = !hookerMap.empty();
        }
        if (hasHooks)
        {
            if (ImGui::BeginTabItem("追踪"))
            {
                // hook 回调跑在游戏线程并且会持 hookerMtx 改 hookerMap（hitCount++ / insert / erase）。
                // 旧代码在 UI 侧无锁遍历，还把 &data 存进 sortedHooker 拿到帧末才解引用：
                // 既是被并发写的数据竞争，也可能因为中途 erase 变成野指针。
                // 改成持锁拷一份最小快照（MethodInfo* + hitCount），之后只读快照。
                // MethodInfo* 指向游戏元数据，本身稳定，可以带出锁外使用。
                // 除累计次数外一并带上「次/秒」：按累计次数排序只能找出
                // 「启动时跑得最多的」，按频率排序才能找出「每帧都在跑的」——
                // 后者才是用户真正想找的热点。
                struct HookerSnapshot
                {
                    MethodInfo *method;
                    int hitCount;
                    float callsPerSecond;
                };
                std::vector<HookerSnapshot> sortedHooker;
                {
                    std::lock_guard guard(hookerMtx);
                    ImGui::Text("追踪方法数量 : %zu", hookerMap.size());
                    for (auto &[name, data] : hookerMap)
                    {
                        int hits = data.hitCount.load(std::memory_order_relaxed);
                        if (hits > 0 && data.method)
                        {
                            sortedHooker.push_back({data.method, hits, data.callsPerSecond});
                        }
                    }
                }
                if (!sortedHooker.empty())
                {
                    if (ImGui::Button("恢复"))
                    {
                        ImGui::OpenPopup("QuickRestorePopup");
                    }
                    // 先按频率降序，频率相同再按累计次数降序
                    std::sort(sortedHooker.begin(), sortedHooker.end(),
                              [](const auto &a, const auto &b)
                              {
                                  if (a.callsPerSecond != b.callsPerSecond)
                                      return a.callsPerSecond > b.callsPerSecond;
                                  return a.hitCount > b.hitCount;
                              });
                    ImGui::BeginChild("TracerList", ImVec2(0, 0), ImGuiChildFlags_None,
                                      ImGuiWindowFlags_HorizontalScrollbar);
                    auto &tab = Tool::GetFirstTab();
                    for (auto &[method, hitCount, cps] : sortedHooker)
                    {
                        // 类名 + 方法名长度不受控（混淆过的 il2cpp 名字可以很长），
                        // 固定 256 字节 + sprintf 就是栈溢出，改成 snprintf 截断。
                        char label[256]{0};
                        if (cps > 0.f)
                        {
                            snprintf(label, sizeof(label), "%s::%s  %.0f/s (%d)###%p",
                                     method->getClass()->getName(), method->getName(), cps, hitCount, method);
                        }
                        else
                        {
                            snprintf(label, sizeof(label), "%s::%s (%d)###%p",
                                     method->getClass()->getName(), method->getName(), hitCount, method);
                        }
                        // if (ImGui::Button(label, ImVec2(ImGui::GetContentRegionAvail().x, 0)))
                        // {
                        //     toBeErased = v;
                        // }

                        ImGui::PushID(method);
                        bool opened = tab.MethodViewer(method->getClass(), method, tab.getCachedParams(method));
                        if (!opened && !changeToToolsTab && ImGui::IsItemHeld())
                        {
                            LOGD("IsItemHeld %s", method->getName());
                            changeToToolsTab = true;
                            Tool::OpenNewTabFromClass(method->getClass()).setOpenedTab = true;
                            ImGui::PopID();
                            break;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndChild();
                    static auto &io = ImGui::GetIO();
                    ImGui::SetNextWindowSizeConstraints(ImVec2(io.DisplaySize.x / 1.2f, 0),
                                                        ImVec2(io.DisplaySize.x / 1.2f, io.DisplaySize.y / 2));
                    if (ImGui::BeginPopup("QuickRestorePopup",
                                          ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_MenuBar))
                    {
                        MethodInfo *toBeErased = nullptr;
                        for (auto &[method, hitCount, cps] : sortedHooker)
                        {
                            char label[256]{0};
                            snprintf(label, sizeof(label), "%s::%s (%dx)###%p", method->getClass()->getName(),
                                     method->getName(), hitCount, method);
                            if (ImGui::Button(label, ImVec2(ImGui::GetContentRegionAvail().x, 0)))
                            {
                                toBeErased = method;
                            }
                        }
                        if (toBeErased)
                        {
                            Tool::ToggleHooker(toBeErased, 0);
                        }
                        ImGui::EndPopup();
                    }
                }
                else
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));
                    ImGui::Text("尚未调用跟踪的方法");
                    ImGui::PopStyleColor();
                }
                ImGui::EndTabItem();
            }
        }
        if (ImGui::BeginTabItem("Dumper"))
        {
            Tool::Dumper();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("对象绘制"))
        {
            if (ObjectDrawManager::showObjectManager)
                ObjectDrawManager::DrawUI();
            else
                ImGui::Text("对象绘制管理器未启用（在「工具」页勾选开启）");
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("设置"))
        {
            ImGui::Separator();
            ImGui::Text("显示");
            // ImGui::SliderFloat("Scale##Global", &font->Scale, .1f, 2.0f, "%.2f");
            auto preview = possibleScale[selectedScale];
            if (ImGui::BeginCombo("Scale##Global", preview))
            {
                for (int i = 0; i < possibleScale.size(); i++)
                {
                    // bool selected = strcmp(possibleScale[i], preview) == 0;
                    bool selected = i == selectedScale;
                    if (ImGui::Selectable(possibleScale[i], selected))
                    {
                        selectedScale = i;
                        ConfigSet("selectedScale", selectedScale);
                        doChangeScale = true;
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::Checkbox("未接收到键盘输入时启用", &Keyboard::check);

            // 字体字形范围。默认只用常用字：完整中日韩范围 2.1 万个字形，
            // 光栅化在渲染线程上要好几秒，是启动卡顿的主因。
            // 切换后需要重启工具才生效 —— 图集一旦烘焙就固定了。
            ImGui::Separator();
            ImGui::Text("字体");
            if (ImGui::Checkbox("加载完整中日韩字形", &g_fontFullRangeRequested))
            {
                ConfigSet("fontFullRange", g_fontFullRangeRequested);
                g_fontRangeDirty = true;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("完整范围约 21000 个字形，首次启动会明显变慢。\n"
                                  "默认的常用字范围约 3000 个，覆盖绝大多数界面文本。\n"
                                  "改动需重启工具生效。");
            }
            if (g_fontRangeDirty)
            {
                ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "重启工具后生效");
            }
            ImGui::TextDisabled("当前: %s", g_fontFullRangeRequested ? "完整中日韩 (21000 字形)" : "常用字 (~3000 字形)");

#ifdef __DEBUG__
            if (ImGui::Checkbox("Show Demo Window", &showDemoWindow))
            {
            }
#endif
            ImGui::Separator();
            ImGui::Text("Info");
            static auto packageName = Il2cpp::getPackageName();
            static auto unityVersion = Il2cpp::getUnityVersion();
            static auto gameVersion = Il2cpp::getGameVersion();
            ImGui::Text("包名: %s", packageName.c_str());
            ImGui::Text("版本: %s", gameVersion.c_str());
            ImGui::Text("Unity版本: %s", unityVersion.c_str());
#ifdef __aarch64__
            ImGui::Text("架构: %s", "arm64-v8a");
#else
            ImGui::Text("架构: %s", "armeabi-v7a");
#endif
            ImGui::Separator();

            // FIXME: DRY
            /*
            if (ImGui::Button("", ImVec2(-1, 0)))
            {
                auto Application = Il2cpp::FindClass("UnityEngine.Application");
                auto OpenURL = Application->getMethod("OpenURL", 1);
                if (OpenURL)
                {
                    OpenURL->invoke_static<void>(Il2cpp::NewString(OBFUSCATE("")));
                }
                else
                {
                    Keyboard::Open(OBFUSCATE(""), nullptr);
                }
            }
            */
            if (ImGui::Button("bilibili HitMargin", ImVec2(-1, 0)))
            {
                auto Application = Il2cpp::FindClass("UnityEngine.Application");
                auto OpenURL = Application->getMethod("OpenURL", 1);
                if (OpenURL)
                {
                    OpenURL->invoke_static<void>(Il2cpp::NewString(OBFUSCATE("https://m.bilibili.com/space/1757946676")));
                }
                else
                {
                    Keyboard::Open(OBFUSCATE(""), nullptr);
                }
            }
            /*
            if (ImGui::Button("Platinmods thread", ImVec2(-1, 0)))
            {
                auto Application = Il2cpp::FindClass("UnityEngine.Application");
                auto OpenURL = Application->getMethod("OpenURL", 1);
                if (OpenURL)
                {
                    OpenURL->invoke_static<void>(
                        Il2cpp::NewString(OBFUSCATE("")));
                }
                else
                {
                    Keyboard::Open(OBFUSCATE(""), nullptr);
                }
            }
            */
            ImGui::Separator();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    static bool doCalculate = false;
    if (doCalculate)
    {
        doCalculate = false;
        Tool::CalculateSomething();
    }

    if (doChangeScale)
    {
        doChangeScale = false;
        static auto font = ImGui::GetFont();

        font->Scale = scaleFactors[selectedScale];
        auto style = initialStyle;
        style.ScaleAllSizes(font->Scale);
        ImGui::GetStyle() = style;
        doCalculate = true;
        if (fullScreen)
            resetWindow = true;
    }

#ifdef __DEBUG__
    if (showDemoWindow)
    {
        ImGui::ShowDemoWindow();
    }
#endif

    // 对象绘制管理器：ESP 刷新与绘制独立于配置 UI 是否打开/折叠。
    // Tick 负责按节流刷新（内部含 il2cpp 调用），DrawAll 只画已算好的坐标；
    // 即使配置页没打开，ESP 也会持续更新，不会再卡住不动。
    if (ObjectDrawManager::showObjectManager)
    {
        try
        {
            ObjectDrawManager::Tick();
        }
        catch (const std::exception &e)
        {
            LOGW("ObjectDraw Tick exception: %s", e.what());
        }
        catch (...)
        {
            LOGW("ObjectDraw Tick unknown exception");
        }
        try
        {
            ObjectDrawManager::DrawAll();
        }
        catch (...)
        {
            LOGW("DrawAll exception");
        }
    }

    ImGui::End();
}

static nlohmann::ordered_json gConf;

template <typename T>
void ConfigSet(const char *key, T value)
{
    LOGD(__FUNCTION__);
    gConf[key] = value;
    LOGD("ConfigWrite %s = %s", key, gConf[key].dump().c_str());
    Util::FileWriter fileWriter("tool_conf.json");
    fileWriter.write(gConf.dump(2).c_str());
}

template <typename T>
T ConfigGet(const char *key, T defaultValue)
{
    LOGD(__FUNCTION__);
    if (gConf.contains(key))
    {
        LOGD("ConfigGet %s = %s", key, gConf[key].dump().c_str());
        // 配置文件是人可编辑的：JSON 合法但类型不对（比如 selectedScale 写成字符串）
        // 时 .get<T>() 会抛 nlohmann::type_error。这个函数在 on_init → setupMenu →
        // eglSwapBuffers 链上跑，异常逃出去就等于游戏崩，必须就地兜住。
        try
        {
            return gConf[key].get<T>();
        }
        catch (const nlohmann::json::exception &e)
        {
            LOGE("配置项 %s 类型错误(%s)，改用默认值并修正配置", key, e.what());
            ConfigSet(key, defaultValue);
            return defaultValue;
        }
    }
    else
    {
        ConfigSet(key, defaultValue);
    }
    return defaultValue;
}

// 字体字形范围。默认只加载常用字（约 3000 个），完整中日韩范围有 2.1 万个，
// 光栅化要在渲染线程上花好几秒。
//
// 必须在 setupMenu 烘焙字体图集**之前**读到 —— 那发生在第一帧
// eglSwapBuffers，而 on_init 跑在它之后。所以由 hack 线程上的
// initModMenu 调 ReadFontConfigEarly() 提前读一次。
//
// 改动需要重启工具才生效：图集一旦烘焙就固定了。
void ReadFontConfigEarly()
{
    Util::FileReader fileReader("tool_conf.json");
    if (!fileReader.exists())
    {
        return;
    }
    auto data = fileReader.read();
    if (data.empty())
    {
        return;
    }
    try
    {
        auto conf = nlohmann::json::parse(data);
        if (conf.contains("fontFullRange") && conf["fontFullRange"].is_boolean())
        {
            g_fontFullRangeRequested = conf["fontFullRange"].get<bool>();
            LOGD("字体范围: %s", g_fontFullRangeRequested ? "完整中日韩（启动会明显变慢）" : "常用字");
        }
    }
    catch (const nlohmann::json::exception &e)
    {
        // 解析失败不是致命的：保持默认的常用字范围即可。
        // 真正的问题会由 on_init 里的 ConfigInit 再报一次。
        LOGE("提前读取字体配置失败: %s", e.what());
    }
}

void ConfigInit()
{
    Util::FileReader fileReader("tool_conf.json");
    if (fileReader.exists())
    {
        auto data = fileReader.read();
        try
        {
            gConf = nlohmann::json::parse(data);
            logcatJson(gConf);
        }
        catch (nlohmann::json::exception &e)
        {
            LOGE("ConfigInit error : %s", e.what());
            Util::FileWriter fileWriter("tool_conf.json");
            fileWriter.write("{}");
        }
    }
    else
    {
        Util::FileWriter fileWriter("tool_conf.json");
        fileWriter.write("{}");
    }
}

void on_init()
{
    LOGD(__FUNCTION__);

    // 这段跑在渲染线程（on_init 是被 eglSwapBuffers 钩子同步调进来的），
    // 所以这里的每一毫秒都是用户看到的画面卡顿。分段计时是为了在真机上
    // 能直接看出钱花在哪 —— 之前完全没有数据，只能靠猜。
    const auto initStart = std::chrono::steady_clock::now();
    auto markPhase = [&](const char *name) {
        static auto prev = initStart;
        const auto now = std::chrono::steady_clock::now();
        LOGI("[启动] %-22s %4lld ms (累计 %4lld ms)", name,
             (long long)std::chrono::duration_cast<std::chrono::milliseconds>(now - prev).count(),
             (long long)std::chrono::duration_cast<std::chrono::milliseconds>(now - initStart).count());
        prev = now;
    };

    // 这段跑在渲染线程（on_init 是被 eglSwapBuffers 钩子同步调进来的）。
    // 原先这里 while + sleep(1) 死等：目标不是 il2cpp 游戏时首帧能卡 60 秒（ANR）。
    // 现在只探测一次，没就绪就返回 INIT_PENDING，由 setupMenu 在后续帧重试，
    // 期间游戏照常渲染，菜单暂时不显示。
    if (!isLibraryLoaded(targetLibName))
    {
        g_initState = INIT_PENDING;
        return;
    }

    LOGI("%s has been loaded", (const char *)targetLibName);

    // il2cpp 没就绪就到此为止：后面的 GetAssembly / GetImages / Unity::HookInput
    // 全都要在解析好的 API 表上跑，强行继续只会一路空指针崩掉。
    //
    // 这里区分「还没就绪」和「彻底失败」：
    // - 还没就绪（渲染线程暂时不是 VM 线程）→ INIT_PENDING，下一帧重试。
    //   这条路径绝不能 sleep 等待，否则会把渲染线程冻住。
    // - 彻底失败（API 表解析不出来）→ INIT_FAILED，不再重试。
    if (!Il2cpp::Init())
    {
        if (Il2cpp::ApiResolved())
        {
            g_initState = INIT_PENDING;
        }
        else
        {
            LOGE("il2cpp API 表解析失败，放弃初始化");
            g_initState = INIT_FAILED;
        }
        return;
    }
    // attach 失败同样可能只是时机问题：下一帧重试，不要一上来就放弃。
    if (!Il2cpp::EnsureAttached())
    {
        LOGI("暂未 attach 到 il2cpp VM，稍后重试");
        g_initState = INIT_PENDING;
        return;
    }

    Keyboard::Init();
    markPhase("Keyboard::Init");

    initialStyle = ImGui::GetStyle();
    ConfigInit();
    selectedScale = ConfigGet<int>("selectedScale", selectedScale);
    if (selectedScale < 0 || selectedScale >= scaleFactors.size())
    {
        selectedScale = 3;
        ConfigSet("selectedScale", selectedScale);
    }
    doChangeScale = true;
    markPhase("配置");

    LOGD("HOOKING...");

    auto images = Il2cpp::GetImages();
    // GetImage 失败时返回 nullptr，列表里会混进空元素。
    // 旧代码直接 images.front() / image->getClasses() 就是空指针解引用。
    images.erase(std::remove_if(images.begin(), images.end(), [](Il2CppImage *img) { return img == nullptr; }),
                 images.end());

    // "Assembly-CSharp" 并非所有 Unity 工程都有：游戏代码可能在别的 assembly，
    // 或者整个工程没建出这个默认名。GetAssembly 找不到时返回 null，
    // 旧代码直接 ->getImage() 就是空指针解引用，工具在启动阶段直接崩。
    g_Image = nullptr;
    if (auto *asmCSharp = Il2cpp::GetAssembly("Assembly-CSharp"))
    {
        g_Image = asmCSharp->getImage();
    }
    if (!g_Image && !images.empty())
    {
        g_Image = images.front();
        LOGW("未找到 Assembly-CSharp，回退到第一个 assembly: %s", g_Image->getName());
    }
    if (!g_Image)
    {
        // 一个 image 都没有时不能往下走：Tool::Init 会开第一个 ClassesTab，
        // 而它会直接 selectedImage->getClasses()，g_Image 为空就是必崩。
        LOGE("没有可用的 assembly，il2cpp 元数据可能尚未就绪，跳过类/方法枚举");
        g_initState = INIT_FAILED;
        return;
    }
    Tool::Init(g_Image, images);
    markPhase("Tool::Init (类枚举)");

    // 输入 hook 放在元数据校验之后：初始化一旦失败就返回，
    // 此时若已装上输入 hook，还得专门摘掉，否则它会摸到被销毁的 ImGui context。
#ifndef LIB_INPUT
    Unity::HookInput();
#endif
    markPhase("输入 hook");

    // 收集全部方法的地址表。
    //
    // 这张表只有一个消费者：Frida.cpp 的地址反查（gummp 的 return-address
    // 回溯）。而 Frida 那条路当前是**关着的** —— Android.mk 里既没有
    // -DUSE_FRIDA，Tool::Init 里的 Frida::Init() 也在 #ifdef USE_FRIDA 之内。
    // 也就是说这张表现在建完之后**永远没有人读**。
    //
    // 代价却不小：遍历所有 assembly × 所有类 × 所有方法，再对结果排序。
    // 一个正常规模的 Unity 游戏是十万量级的 MethodInfo，每轮还要为每个类
    // 分配一次 getMethods() 的 vector。这些全跑在渲染线程上
    // （on_init ← eglSwapBuffers），是启动卡顿的另一个主因。
    //
    // 所以按 USE_FRIDA 门禁：真正启用 Frida 时照旧构建，不启用时完全跳过。
    // 想启用就在 Android.mk 的 LOCAL_CFLAGS 里加 -DUSE_FRIDA。
#ifdef USE_FRIDA
    {
        const auto methodsStart = std::chrono::steady_clock::now();
        g_Methods.reserve(64 * 1024); // 预留，避免十几次 vector 翻倍
        for (auto image : images)
        {
            for (auto klass : image->getClasses())
            {
                for (auto m : klass->getMethods())
                {
                    if (!m->methodPointer)
                        continue;
                    g_Methods.emplace_back(m);
                }
            }
        }
        LOGD("%zu methods", g_Methods.size());
        LOGD("SORTING");
        std::sort(g_Methods.begin(), g_Methods.end(),
                  [](const auto &a, const auto &b) { return a->methodPointer < b->methodPointer; });
        // 元数据被裁剪过的游戏可能一个带 methodPointer 的方法都没有，
        // 此时 front()/back() 是未定义行为。
        if (!g_Methods.empty())
        {
            LOGPTR(g_Methods.front()->methodPointer);
            LOGPTR(g_Methods.back()->methodPointer);
        }
        else
        {
            LOGW("未收集到任何带 methodPointer 的方法，地址反查/回溯将不可用");
        }
        LOGD("SORTED");
        const auto methodsEnd = std::chrono::steady_clock::now();
        LOGI("方法表构建完成: %zu 条，耗时 %lld ms", g_Methods.size(),
             (long long)std::chrono::duration_cast<std::chrono::milliseconds>(methodsEnd - methodsStart).count());
    }
#else
    // 不启用 Frida：没人会读这张表，直接不构建。
    // 日志里明确记一笔，否则以后有人看到 g_Methods 为空会以为功能坏了。
    LOGD("未启用 USE_FRIDA，跳过方法表构建（仅 Frida 回溯功能需要）");
#endif
    LOGD("HOOKED!");

    // 全部成功才算就绪。setupMenu 见到 INIT_READY 才会把菜单标记为可用并开始渲染。
    // （早前各个失败/未就绪分支已经把状态置成 INIT_FAILED / INIT_PENDING 并 return 了。）
    g_initState = INIT_READY;
    markPhase("总计");
}

// we will run our hacks in a new thread so our while loop doesn't block process main thread
void *hack_thread(void *)
{
    logger::Clear();

    LOGI("pthread created");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    initModMenu((void *)draw_thread, (void *)on_init);
    return nullptr;
}

// 在导出进行中时进程退出（用户强制停止游戏）也要安全收尾。
// g_dump.worker 是全局 std::thread：析构时如果仍 joinable 就是
// std::terminate —— 而这是我们**注入进别人游戏**的库，触发它等于
// 让用户的游戏莫名其妙崩掉。
//
// atexit 在正常退出和 return/exit 时都会跑；配合进程被 SIGKILL 之外的
// 终止路径，覆盖不到的情况就只能靠 OS 回收进程资源了。
__attribute__((destructor)) void lib_cleanup()
{
    Tool::ShutdownDumper();
    // 同样要停：筛选工作线程是全局 std::thread，析构时仍 joinable 就是
    // std::terminate。而且它还在跑 il2cpp 调用 —— 不 join 就让进程退出，
    // 等于让一个正在遍历托管元数据的线程被强行掐掉。
    ClassesTabWorker::Shutdown();
    // 释放软键盘的 GC 句柄。不释放就是一次句柄泄漏，而我们是注入进
    // 别人进程的库 —— 漏的是游戏进程的 GC 句柄表。
    Keyboard::Reset();
}

__attribute__((constructor)) void lib_main()
{
    // Create a new thread so it does not block the main thread, means the game would not freeze
    pthread_t ptid;
    pthread_create(&ptid, nullptr, hack_thread, nullptr);
}
