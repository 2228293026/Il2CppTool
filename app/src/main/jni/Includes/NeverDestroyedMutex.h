#pragma once

#include <mutex>

// 一个「永不析构」的互斥量。
//
// 为什么需要它：本项目是**注入进别人游戏进程**的共享库。命名空间作用域的
// `static std::mutex g_xxx;` 会在库卸载时被析构（.fini_array / __cxa_atexit），
// 而那一刻游戏线程、hook 回调线程、后台扫描线程都可能还在跑。
//
// 跨翻译单元的静态对象析构顺序是**未指定**的。一旦某个 static 容器或
// static 对象先于 mutex 被析构，之后再有代码去 lock 这把 mutex，就是
// 在一个已析构的对象上操作 —— pthread 实现上轻则死锁、重则崩溃。
//
// 而且这类问题极难定位：只在**游戏退出时**偶发，表现为「退出时卡死/崩溃」，
// 和工具的任何功能都看不出关联。
//
// 做法是故意泄漏：进程生命周期内始终有效，进程结束时由内核统一回收。
// 代价是这几块内存在进程结束前不归还 —— 每把 mutex 几十字节，可以忽略。
//
// 用法与 std::mutex 完全一致（满足 BasicLockable / Lockable），
// 所以 std::lock_guard / std::unique_lock 都能直接用。
class NeverDestroyedMutex
{
  public:
    NeverDestroyedMutex() = default;
    NeverDestroyedMutex(const NeverDestroyedMutex &) = delete;
    NeverDestroyedMutex &operator=(const NeverDestroyedMutex &) = delete;

    void lock()
    {
        impl().lock();
    }

    void unlock()
    {
        impl().unlock();
    }

    bool try_lock()
    {
        return impl().try_lock();
    }

    std::mutex &impl()
    {
        // 函数内 static 的初始化是线程安全的（C++11 起保证）。
        // 故意 new 而不是栈上/值语义持有：这样它就不会进入任何 .fini_array。
        static std::mutex *m = new std::mutex();
        return *m;
    }
};
