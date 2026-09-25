#include "Unity.h"
#include "Il2cpp/Il2cpp.h"
#include "Includes/Macros.h"
#include "Includes/obfuscate.h"
#include "dobby.h"
#include "imgui/imgui.h"

// this function hook will prevent touch pass through the ImGui window
//
// 注意 il2cpp 的 ABI：静态方法真实的签名末尾带一个隐藏的 MethodInfo*，
// 本项目调用约定也是 T(*)(Args..., MethodInfo*)（见 il2cpp-class.h 的
// MethodInfo::invoke_static）。旧代码把 orig/替换函数都声明成不带这个参数的
// 形式，游戏调用时 x1 里带着的 MethodInfo* 就没有对应的形参接收 ——
// 参数错位，既可能读垃圾元数据也可能踩坏寄存器。
int (*o_get_touchCount)(MethodInfo *);
int get_touchCount(MethodInfo *method);
bool (*oInput_GetMouseButton)(int n, MethodInfo *method);
bool Input_GetMouseButton(int n, MethodInfo *method);

// hook 里要转发给原函数，需要用到当初解析出来的 MethodInfo。
static MethodInfo *s_get_touchCount = nullptr;
static MethodInfo *s_GetMouseButton = nullptr;
// ImGui 侧左键是否处于「已按下」状态。用于补发抬起事件：
// 触摸被系统取消、或手指数归零而 Unity 没给 Ended 时，靠它兜底。
//
// 必须是 atomic：下面两个 hook 跑在**游戏的输入线程**上，而它们读写的
// ImGuiIO 由**渲染线程**的 NewFrame 更新。普通 bool 会被编译器自由提升
// 读寄存器，跨线程毫无保证 —— 两个调用线程还会互相把「按住」状态改花，
// 表现就是界面卡在按下态。
static std::atomic<bool> s_pressed{false};

// 本次手势是否归菜单消费。
//
// 在 Began 那一刻定下来，整段手势不再变 —— 这是为了让游戏要么完整看到
// 一次触摸、要么完全看不到，不会看到「开始了却凭空消失」（那会被当成取消）。
static std::atomic<bool> s_touchCaptured{false};

// 触摸偏移自检结果：0 = 未自检，1 = 通过，-1 = 失败。
static std::atomic<int> s_touchOffsetOk{0};

int Unity::TouchOffsetCheck()
{
    return s_touchOffsetOk.load();
}

static Il2CppClass *Input;

// ImGui context 是否还活着。输入 hook 装在游戏的输入路径上，
// 初始化失败销毁 context 后游戏仍会调进来，这时必须直通，不能摸 ImGui。
// 由 Menu/ImGui.cpp 在创建/销毁 context 时维护。
namespace Unity
{
bool g_uiContextAlive = false;

// 保护 ImGuiIO 的跨线程访问。
//
// 关键冲突：输入 hook（游戏输入线程）调 io.AddMousePosEvent / AddMouseButtonEvent，
// 它们最终 push_back 到 g.InputEventsQueue —— 一个**会扩容、会 realloc、
// 释放旧缓冲**的 ImVector。而渲染线程的 ImGui::NewFrame() 正在遍历同一个
// vector、然后 resize(0) 清空它。
//
// 后果不是「读到旧值」这么轻：realloc 会 free 旧缓冲，另一线程还在遍历它
// 就是 use-after-free。io.DisplaySize 同样是无同步读写。
//
// 常见 Unity+GLES3 配置下玩家循环、get_touchCount、eglSwapBuffers 都在
// UnityMain 上，可能碰巧不出事；但有独立渲染线程 / 多线程图形作业的游戏上
// 就会炸。这里加锁 —— 临界区只是几个 push_back，代价可忽略，
// 而 IM_ASSERT 映射的是 __builtin_trap（无声 SIGILL），赌不起。
//
// 用永不析构的单例：本库是注入进别人进程的，锁不能进 .fini_array。
NeverDestroyedMutex &InputMutex()
{
    static NeverDestroyedMutex m;
    return m;
}

// 菜单窗口的屏幕矩形。渲染线程写、输入线程读，共用 InputMutex。
static float g_menuRect[4] = {0.f, 0.f, 0.f, 0.f};
static bool g_menuRectValid = false;

// 注意：Publish/Query 都**不自己加锁**，调用方必须已持有 InputMutex。
// 渲染线程那边是先取锁再调用，输入 hook 那边整个函数体都在锁内 ——
// 两边都是「已持锁 → 调用」，这样不会自锁。
void PublishMenuRect(float x0, float y0, float x1, float y1)
{
    g_menuRect[0] = x0;
    g_menuRect[1] = y0;
    g_menuRect[2] = x1;
    g_menuRect[3] = y1;
    g_menuRectValid = true;
}

bool QueryMenuRect(float &x0, float &y0, float &x1, float &y1)
{
    if (!g_menuRectValid)
    {
        return false;
    }
    x0 = g_menuRect[0];
    y0 = g_menuRect[1];
    x1 = g_menuRect[2];
    y1 = g_menuRect[3];
    return true;
}
} // namespace Unity

