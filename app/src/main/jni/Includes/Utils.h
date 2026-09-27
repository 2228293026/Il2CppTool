#ifndef UTILS
#define UTILS

#include <jni.h>
#include <functional>
#include <string>

typedef unsigned long DWORD;

DWORD findLibrary(const char *library);

DWORD getAbsoluteAddress(const char *libraryName, DWORD relativeAddr);

jboolean isGameLibLoaded(JNIEnv *env, jobject thiz);

bool isLibraryLoaded(const char *libraryName);

uintptr_t string2Offset(const char *c);

void patchOffsetSym(uintptr_t absolute_address, std::string hexBytes, bool isOn);

void patchOffset(const char *fileName, uint64_t offset, std::string hexBytes, bool isOn);

namespace ToastLength
{
    inline const int LENGTH_LONG = 1;
    inline const int LENGTH_SHORT = 0;
} // namespace ToastLength

// 把 textFn() 的结果放进剪贴板（第 118 轮）。
//
// **保证不抛** —— 这一点比它看起来重要得多。
//
// textFn 是「拼一整份字符串」的那段代码：改动记录的导出、关注值整份、
// 自检报告，都要 += 几百次。内存不够时它会抛 std::bad_alloc。
//
// 而这些按钮全都画在**渲染线程**上，也就是 eglSwapBuffers 钩子里。
// 异常一路逃出去的后果不是「这一次复制没成功」，而是
// **这一整帧的菜单都不画** —— 用户看到的是界面闪一下、剪贴板没变，
// 完全不知道是自己操作的问题还是工具崩了。
//
// 所以边界放在这里，由这一个函数统一兜住：
//   失败 -> 返回 false，并把原因写进 *error，由调用方**如实告诉用户**。
//   成功 -> 返回 true。
//
// 为什么用回调而不是 `const std::string&`：
// 抛的那部分（拼接）必须**在边界里面**。如果让调用方先拼好再传进来，
// 异常早就发生了，边界根本来不及。
bool CopyToClipboard(const std::function<std::string()> &textFn, std::string *error = nullptr);

#endif
