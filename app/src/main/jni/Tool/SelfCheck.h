#pragma once

#include <string>
#include <vector>

// 运行时自检。
//
// 为什么需要它：这是一个注入进别人游戏进程的库，很多问题**只在运行时**
// 才暴露，而且默认是**静默**的 —— 比如 ESP 拿不到相机时只是不画框，
// 触摸偏移错了时菜单干脆没反应，用户完全看不出是哪一环坏了。
//
// 自检把「哪些前置条件成立」一次性摊开，失败项直接说明原因。
// 配合「日志」页使用：自检告诉你哪一环坏了，日志告诉你为什么。

namespace SelfCheck
{
enum class Status
{
    Ok,     // 通过
    Warn,   // 有问题但不影响主要功能
    Fail,   // 该功能不可用
    Info,   // 仅展示信息
};

struct Result
{
    std::string name;
    Status status = Status::Info;
    std::string detail;
};

// 采集一次自检结果。**不要**在持有对象管理器内部锁的情况下调用 ——
// 它会去读那些状态。
std::vector<Result> Collect();

// 在界面上绘制自检结果。
void DrawUI();
} // namespace SelfCheck
