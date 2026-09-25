#pragma once
#include "Il2cpp/il2cpp-class.h"
#include "Includes/circular_buffer.h"
#include "Tool/ClassesTab.h"
#include <set>

// 「最近被调用」列表里的一项。
struct HookerTrace
{
    // 方法地址。用它做去重键 —— 旧实现在每次 hook 回调里先 snprintf 出
    // 一串 512 字节的 "地址 | 方法名"，再拿这串去做线性查找比较。
    // 那段代码跑在**游戏线程**上、而且持有全局互斥锁：被 hook 的方法每秒被
    // 调上千次时，就是每秒上千次格式化 + 上千次字符串比较全在游戏主路径上，
    // 直接拖慢游戏。改成按指针去重后，热路径上只剩一次整数比较。
    void *address = nullptr;

    // 显示用的文本。只在**首次**命中时格式化一次（见 hookerHandler）。
    std::string name;

    float time = 0.f;
    float goneTime = 0.f;
    int hitCount = 0;
};

struct HookerData
{
    // 被 hook 方法的累计调用次数。
    //
    // 由游戏线程的 hook 回调递增，UI 读取。两者都用 relaxed 原子而不是
    // 互斥锁：这是一个纯统计计数器，丢一次更新没有任何后果，而为它
    // 在游戏的每次函数调用上 contending 一把全局锁，代价比数据本身大得多。
    std::atomic<int> hitCount{0};

    // 累计调用次数的增速（次/秒）历史，供 UI 画曲线。
    // 只在渲染线程被采样/绘制，hook 回调不碰它，所以不需要锁。
    std::vector<float> rateHistory;
    int sampledHitCount = 0;
    double lastSampleTime = 0.0;
    float callsPerSecond = 0.f;

    float time = 0.f;
    MethodInfo *method = nullptr;
    bool backtracing = false;
    CircularBuffer<std::vector<std::string>> backtraced{10};
    static CircularBuffer<HookerTrace> visited;
    static std::unordered_map<Il2CppClass *, std::set<Il2CppObject *>> collectSet;
};
namespace Tool
{
    void ConfigSave();
    void ConfigLoad();
    void Init(Il2CppImage *image, std::vector<Il2CppImage *> images);
    void FilterClasses(const std::string &filter);
    void Draw();
    void Tracer();
    void Hooker();
    void GameObjects();
    void Dumper();
    // 停止并回收 dump 工作线程。必须在线程仍 joinable 时调用，
    // 否则全局 std::thread 的析构会 std::terminate。
    void ShutdownDumper();
    bool ToggleHooker(MethodInfo *method, int state = -1);
    void CalculateSomething();
    // 每帧在渲染线程调用：采样各 hook 的调用频率。
    // 绝不能放进 hook 回调里 —— 那是游戏的执行路径。
    void SampleHookRates();
    ClassesTab &GetFirstTab(); // TODO: maybe just return classesTabs
    ClassesTab &OpenNewTab();
    ClassesTab &OpenNewTabFromClass(Il2CppClass *klass);
} // namespace Tool
