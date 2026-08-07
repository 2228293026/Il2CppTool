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
        LOGD("Keyboard Open");
        openedKeyboard = TouchScreenKeyboard->invoke_static_method<Il2CppObject *>("Open", Il2cpp::NewString(text), 0,
                                                                                   0, 0, 0, Il2cpp::NewString(""), 0);
        lastCallback = callback;
    }

    void Reset()
    {
        lastCallback = nullptr;

        // private System.Void Destroy(); // 0x28c52e8
        // protected override System.Void Finalize(); // 0x28c53b4
        static auto Destroy = openedKeyboard->klass->getMethod("Destroy");
        static auto Finalize = openedKeyboard->klass->getMethod("Finalize");
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
            static auto get_statusMethod = TouchScreenKeyboard->getMethod("get_status");
            // static auto get_status = (TouchScreenKeyboardStatus(*)(void *, MethodInfo *, Il2CppObject *,
            //                                                        void *))get_statusMethod->invoker_method;
            TouchScreenKeyboardStatus status = Canceled;
            if (check)
            {
                status = openedKeyboard->invoke_method<TouchScreenKeyboardStatus>("get_status");
            }
            else
            {
                // status = get_status(get_statusMethod->methodPointer, get_statusMethod, openedKeyboard, nullptr);
                auto result = Il2cpp::RuntimeInvoke(get_statusMethod, openedKeyboard, nullptr, nullptr);
                if (!result)
                {
                    LOGE("Failed to get status");
                    // static auto set_activeMethod = TouchScreenKeyboard->getMethod("set_active");
                    // if (set_activeMethod)
                    // {
                    //     // FIXME: maybe use invoker_method
                    //     // set_active->invoke_static<void>(openedKeyboard, 0);
                    //     auto set_active = (void (*)(bool))set_activeMethod->invoker_method;
                    //     set_active(0);

                    //     LOGD("set active");
                    // }
                    // else
                    // {
                    //     LOGE("Failed to set active");
                    // }
                    return Reset();
                }
                status = Il2cpp::GetUnboxedValue<TouchScreenKeyboardStatus>(result);
            }
            if (status == Done)
            {
                static auto get_textMethod = TouchScreenKeyboard->getMethod("get_text");
                static auto get_text =
                    (Il2CppString * (*)(void *, MethodInfo *, Il2CppObject *, void *)) get_textMethod->invoker_method;
                Il2CppString *text = nullptr;
                if (check)
                {
                    text = openedKeyboard->invoke_method<Il2CppString *>("get_text");
                }
                else
                {
                    text = get_text(get_textMethod->methodPointer, get_textMethod, openedKeyboard, nullptr);
                }

                auto result = text->to_string();
                if (lastCallback)
                {
                    lastCallback(result);
                }

                Reset();
                LOGD("Keyboard Done");
            }
            else if (status != Visible)
            {
                Reset();
                LOGD("Keyboard Canceled");
            }
        }
    }

    bool IsOpen()
    {
        return openedKeyboard != nullptr;
    }
} // namespace Keyboard
