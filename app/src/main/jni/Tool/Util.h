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

    class FileWriter
    {
      public:
        FileWriter() = default;
        FileWriter(const std::string &fileName);
        void open();
        void init(const std::string &fileName);
        void write(const char *data);
        bool exists();
        ~FileWriter();

      private:
        std::ofstream fileStream;
        std::string fileName;
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
