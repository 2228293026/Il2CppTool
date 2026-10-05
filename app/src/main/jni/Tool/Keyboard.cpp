#include "Keyboard.h"
#include "Il2cpp/Il2cpp.h"
#include "string"

namespace Keyboard
{
    Il2CppClass *TouchScreenKeyboard = nullptr;
    Il2CppObject *openedKeyboard = nullptr;
    // openedKeyboard 的 GC 强根。
    //
    // 键盘是**托管对象**，从 TouchScreenKeyboard.Open() 返回到我们
    // Destroy() 它之间要横跨若干帧，期间游戏随时可能触发一次 GC。
    // 裸指针被回收后，updateImpl() 里的 `openedKeyboard->invoke_method`
    // 就是解引用野指针 —— 而且它发生在渲染线程的 eglSwapBuffers 钩子里，
    // 崩了就是整个游戏崩。
    // 句柄为 0 表示没打开键盘（也覆盖 NewHandle 失败的情况）。
    uint32_t openedKeyboardHandle = 0;

    std::function<void(const std::string &)> lastCallback = nullptr;

    void Init()
    {
        TouchScreenKeyboard = Il2cpp::FindClass("UnityEngine.TouchScreenKeyboard");
        LOGPTR(TouchScreenKeyboard);
        if (!TouchScreenKeyboard)
        {
            // 不在这里放弃：Open() 每次会重试（见那里的注释），
            // 这里只先说明，免得完全无声。
            LOGW("TouchScreenKeyboard 暂时不可用，将在每次打开键盘时重试");
        }
    }

    bool IsAvailable()
    {
        return TouchScreenKeyboard != nullptr;
    }

    void Open(const std::function<void(const std::string &)> &callback)
    {
        Open("", callback);
    }
    void Open(const char *text, const std::function<void(const std::string &)> &callback)
    {
        // 类可能不存在（没有该模块的 Unity 版本），也可能是**取得太早**：
        // Init() 只解析一次，而它跑在 on_init 里，不保证晚于游戏的元数据
        // 注册。一次失败就 null 到会话结束 = **整个工具的文本输入全废**
        // （搜索框、参数输入、字段编辑、预设名全靠它），
        // 而用户只会看到「点了没反应」。
        // 所以这里取不到就再试一次 —— 代价只是一次类查找。
        if (!TouchScreenKeyboard)
        {
            TouchScreenKeyboard = Il2cpp::FindClass("UnityEngine.TouchScreenKeyboard");
            if (TouchScreenKeyboard)
            {
                LOGI("TouchScreenKeyboard 已可用（此前解析失败，现已恢复）");
            }
        }
        if (!TouchScreenKeyboard)
        {
            LOGE("TouchScreenKeyboard 类不可用，无法打开键盘（文本输入不可用）");
            return;
        }
        LOGD("Keyboard Open");
        auto *kb = TouchScreenKeyboard->invoke_static_method<Il2CppObject *>(
            "Open", Il2cpp::NewString(text), 0, 0, 0, 0, Il2cpp::NewString(""), 0);
        if (!kb)
        {
            LOGE("TouchScreenKeyboard.Open 返回空");
            return;
        }
        // 上一次还没关就先关掉，否则旧键盘对象会一直挂在触屏上，
        // 而且旧句柄泄漏。
        //
        // 第 141 轮发现：原来这里**只有 FreeHandle、没有 Destroy**，
        // 而上面那句注释说的正是「旧键盘对象会一直挂在触屏上」。
        // TouchScreenKeyboard 背后是一个 OS 原生输入面板，
        // 只放 GC 句柄只是让托管侧可以被回收，**面板还在屏幕上** ——
        // 而 Unity 那边认为它还活着，再开一个就会出现两个键盘。
        //
        // 直接调 Reset() 而不是把它的逻辑抄一份：
        // 抄一份就要同步两处，第 138 轮（GetFilterFailure）就是这么
        // 漏掉的 —— 同一个语义有两份实现时，一定会只改一处。
        //
        // 守卫用 `IsOpen()` 而不是 `openedKeyboardHandle != 0`：
        // 加根失败那条路径（下面第 112 行）只 Destroy(kb) 就 return 了，
        // **没有清 openedKeyboard**，所以存在一个真实可达的状态：
        //
        //     openedKeyboard != nullptr   而   openedKeyboardHandle == 0
        //
        // 原来那个守卫在这里不成立 -> 旧键盘不会被关 ->
        // **正是这个函数要修的那个问题，在这个状态下依然发生**。
        //
        // （而且是我引入的：修复前这里无条件 FreeHandle，句柄至少被处理了。）
        if (IsOpen())
        {
            Reset();   // Destroy() + FreeHandle + openedKeyboard = nullptr
        }
        openedKeyboardHandle = Il2cpp::GC::NewHandle(kb);
        if (openedKeyboardHandle == 0)
        {
            // 加根失败 = 这个托管对象**随时可能被 GC 回收**。
            //
            // 而 updateImpl() 每帧都要解引用 openedKeyboard（get_status /
            // get_text），它跑在渲染线程的 eglSwapBuffers 钩子里 ——
            // 解引用一个被回收的对象就是崩游戏，而且崩的是**游戏**，
            // 不是这个工具。
            //
            // 旧代码在这里只 LOGW 一句「可能读到已回收对象」，然后
            // **照样** openedKeyboard = kb：明知没有根还继续每帧解引用。
            // Update() 的唯一守卫就是 if (openedKeyboard)，所以那条
            // 「继续走下去」的路径是**实际会发生**的，不是理论风险。
            //
            // 正确做法是**别开这个键盘**：把刚打开的关掉，
            // openedKeyboard 保持空 —— Update() 于是整个跳过。
            LOGE("键盘对象加根失败，已放弃打开（否则每帧会解引用可能已回收的对象）");
            if (kb->klass)
            {
                if (auto *destroyMethod = kb->klass->getMethod("Destroy"))
                {
                    destroyMethod->invoke_static<void>(kb);
                }
            }
            // 第 143 轮：这里原来只 Destroy + return，**没有清 openedKeyboard**。
            //
            // 而 Update() 的唯一守卫就是 `if (openedKeyboard)` ——
            // 留着它就等于「明明放弃了打开，每帧还是去解引用这个
            // 没有 GC 根的对象」，也就是上面整段注释在防的那件事。
            //
            // 这个状态还让 `Open()` 里「关掉上一个键盘」的守卫失效
            // （如果它写成 `openedKeyboardHandle != 0`），所以两处必须一致。
            openedKeyboard = nullptr;
            openedKeyboardHandle = 0;
            lastCallback = nullptr;
            return;
        }
        openedKeyboard = kb;
        lastCallback = callback;
    }

