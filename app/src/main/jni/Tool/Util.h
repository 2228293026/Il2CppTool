#pragma once
#include "imgui/imgui.h"
#include <string>
#include <fstream>

namespace Util
{
    void prependStringToBuffer(char *buffer, const char *string);
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
