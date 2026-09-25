#pragma once
#include <jni.h>
#include <android/log.h>

// pasted straight from the imgui_demo.cpp
namespace logger
{
    void Clear();

    void AddLog(const char *prefix, const char *fmt, ...);

    void DebugLog(const char *fmt, ...);

    void Draw(const char *title, bool *p_open = 0);
}; // namespace logger

// 日志分级策略
//
// **旧实现的问题**：整个宏体系被 `__DEBUG__` 一刀切，而 Android.mk 从来没有
// 定义过它 —— 也就是说正式发布的构建里 `LOGE(...)` 展开成**空语句**，
// 一条诊断信息都不会产生。
//
// 这对一个逆向工具尤其致命：用户报「打补丁没反应」「dump 失败」，
// 而工具这边连一句「因为 XXX 被拒绝了」都不可能打出来 —— 所有排障路径
// 全部断掉。比如 Patcher 现在会明确说「stub 装不下这个方法，已拒绝」，
// 在 release 下这句话是编译掉的，用户只看到「按钮点了没变化」。
//
// 改成按**严重程度**分级，而不是按构建类型一刀切：
// - LOGD：非常吵（热路径里到处是），默认关，需要时用 __DEBUG__ 打开
// - LOGW/LOGE/LOGI：偶发且信息量大，**永远开启**
//
// 代价是 release 下 logcat 里会多出一些 W/E 行。这是可接受的：
// 它们只在真正出问题时出现，而且这正是排障时唯一能拿到的东西。
// 需要完整的 D 级日志时，用 build.ps1 的 -Debug 开关构建。
#define LOG_TAG "MXP"

#ifdef __DEBUG__
    // 调试构建：连 D 级一起全开。
    #define LOGD(...)                                                                                                  \
        do                                                                                                             \
        {                                                                                                              \
            logger::AddLog("[D] ", __VA_ARGS__);                                                                       \
            __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__);                                              \
        } while (0)
#else
    // 正式构建：D 级静默（它太吵，且对排障帮助有限）。
    // 其余级别照常输出 —— 见上面的说明。
    #define LOGD(...)                                                                                                  \
        do                                                                                                             \
        {                                                                                                              \
        } while (0)
#endif

#define LOGW(...)                                                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        logger::AddLog("[W] ", __VA_ARGS__);                                                                           \
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__);                                                   \
    } while (0)
#define LOGE(...)                                                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        logger::AddLog("[E] ", __VA_ARGS__);                                                                           \
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__);                                                  \
    } while (0)
#define LOGI(...)                                                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        logger::AddLog("[I] ", __VA_ARGS__);                                                                           \
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__);                                                   \
    } while (0)

#define LOGPTR(ptr) LOGD(#ptr " => %p", ptr)
#define LOGHEX(ptr) LOGD(#ptr " => 0x%llX", ptr)
#define LOGINT(var) LOGD(#var " => %d", var)
#define LOGSINGLE(var) LOGD(#var " => %f", var)
#define LOGSTR(il2cppstring) LOGD(#il2cppstring " => %s", il2cppstring->to_string().c_str())
