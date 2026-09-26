#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

// 改动记录：把「**这个工具改动了目标游戏的什么**」记成一份可导出的清单。
//
// 为什么需要它
//
// 一次分析下来，字段可能改了十几个、补丁可能打了五六个，还 hook 了
// 一堆方法。十分钟后你已经想不起来「当时到底把哪个字段设成了多少」。
// 而这些改动**只存在于游戏进程内存里**：游戏一关全部消失，工具自己
// 也不留任何记录。
//
// 于是会出现很典型的丢数据场景：改了值 → 忘了改的是哪个字段 →
// 游戏崩了 / 换了个场景 / 想复现效果时，只能从头再找一遍。
//
// 关键性质：**只记录，不参与**任何游戏逻辑。写记录失败绝不能影响
// 工具本身，所以全程加锁 + try/catch，且不持有任何 il2cpp 对象指针。
namespace ChangeLog
{
// kind 只用于界面上分组和配色，不是可扩展的枚举协议。
enum class Kind
{
    Field,   // 改了某个对象的字段
    Patch,   // 给方法打了补丁
    Hook,    // 开始/取消追踪
    Watch,   // 加了关注
    Save,    // 保存了对象
};

struct Entry
{
    Kind kind;
    std::string target;  // 例："Player.health" / "UnityEngine.Camera::get_main"
    std::string detail;  // 例："100 -> 999" / "Int32 返回值改为 1"
};

// 记录一条。thread-safe。target/detail 会被截断到合理长度。
void Record(Kind kind, const std::string &target, const std::string &detail);

// 快照。返回的副本之后随便改，不影响内部状态。
std::vector<Entry> Snapshot();

// 条数。渲染线程每次都拷整个 vector 的话，先问一句更划算。
size_t Count();

void Clear();

void DrawUI();
} // namespace ChangeLog
