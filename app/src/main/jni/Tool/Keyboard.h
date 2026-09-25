#pragma once

#include <functional>
enum TouchScreenKeyboardStatus
{
    Visible = 0,
    Done = 1,
    Canceled = 2,
    LostFocus = 3
};

namespace Keyboard
{
    inline bool check = false;
    void Init();
    void Open(const std::function<void(const std::string &)> &callback);
    void Open(const char *text, const std::function<void(const std::string &)> &callback);
    void Reset();
    void Update();
    bool IsOpen();
    namespace detail
    {
        void updateImpl(); // 真正的轮询逻辑，由 Update() 加异常边界后调用
    }
}; // namespace Keyboard
