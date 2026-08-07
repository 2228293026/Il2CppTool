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
    void Do(const std::string &result)
    {
        lastCallback(result);
        lastCallback = nullptr;
        userData = nullptr;
    }
    std::function<void(const std::string &)> lastCallback;
};