extern bool collapsed;
extern bool fullScreen;

// 这次触摸归不归菜单？必须在**调用方已持有 InputMutex** 时使用。
//
// 优先用菜单窗口的实际矩形（渲染线程每帧发布），因为它对「当前触摸坐标」
// 是精确的；而 io.WantCaptureMouse 是由**上一帧**的鼠标位置算出来的 ——
// 第一次按下时手指还没落下，必然滞后一帧，于是点菜单会漏到游戏里。
static bool ShouldCaptureAt(float x, float y, const ImGuiIO &io)
{
    // 折叠 + 全屏时左上角留了一块「唤出菜单」的角标区域，
    // 点那里不应该算作「点菜单」，否则用户根本点不出菜单。
    const ImVec2 corner{ImGui::GetFrameHeight() * 2.f, ImGui::GetFrameHeight() * 2.f};
    if (collapsed && fullScreen && x <= corner.x && y <= corner.y)
    {
        return false;
    }

    float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
    if (Unity::QueryMenuRect(x0, y0, x1, y1))
    {
        return x >= x0 && x <= x1 && y >= y0 && y <= y1;
    }
    // 矩形还没发布（菜单刚起来的第一帧）时退回旧判据。
    return io.WantCaptureMouse;
}

bool Input_GetMouseButton(int n, MethodInfo *method)
{
    // hook 失败时直通，绝不能调用空的原函数指针
    if (!oInput_GetMouseButton)
        return false;

    // 转发时必须把 il2cpp 传来的隐藏 MethodInfo* 原样传回去；
    // 用我们解析出来的那个兜底。只在 ImGui 可用时才拦截，
    // 否则直接透传，语义与原函数一致。
    auto *mi = method ? method : s_GetMouseButton;

    // ImGui context 已被销毁（初始化失败后 setupMenu 会 DestroyContext）时，
    // 不能再摸 ImGui::GetIO()：GImGui 为空 → 空指针解引用。
    // 这两个 hook 装在游戏的输入路径上，销毁 context 前必须先摘掉。
    if (!Unity::g_uiContextAlive)
        return oInput_GetMouseButton(n, mi);

    std::lock_guard<NeverDestroyedMutex> guard(Unity::InputMutex());
    // 上面那个检查只是「进来时 context 还活着」；拿到锁之后必须重新确认
    // —— 等锁期间渲染线程完全可能已经 DestroyContext 了。
    if (!Unity::g_uiContextAlive)
        return oInput_GetMouseButton(n, mi);

    ImGuiIO &io = ImGui::GetIO();

    ImVec2 size{ImGui::GetFrameHeight() * 2.f, ImGui::GetFrameHeight() * 2.f};
    if (io.WantCaptureMouse && !(collapsed && fullScreen && (io.MousePos.x > size.x && io.MousePos.y > size.y)))
        return false;
    return oInput_GetMouseButton(n, mi);
}
int get_touchCount(MethodInfo *method)
{
    // 输入 hook 没装成功时必须直通原函数，不能去调还为空的 o_get_touchCount
    if (!o_get_touchCount)
        return 0;

    auto *mi = method ? method : s_get_touchCount;

    // ImGui context 已销毁时不能摸 GetIO()，见上方说明
    if (!Unity::g_uiContextAlive)
        return o_get_touchCount(mi);

    // 整段持锁：下面所有 io.* 写入都往 g.InputEventsQueue 里 push_back，
    // 而渲染线程的 NewFrame 正在遍历并清空同一个 vector。详见
    // Unity::InputMutex 的注释。
    std::lock_guard<NeverDestroyedMutex> guard(Unity::InputMutex());
    if (!Unity::g_uiContextAlive)
        return o_get_touchCount(mi);

    ImGuiIO &io = ImGui::GetIO();

    auto count = o_get_touchCount(mi);
    if (count > 0 && Input)
    {
        // auto mousePresent = Input->invoke_static_method<bool>("get_mousePresent");
        // if (mousePresent)
        // {
        //     LOGD("MOUSE");
        //     io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
        // }
        // else
        // {
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        // }
        auto touch = Input->invoke_static_method<UnityEngine_Touch>("GetTouch", 0);
        float x = touch.m_Position.x;
        float y = io.DisplaySize.y - touch.m_Position.y;

        // 一次性自检：UnityEngine_Touch 的字段偏移是**假设**的（注释写
        // m_FingerId 在 0x10，即假定对象带 16 字节头），而按值返回的值类型
        // 其实**没有** Il2CppObject 头 —— 那样的话所有偏移都该减去 16。
        //
        // 判据很干脆：m_Phase 必须落在 0~3（Began/Moved/Ended/Canceled）。
        // 如果偏移错了，0x34 处读到的是别的字段（比如 m_Radius ≈ 0.5f，
        // 即 0x3F000000 = 1056964608），下面四个分支一个都不会命中 ——
        // 表现是**菜单对触摸完全无反应**，而且没有任何报错。
        //
        // 只报一次，不在热路径上刷屏。
        static bool offsetsChecked = false;
        if (!offsetsChecked)
        {
            offsetsChecked = true;
            const int phase = static_cast<int>(touch.m_Phase);
            if (phase < 0 || phase > 3)
            {
                s_touchOffsetOk = false;
                LOGE("UnityEngine_Touch 字段偏移疑似错误: m_Phase=%d (应为 0..3)，"
                     "m_Position=(%f, %f)。触摸将无法工作 —— "
                     "请对照 dump 出来的 UnityEngine.Touch 布局修正 Unity.h",
                     phase, (double)touch.m_Position.x, (double)touch.m_Position.y);
            }
            else
            {
                s_touchOffsetOk = true;
                LOGI("UnityEngine_Touch 偏移自检通过: m_Phase=%d, m_Position=(%f, %f)", phase,
                     (double)touch.m_Position.x, (double)touch.m_Position.y);
            }
        }

        if (touch.m_Phase == UnityEngine_TouchPhase::Began)
        {
            io.AddMousePosEvent(x, y);
            io.AddMouseButtonEvent(0, true);
            s_pressed = true;
            // **在手势开始的那一刻**就决定这次触摸归谁，之后不再变。
            //
            // 旧代码是每帧重新判断 WantCaptureMouse，于是手指从游戏区划到菜单上
            // 时判断会中途翻转：游戏先拿到 touchCount==1（Began），下一帧突然
            // 变成 0，而 Ended 从来没发给它。绝大多数游戏把「手指凭空消失」
            // 当作取消 —— 表现为点一下菜单，角色当场停住、镜头拖拽弹回、
            // 蓄力被丢弃。用起来就像菜单本身把游戏弄坏了。
            //
            // 现在用**当前触摸坐标**和菜单的实际矩形来判断，而不是用
            // io.WantCaptureMouse（后者由上一帧的鼠标位置算出，第一次按下
            // 时必然滞后一帧）。矩形由渲染线程每帧发布。
            s_touchCaptured = ShouldCaptureAt(x, y, io);
        }
        else if (touch.m_Phase == UnityEngine_TouchPhase::Ended)
        {
            io.AddMousePosEvent(x, y);
            io.AddMouseButtonEvent(0, false);
            io.AddMousePosEvent(-1, -1);
            s_pressed = false;
            s_touchCaptured = false;
        }
        else if (touch.m_Phase == UnityEngine_TouchPhase::Moved)
        {
            io.AddMousePosEvent(x, y);
        }
        else if (touch.m_Phase == UnityEngine_TouchPhase::Canceled)
        {
            // 系统中断这次触摸（来电、通知、任务切换）。
            // 旧代码漏了这个分支，ImGui 那边按钮会一直保持按下状态，
            // 直到下一次 Began/Ended 才对上 —— 表现为界面卡在「按住」。
            if (s_pressed)
            {
                io.AddMouseButtonEvent(0, false);
                io.AddMousePosEvent(-1, -1);
                s_pressed = false;
            }
            s_touchCaptured = false;
        }
    }
    else if (s_pressed)
    {
        // 手指数量归零了。Unity 偶尔不会给出 Ended（比如触摸被系统吃掉），
        // 这里兜底把按住状态松开，避免 ImGui 永远停在按下态。
        io.AddMouseButtonEvent(0, false);
        io.AddMousePosEvent(-1, -1);
        s_pressed = false;
        s_touchCaptured = false;
    }

    // 这次手势归菜单就返回 0，让游戏认为没有手指。
    // 注意是「整段手势都返回 0」—— Began 那一刻就已经决定了，
    // 所以游戏要么完整看到这次触摸，要么从头到尾看不到，
    // 绝不会看到「开始了却凭空消失」。
    if (s_touchCaptured)
    {
        return 0;
    }

    return count;
}

