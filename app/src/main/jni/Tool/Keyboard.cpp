#include "Keyboard.h"
#include "Il2cpp/Il2cpp.h"
#include "string"

namespace Keyboard
{
    Il2CppClass *TouchScreenKeyboard = nullptr;
    Il2CppObject *openedKeyboard = nullptr;
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
        openedKeyboard = kb;
        lastCallback = callback;
    }

    void Reset()
    {
        lastCallback = nullptr;

        // private System.Void Destroy(); // 0x28c52e8
        // protected override System.Void Finalize(); // 0x28c53b4
        // 旧代码在这里 static 初始化并立刻解引用 openedKeyboard->klass：
        // 键盘从没打开过时 openedKeyboard 是空的 → 空指针解引用。
        if (!openedKeyboard)
        {
            return;
        }
        auto Destroy = openedKeyboard->klass->getMethod("Destroy");
        auto Finalize = openedKeyboard->klass->getMethod("Finalize");
        if (Destroy)
        {
            Destroy->invoke_static<void>(openedKeyboard);
        }
        if (Finalize)
        {
            Finalize->invoke_static<void>(openedKeyboard);
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
                lastCallback(resultText);
            }

            Reset();
            LOGD("Keyboard Done");
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
