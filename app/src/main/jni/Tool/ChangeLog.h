#pragma once

#include <cstddef>
#include <cstdint>
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
    // ---- 恢复（撤销）所需的信息 ----
    //
    // 没有这些就只能「看」，不能「退回去」。而对改内存的工具来说，
    // 「退回去」比「看」常用得多：冻了个值玩够了、补丁打错了、
    // 想让游戏恢复正常 —— 每一次都需要撤销，而不是「记得当时
    // 改成了多少」再手填回去。
    std::string oldValue;  // 改动**之前**的值（文本形式）
    std::string type;      // 声明类型名，如 "System.Int32"
    std::string field;     // 字段名，如 "health"
    uint32_t handle = 0;   // 根对象的 GC 句柄（0 = 不可恢复）
};

// 记录一条。thread-safe。target/detail 会被截断到合理长度。
void Record(Kind kind, const std::string &target, const std::string &detail);

// 记录一条**可撤销**的字段改动。
//
// handle 必须是**根对象**的 GC 强根，所有权转移给记录表 ——
// 记录表会在条目被淘汰或 Clear() 时自己释放它。
// 传 0 表示这次改动不可撤销（对象已经没了、或者「整对象保存」
// 这类没有「单个旧值」可言的操作）。
void RecordUndoable(Kind kind, const std::string &target, const std::string &oldValue,
                    const std::string &newValue, uint32_t handle, const std::string &type,
                    const std::string &field);

// 撤销动作由外部注入：ChangeLog 不认识 il2cpp，也不该认识。
// 返回 true 表示恢复成功。
using Restorer = bool (*)(const Entry &entry);
void SetRestorer(Restorer fn);

// GC 句柄的释放器。和撤销器一样由外部注入 —— ChangeLog 不认识 il2cpp，
// 但它需要**在条目被淘汰 / 清空时把句柄还回去**，否则那个对象
// 永远不会被回收（512 条记录可能对应几百个游戏对象）。
using HandleReleaser = void (*)(uint32_t handle);
void SetHandleReleaser(HandleReleaser fn);

// 这一条现在还能不能撤销（对象还在、值没被恢复过、有注入的恢复器）。
bool CanUndo(const Entry &entry);
// 真正执行恢复。
bool Undo(const Entry &entry);
// 恢复成功后调用：这一条的可恢复状态作废（再点一次会跳过）。
void MarkUndone(const Entry &entry);

// 快照。返回的副本之后随便改，不影响内部状态。
std::vector<Entry> Snapshot();

// 条数。渲染线程每次都拷整个 vector 的话，先问一句更划算。
size_t Count();

void Clear();

void DrawUI();
} // namespace ChangeLog
