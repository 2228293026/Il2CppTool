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

// This menu_addr is used to allow for multiple game support in the future
void *initModMenu(void *menu_addr, void *on_init_addr = nullptr, bool isJni = false);

void setupMenu();

void internalDrawMenu(int width, int height);

EGLBoolean swapbuffers_hook(EGLDisplay dpy, EGLSurface surf);

int getGlWidth();
int getGlHeight();
