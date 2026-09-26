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
    // TouchScreenKeyboard 是否可用。不可用时**所有**文本输入都不可用
    // （搜索框 / 参数 / 字段编辑 / 预设名全靠它），而界面上看不出原因，
    // 所以自检页要显式列出这一项。
    bool IsAvailable();
    namespace detail
    {
        void updateImpl(); // 真正的轮询逻辑，由 Update() 加异常边界后调用
    }
}; // namespace Keyboard
