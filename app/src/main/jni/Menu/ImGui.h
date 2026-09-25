#pragma once
#include <EGL/egl.h>
void menuStyle();

// HOOKINPUT(void, Input, void *thiz, void *ex_ab, void *ex_ac)
// {
//     origInput(thiz, ex_ab, ex_ac);
//     ImGui_ImplAndroid_HandleInputEvent((AInputEvent *)thiz);
//     return;
// }

// 初始化状态。on_init 里「等 libil2cpp 加载」这类等待原先是在渲染线程上
// sleep 死等的，目标不对时首帧能卡住一分钟（ANR）。现在改成用每帧的
// eglSwapBuffers 钩子轮询：没就绪就跳过这一帧的菜单渲染，游戏照常出画面。
enum InitState
{
    INIT_PENDING = 0, // 还在等依赖，下一帧重试
    INIT_READY = 1,   // 初始化完成
    INIT_FAILED = 2,  // 彻底失败，不再重试
};
extern int g_initState;

// 重试多少次后放弃（按帧计，60fps 下 3600 帧约合 60 秒）
#define INIT_MAX_ATTEMPTS 3600

// 是否加载完整中日韩字形范围（约 21000 个字形）。
//
// 默认 false：只加载常用字（约 3000 个），把启动时的字体光栅化从
// 「好几秒」降到「几乎无感」。这个开关是启动性能的主要来源 —— 完整范围
// 的光栅化跑在渲染线程（eglSwapBuffers）上，用户看到的是开屏后长时间卡死。
//
// 读取时机很关键：必须在字体图集烘焙**之前**，也就是早于第一帧
// eglSwapBuffers。on_init 跑在图集烘焙之后，所以由 initModMenu
// （hack 线程）调 ReadFontConfigEarly() 提前解析配置文件。
extern bool g_fontFullRangeRequested;

// This menu_addr is used to allow for multiple game support in the future
void *initModMenu(void *menu_addr, void *on_init_addr = nullptr, bool isJni = false);

void setupMenu();

void internalDrawMenu(int width, int height);

EGLBoolean swapbuffers_hook(EGLDisplay dpy, EGLSurface surf);

int getGlWidth();
int getGlHeight();