    void Reset()
    {
        lastCallback = nullptr;

        // private System.Void Destroy(); // 0x28c52e8
        // 旧代码在这里还额外调了一次 Finalize()。
        //
        // 那是**双重终结**：Finalize 是终结器，GC 迟早会自己调一次，
        // 我们先手动调一遍等于让同一对象被终结两次 —— 托管侧的
        // 资源释放跑两遍，行为未定义。而且它绕过正常的销毁路径。
        // 只需要 Destroy 就够了。
        //
        // 另外这段只在键盘确实打开过时才做：openedKeyboard 为空时
        // 旧代码静态初始化完就立刻解引用它。
        if (!openedKeyboard)
        {
            if (openedKeyboardHandle != 0)
            {
                Il2cpp::GC::FreeHandle(openedKeyboardHandle);
                openedKeyboardHandle = 0;
            }
            return;
        }
        auto Destroy = openedKeyboard->klass ? openedKeyboard->klass->getMethod("Destroy") : nullptr;
        if (Destroy)
        {
            Destroy->invoke_static<void>(openedKeyboard);
        }
        if (openedKeyboardHandle != 0)
        {
            Il2cpp::GC::FreeHandle(openedKeyboardHandle);
            openedKeyboardHandle = 0;
        }
        openedKeyboard = nullptr;
    }

    void Update()
    {
        if (openedKeyboard)
        {
            // 回调是在渲染线程上调用的，调用方里可能抛异常
            // （例如 ClassesTab 里的 std::stoi）。异常一旦逃出去，
            // 就会跳过 ImGui::Render() 并冲出 eglSwapBuffers 钩子 → 游戏崩。
            try
            {
                detail::updateImpl();
            }
            catch (const std::exception &e)
            {
                LOGE("Keyboard::Update 回调异常: %s", e.what());
                Reset();
            }
            catch (...)
            {
                LOGE("Keyboard::Update 未知异常");
                Reset();
            }
        }
    }

