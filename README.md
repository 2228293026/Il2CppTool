# Il2CppTool

**Unity (il2cpp) 游戏运行时注入分析工具** —— 将一个 `.so` 注入进目标 il2cpp 游戏进程，在游戏画面上叠加 ImGui 悬浮窗，提供 **il2cpp 类/方法/字段浏览、方法追踪、DUMP、对象可视化、方法补丁** 等能力。

> 注意：仓库里原有的 README 是从 [LGLTeam/Android-Mod-Menu](https://github.com/LGLTeam/Android-Mod-Menu) 复制来的通用模板，与本仓库代码无关。本文按当前源码（v0.9）重新编写。

## 它是什么 / 不是什么

- 它是**运行时工具**，不是离线逆向器。它注入游戏进程后，直接读取运行中进程的 il2cpp 元数据（类、方法、字段、对象），操作不会保存 `global-metadata.dat` 之外的东西。
- 它本身需要一个**能加载 `libIl2CppTool.so` 的环境**（注入到游戏 APK / root 设备 / 模拟器），见下文「如何注入」。
- 构建它**不需要 Gradle、不需要 Android SDK**，只要有 NDK 即可（见「构建」）。仓库里的 Gradle 文件是历史遗留。目标进程必须是 **il2cpp** 编译的 Unity 游戏（运行时等待 `libil2cpp.so` 加载）。

## 功能

主界面悬浮窗（ImGui，中文界面）包含以下标签页：

- **工具**：核心浏览面板。可开任意多个「类标签页」，每个标签页：
  - 类 / 方法 / 字段的搜索与过滤（可切换大小写敏感、按类/方法/字段过滤）
  - 展开任意类查看其**字段（Field）与当前值、方法（Method）参数、嵌套类型**
  - **直接调用方法**（填参数→运行），查看返回值 / 异常
  - 按对象查看运行时对象图（`dataMap`，显示实例字段与类型层级）
  - 长按某对象/方法可把它在**新标签页**中打开继续分析；已打开的标签页会保留在 tab 栏里
- **追踪（Trace）**：对被关注的方法做 **frida-gum hook 追踪**，实时显示命中次数、最近调用、backtracer（frida 汇编回溯栈），可一键「恢复」（取消追踪）。
- **DUMP**：把当前进程的 il2cpp 类型树导出为 `.cs`（输出 `.cs` 文件到游戏数据目录），Dumper 基于 il2cpp 元数据（Perfree 体系的 il2cpp_dump）。
- **设置**：UI 缩放、全屏、是否拦截键盘输入；显示**包名 / 游戏版本 / Unity 版本 / 架构**；提供作者链接（bilibili）。配置存到 `tool_conf.json`。

此外还有底层能力（部分在「工具」内，部分常驻）：

- **对象绘制管理 / GameObjects ESP**：枚举进程内 `UnityEngine.GameObject` / `Camera`，用 `WorldToScreenPoint` 把世界坐标转屏幕坐标，在前台绘制框/线/点，可选自动刷新。
- **Patcher**：用 **asmjit** 就地改写 il2cpp 方法体（生成 `mov`/`movPtr` 等 AArch32/AArch64 指令补丁，写入方法所在的可写内存），用于做方法级修改。
- **内存操作**：KittyMemory（读/写/补丁/备份）。
- **字符串混淆**：`Includes/obfuscate.h`（AY 混淆）保护关键字符串。
- 按键输入由 Unity 自身的输入 hook 通道；当目标不在窗口线程时由 swapBuffers hook 投喂触控事件。

## 支持范围

- 架构：`arm64-v8a`（`Application.mk` 当前 `APP_ABI := arm64-v8a`；代码结构里保留了 `armeabi-v7a` 的预编译库，编成 `APP_ABI=armeabi-v7a` 也可用）。
- 系统：Android，需要目标游戏的进程里能加载我们的 so。

## 目录结构

```
app/src/main/jni/
├── Main.cpp            JNI 入口：等 libil2cpp.so 加载 → 初始化菜单/工具
├── Android.mk          模块定义：libIl2CppTool, libdobby, frida, asmjit(预编译)
├── Application.mk      APP_ABI=arm64-v8a, APP_PLATFORM=android-28, APP_STL=c++_static
├── Il2cpp/             il2cpp 运行时操作封装 + xdl(ELF 符号解析) + il2cpp_dump
├── Menu/               ImGui 渲染/输入管线（eglSwapBuffers hook）
├── imgui/              ImGui + android/opengl 后端 + 中文字体
├── Tool/               界面与功能：ClassesTab(浏览) Frida(追踪) Patcher ObjectDrawManager Dumper Unity(输入)
├── Frida/              frida-gum 静态库 + gumpp C++ 封装（追踪/回溯）
├── Dobby/              Dobby hook 库
├── KittyMemory/        内存补丁工具
├── asmjit/             汇编生成器（方法补丁）
└── Includes/           Logger / Utils / obfuscate / 字体
```

`install.txt`：注入一个游戏的 smali 片段（`System.loadLibrary("Il2CppTool")`）。

## 构建（Windows / Linux / Termux，无需 Gradle）

**前置**：安装好 Android NDK（r29 或兼容版本，64 位）。

Windows 上若已设好环境变量（`ANDROID_NDK_HOME=D:\android-ndk-r29`），直接：

```
build.bat
```

（PowerShell 版本，功能一致：）

```powershell
.\build.ps1                            # 走 ANDROID_NDK_HOME
.\build.ps1 D:\android-ndk-r29         # 显式 NDK 路径
.\build.ps1 APP_ABI=armeabi-v7a,arm64-v8a   # 追加 make 参数
# 若执行策略拦截：powershell -ExecutionPolicy Bypass -File .\build.ps1
```

或手动执行（Windows / Git Bash）：

```bash
# Windows cmd（注意反斜杠）：
#   cd app\src\main && "%ANDROID_NDK_HOME%\ndk-build.cmd" -j8
# Git Bash / Linux / Termux：
cd app/src/main
"$ANDROID_NDK_HOME/ndk-build.cmd" -j8     # Windows 下可用 /d/android-ndk-r29/ndk-build.cmd
# Linux / Termux： $ANDROID_NDK_HOME/ndk-build -j8
```

更多 ABI（可选）：

```
build.bat APP_ABI=armeabi-v7a,arm64-v8a
```

**输出**：`app/src/main/libs/<abi>/libIl2CppTool.so`（当前默认 `arm64-v8a`）。

> 若未设环境变量，也可以显式给路径：`build.bat D:\android-ndk-r29`（Windows）。

## 如何注入到游戏

核心是让目标游戏进程加载 `libIl2CppTool.so`，可以用两种方式：

1. **静态注入（改 APK，无需 root）** ：把 `libIl2CppTool.so` 放进解包游戏的 `lib/arm64-v8a/`，并在游戏 smali 中插入：

   ```smali
   const-string v0, "Il2CppTool"
   invoke-static {v0}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
   ```

   重新打包并签名安装（要求目标未启用签名校验）。

2. **Root 注入**：`adb push libIl2CppTool.so /data/local/tmp/` → `su -c 'mv /data/local/tmp/libIl2CppTool.so /data/app/.../lib/arm64-v8a/'`（具体路径看包的数据目录），完成后再让游戏重新加载（或挂载 `LD_LIBRARY_PATH` / 用注入器）。仓库里 Gradle 侧的 `sendToPhone`/`applySendToPhone` 任务就是这个流程。

游戏中打开游戏后，等待悬浮菜单浮现（菜单需要一个能触发加载的场景，通常游戏启动后）。

## 注意事项 / 免责声明

- 这是**研究 / 学习 / 自用**工具。用在在线游戏里可能违反用户协议甚至封号，请自行评估风险。
- 运行时会向游戏进程数据目录写入配置文件 `tool_conf.json`（UI 缩放等设置）。
- 本项目不含任何付费授权的东西；不要购买所谓“源码”。

## 依赖与致谢

代码内使用（各自许可证）：

- [ImGui](https://github.com/ocornut/imgui)（+ android/opengl 后端） — 界面
- [frida-gum](https://frida.re)（gumpp C++ 绑定）——方法追踪
- [Dobby](https://github.com/jmpe/Dobby) —— Hook（eglSwapBuffers 等）
- [asmjit](https://github.com/asmjit/asmjit) —— 汇编生成/方法补丁
- [KittyMemory](https://github.com/MJx0/KittyMemory)——内存操作
- [nlohmann/json](https://github.com/nlohmann/json) —— 配置 `tool_conf.json`
- il2cpp 访问与 Dumper 借鉴了 [Perfree/Zygisk-Il2CppDumper](https://github.com/Perfree/Zygisk-Il2CppDumper) 的思路
- 项目开发与维护：**HitMargin**（bilibili：https://m.bilibili.com/space/1757946676 ）

## License

[GNU General Public License v3.0](./LICENSE)