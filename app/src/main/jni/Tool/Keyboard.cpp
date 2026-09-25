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
    }

    void Open(const std::function<void(const std::string &)> &callback)
    {
        Open("", callback);
    }
    void Open(const char *text, const std::function<void(const std::string &)> &callback)
    {
        // 类可能不存在（没有该模块的 Unity 版本）。
        // 旧代码直接 TouchScreenKeyboard->invoke_static_method，空指针就崩。
        if (!TouchScreenKeyboard)
        {
            LOGE("TouchScreenKeyboard 类不可用，无法打开键盘");
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
        if (openedKeyboardHandle != 0)
        {
            Il2cpp::GC::FreeHandle(openedKeyboardHandle);
            openedKeyboardHandle = 0;
        }
        openedKeyboardHandle = Il2cpp::GC::NewHandle(kb);
        if (openedKeyboardHandle == 0)
        {
            LOGW("键盘对象加根失败，后续状态查询可能读到已回收对象");
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
        static MethodInfo *get_statusMethod = TouchScreenKeyboard->getMethod("get_status");
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
            static MethodInfo *get_textMethod = TouchScreenKeyboard->getMethod("get_text");
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
