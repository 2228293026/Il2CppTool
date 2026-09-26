#pragma once
#include "imgui/imgui.h"
#include <string>
#include <fstream>

namespace Util
{
    // capacity 是 buffer 的总字节数（含结尾 '\0'），必须有。
    // 旧版本没有容量参数：调用方是 512 字节的 treeLabel，而里面的
    // 类名/方法名/返回值文本长度都不受控，连续 prepend 几次就栈溢出。
    void prependStringToBuffer(char *buffer, size_t capacity, const char *string);
    std::string extractClassNameFromTypename(const char *typeName);

    // 数据目录（带「只缓存成功 + 失败会重试」的逻辑，见 Util.cpp 的 DataPath()）。
    //
    // ConfigSave 的原子写需要自己拼最终路径，所以要能拿到这个路径 ——
    // 但**必须走这里**，不能直接调 Il2cpp::getDataPath()：
    // 那个「上层一次 static 缓存抵消下层修复」的坑踩过一次。
    const std::string &DataPathString();

    // **原子写**：所有内容先写进 `<名字>.part`，析构时成功才 rename 成正式文件。
    //
    // 旧实现直接 ofstream 打开正式文件（默认 trunc），于是进程在写的中途
    // 被杀（游戏崩、用户强杀、系统回收），磁盘上留下的是**半截文件**。
    // 下次启动读到它、解析失败、于是**全部**配置丢失 ——
    // 而保存那一下「看起来是成功的」。
    //
    // 为什么放在这里而不是每个调用点各自处理：第 89 轮已经见过
    // 「导出 .cs 有原子写、配置没有」这种同项目内策略不一致。
    // 机制只该有一份，放在**所有写入必经的入口**上。
    class FileWriter
    {
      public:
        FileWriter() = default;
        FileWriter(const std::string &fileName);
        void open();
        void init(const std::string &fileName);
        void write(const char *data);
        bool exists();
        // 析构之后想知道成不成功，就看这个 —— 失败时它会明确报出来，
        // 而不是像旧代码那样只有一行 LOGE，调用方完全不知情。
        bool ok() const
        {
            return m_ok;
        }
        // 正式文件的完整路径（调用方要自己拼路径时用得上）。
        const std::string &path() const
        {
            return m_finalPath;
        }
        ~FileWriter();

      private:
        std::ofstream fileStream;
        std::string fileName;    // 实际打开的是临时文件
        std::string m_finalPath; // rename 的目标
        std::string m_tempPath;
        bool m_dirty = false;
        bool m_ok = false;
    };

    class FileReader
    {
      public:
        FileReader(const std::string &fileName);
        std::string read();
        bool exists();
        ~FileReader();

      private:
        std::ifstream fileStream;
        std::string fileName;
    };
} // namespace Util

namespace ImGui
{
    void ScrollWhenDraggingOnVoid_Internal(const ImVec2 &delta, ImGuiMouseButton mouse_button);
    void ScrollWhenDraggingOnVoid(); // calls ScrollWhenDraggingOnVoid_Internal(ImVec2(0.0f, -mouse_delta.y),
                                     // ImGuiMouseButton_Left)
    bool IsItemHeld(float holdTime = 0.5f);

    void FpsGraph_Internal(const char *label, const std::vector<float> &fpsBuffer,
                           const ImVec2 &graphSize = ImVec2(-1, 200));
    void FpsGraph();

} // namespace ImGui
