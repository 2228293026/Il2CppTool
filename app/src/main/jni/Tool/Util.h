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