namespace Unity
{
    static Il2CppImage *g_Image; // REPLACE_* macro depends on g_Image

    // 输入 hook 是否完整可用。false 时 get_touchCount / Input_GetMouseButton
    // 必须直通，否则它们会调用还为空的原函数指针 → 跳进 0 地址。
    static bool g_inputHooked = false;

    void HookInput()
    {
        // 元数据没就绪 / 类名不存在时，直接放弃输入 hook，而不是硬着头皮解引用。
        g_Image = Il2cpp::GetImage("UnityEngine.InputLegacyModule"); // hack
        if (!g_Image)
        {
            LOGE("找不到 UnityEngine.InputLegacyModule，跳过输入 hook");
            return;
        }
        Input = g_Image->getClass("UnityEngine.Input");
        if (!Input)
        {
            LOGE("找不到 UnityEngine.Input 类，跳过输入 hook");
            return;
        }

        // 先把 MethodInfo 留下来：hook 里转发给原函数时要用
        // （游戏正常调用会把隐藏的 MethodInfo* 传进来，但兜底路径需要我们自己的）。
        s_get_touchCount = Input->getMethod("get_touchCount");
        s_GetMouseButton = Input->getMethod("GetMouseButton");
        if (!s_get_touchCount || !s_GetMouseButton)
        {
            LOGE("找不到 Input.get_touchCount / GetMouseButton，跳过输入 hook");
            s_get_touchCount = nullptr;
            s_GetMouseButton = nullptr;
            return;
        }

        REPLACE_NAME_ORIG("UnityEngine.Input", "get_touchCount", get_touchCount,
                          o_get_touchCount); // TODO: pass image to REPLACE macro
        REPLACE_NAME_ORIG("UnityEngine.Input", "GetMouseButton", Input_GetMouseButton, oInput_GetMouseButton);

        // 两个原函数指针都必须拿到，缺一个就不能接管输入。
        // 旧实现只判 orig 为空；这里还要回滚已经装上的那一个 ——
        // 否则 Input_GetMouseButton 会用空 orig 直接 return false，
        // 等于把游戏鼠标输入整个禁掉了。
        if (!o_get_touchCount || !oInput_GetMouseButton)
        {
            LOGE("输入 hook 安装不完整（touch=%p mouse=%p），回滚并直通原函数", (void *)o_get_touchCount,
                 (void *)oInput_GetMouseButton);
            if (o_get_touchCount)
            {
                DobbyDestroy((void *)s_get_touchCount->methodPointer);
                MethodInfo::_removeFromHookedMap((uintptr_t)s_get_touchCount->methodPointer);
                o_get_touchCount = nullptr;
            }
            if (oInput_GetMouseButton)
            {
                DobbyDestroy((void *)s_GetMouseButton->methodPointer);
                MethodInfo::_removeFromHookedMap((uintptr_t)s_GetMouseButton->methodPointer);
                oInput_GetMouseButton = nullptr;
            }
            s_get_touchCount = nullptr;
            s_GetMouseButton = nullptr;
            g_inputHooked = false;
            return;
        }
        g_inputHooked = true;
    }

