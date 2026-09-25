#pragma once
#include "string"
#include <functional>

class PopUpSelector
{
  private:
    std::string needOpen = "";

    void *userData = nullptr;

  public:
    void Open(const std::string &type, const std::function<void(const std::string &)> &callback, void *data = nullptr);

    void Update();

  private:
    // 触发一次选择。
    //
    // 关键：**先把回调搬走、清空状态，再调用它**。
    //
    // 旧顺序是 `lastCallback(result); lastCallback = nullptr; userData = nullptr;`
    // —— 如果回调内部又调了 Open()（链式选择），它刚设好的 lastCallback 和
    // userData 会被紧随其后的两行全部抹掉，于是新弹窗打开了却点不动。
    // 先取走再调用，回调里新开的那一轮就不受影响。
    void Do(const std::string &result)
    {
        if (!lastCallback)
        {
            return;
        }
        auto callback = std::move(lastCallback);
        lastCallback = nullptr;
        userData = nullptr;
        callback(result);
    }
    std::function<void(const std::string &)> lastCallback;
};