    namespace detail
    {
        void updateImpl()
        {
        // 方法指针**不许缓存失败**（第 141 轮）。
        //
        // 原来是函数级 static，初值 = TouchScreenKeyboard->getMethod(...)。
        // `static` 只初始化**一次**，所以一旦那一刻 getMethod 返回 nullptr
        // （元数据还没注册完 / 这个 Unity 版本没这个方法），
        // 之后**永远**是 nullptr —— 每帧都走下面的「找不到 get_status」
        // 直接 Reset()。
        //
        // 于是键盘永远打不开，而界面上一点提示都没有：
        // 用户看到的是「点了没反应」。而 `Open()` 里专门写了
        // 「类解析失败可以重试」（第 50-57 行），
        // 说明「暂时拿不到」在这份代码里是**会发生的**。
        //
        // 同一件事两处处理不同 = 必然有一处是错的。
        // 缓存**成功**的结果（省一次 getMethod），失败就走慢路径重试。
        //
        // 而且这里每次调用都 getMethod 也不贵：只在 status==Done 分支里
        // 才需要 get_text，而那是一次性事件（用户按完确定）。
        MethodInfo *get_statusMethod = TouchScreenKeyboard->getMethod("get_status");
        if (!get_statusMethod)
        {
            LOGE("找不到 get_status");
            return Reset();
        }
        TouchScreenKeyboardStatus status = Canceled;
        if (check)
        {
            status = openedKeyboard->invoke_method<TouchScreenKeyboardStatus>("get_status");
        }
        else
        {
            auto result = Il2cpp::RuntimeInvoke(get_statusMethod, openedKeyboard, nullptr, nullptr);
            if (!result)
            {
                LOGE("Failed to get status");
                return Reset();
            }
            status = Il2cpp::GetUnboxedValue<TouchScreenKeyboardStatus>(result);
        }
        if (status == Done)
        {
            // 同上：不缓存失败。get_text 只在 status==Done 时查一次，
            // 而那是用户按完确定的一次性事件，慢路径完全够用。
            MethodInfo *get_textMethod = TouchScreenKeyboard->getMethod("get_text");
            if (!get_textMethod)
            {
                LOGE("找不到 get_text");
                return Reset();
            }
            Il2CppString *text = nullptr;
            if (check)
            {
                text = openedKeyboard->invoke_method<Il2CppString *>("get_text");
            }
            else
            {
                auto get_text = (Il2CppString * (*)(void *, MethodInfo *, Il2CppObject *, void *))
                    get_textMethod->invoker_method;
                if (get_text)
                    text = get_text(get_textMethod->methodPointer, get_textMethod, openedKeyboard, nullptr);
            }

            // get_text 可能返回空（方法缺失 / 调用失败），旧代码直接 ->to_string() 空指针崩
            std::string resultText;
            if (text)
            {
                resultText = text->to_string();
            }
            else
            {
                LOGE("get_text 返回空");
            }
            if (lastCallback)
            {
                // 关键：**先把回调搬走并清空状态，再调用它**。
                //
                // 旧顺序是 `lastCallback(resultText); Reset();`，而 Reset() 会
                // 做 `lastCallback = nullptr` + 销毁键盘对象。问题在于：如果
                // 回调内部又调了 Keyboard::Open()（链式输入，比如填完一个
                // 参数接着问下一个），它刚设好的 lastCallback / openedKeyboard
                // 会被紧随其后的 Reset() 全部抹掉 —— 用户点了确定，界面
                // 「什么都没发生」。
                //
                // 先取走回调（同时清空 lastCallback 表达「本次已消费」），
                // 再调用。回调里新开的那一轮就不受影响了。
                auto callback = std::move(lastCallback);
                lastCallback = nullptr;
                callback(resultText);

                // 只有当回调**没有**重开键盘时才收尾销毁。
                // 重开了就说明那是一个新的交互周期，把人家的对象留着。
                if (!IsOpen())
                {
                    Reset();
                    LOGD("Keyboard Done");
                }
            }
            else
            {
                Reset();
            }
        }
        else if (status != Visible)
        {
            Reset();
            LOGD("Keyboard Canceled");
        }
        } // namespace detail
    }

    bool IsOpen()
    {
        return openedKeyboard != nullptr;
    }
} // namespace Keyboard