    void UninstallInputHooks()
    {
        // 顺手清掉手势归属标记。万一摘 hook 时正有一次触摸被判给了菜单，
        // 这个 true 会留下来；下次重新装 hook 后游戏就一直收不到手指
        // （表现为「重新初始化之后游戏点不动了」）。
        s_touchCaptured = false;
        s_pressed = false;
        if (!g_inputHooked)
        {
            return;
        }
        // 销毁 ImGui context 之前必须把输入 hook 摘掉：
        // 否则游戏下一次调 Input.get_touchCount 会进来摸已经不存在的 ImGui 上下文。
        // g_uiContextAlive 置 false 让两个 hook 先直通原函数，摘干净后再销毁 context。
        g_uiContextAlive = false;
        s_pressed = false;
        if (Input)
        {
            // 摘钩子的同时要把 alreadyHooked 里的登记清掉。
            // 只 DobbyDestroy 的话这张表还留着失效的 trampoline 记录，
            // 之后再 hook 同一个方法会被 _isAlreadyHooked 挡掉并返回 nullptr。
            for (MethodInfo *m : {s_get_touchCount, s_GetMouseButton})
            {
                if (!m || !m->methodPointer)
                {
                    continue;
                }
                if (DobbyDestroy((void *)m->methodPointer) == 0)
                {
                    MethodInfo::_removeFromHookedMap((uintptr_t)m->methodPointer);
                }
                else
                {
                    LOGE("卸载输入 hook 失败: %s", m->getName() ? m->getName() : "?");
                }
            }
        }
        s_get_touchCount = nullptr;
        s_GetMouseButton = nullptr;
        o_get_touchCount = nullptr;
        oInput_GetMouseButton = nullptr;
        g_inputHooked = false;
        LOGI("已卸载 Unity 输入 hook");
    }
} // namespace Unity
