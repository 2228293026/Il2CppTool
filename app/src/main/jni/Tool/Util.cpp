#include "Util.h"
#include "Il2cpp/Il2cpp.h"
#include <cstring>
#include <sstream>
#include "imgui//imgui_internal.h"

namespace Util
{
    // https://stackoverflow.com/a/2328191
    void prependStringToBuffer(char *buffer, const char *string)
    {
        size_t string_length = strlen(string);
        size_t buffer_length = strlen(buffer);
        memmove(buffer + string_length, buffer, buffer_length + 1);
        memcpy(buffer, string, string_length);
    }

    std::string extractClassNameFromTypename(const char *typeName)
    {
        std::string nameStr{typeName};
        size_t dotIndex = nameStr.find_last_of('.');
        size_t ltIndex = nameStr.find_first_of("<");
        std::string classNamespace;

        if (dotIndex == std::string::npos)
        {
            classNamespace = "";
        }
        else
        {
            if (ltIndex == std::string::npos)
            {
                classNamespace = nameStr.substr(0, dotIndex);
            }
            else
            {
                if (dotIndex > ltIndex)
                {
                    dotIndex = nameStr.find_last_of('.', ltIndex);
                    classNamespace = nameStr.substr(0, dotIndex);
                }
            }
        }
        return nameStr.substr(dotIndex + 1);
    }

    FileWriter::FileWriter(const std::string &fileName)
    {
        static auto path = Il2cpp::getDataPath();
        this->fileName = path + "/" + fileName;
        this->open();
    }

    void FileWriter::init(const std::string &fileName)
    {
        static auto path = Il2cpp::getDataPath();
        this->fileName = path + "/" + fileName;
    }

    void FileWriter::open()
    {
        fileStream.open(this->fileName);
    }

    void FileWriter::write(const char *data)
    {
        fileStream << data;
        fileStream << std::endl;
    }

    bool FileWriter::exists()
    {
        return fileStream.is_open();
    }

    FileWriter::~FileWriter()
    {
        fileStream.close();
    }
    FileReader::FileReader(const std::string &fileName)
    {
        static auto path = Il2cpp::getDataPath();
        this->fileName = path + "/" + fileName;
        fileStream.open(this->fileName);
    }

    std::string FileReader::read()
    {
        std::stringstream buffer;
        buffer << fileStream.rdbuf();
        return buffer.str();
    }

    bool FileReader::exists()
    {
        return fileStream.is_open();
    }

    FileReader::~FileReader()
    {
        fileStream.close();
    }

} // namespace Util

namespace ImGui
{
    void ScrollWhenDraggingOnVoid_Internal(const ImVec2 &delta, ImGuiMouseButton mouse_button)
    {
        ImGuiContext &g = *ImGui::GetCurrentContext();
        ImGuiWindow *window = g.CurrentWindow;
        if (!window->DC.NavWindowHasScrollY)
        {
            return;
        }
        bool hovered = false;
        bool held = false;
        ImGuiID id = window->GetID("##scrolldraggingoverlay");
        ImGui::KeepAliveID(id);
        ImGuiButtonFlags button_flags = (mouse_button == 0)   ? ImGuiButtonFlags_MouseButtonLeft
                                        : (mouse_button == 1) ? ImGuiButtonFlags_MouseButtonRight
                                                              : ImGuiButtonFlags_MouseButtonMiddle;
        if (g.HoveredId == 0) // If nothing hovered so far in the frame (not same as IsAnyItemHovered()!)
            ImGui::ButtonBehavior(window->Rect(), id, &hovered, &held, button_flags);
        if (held && delta.x != 0.0f)
            ImGui::SetScrollX(window, window->Scroll.x + delta.x);
        if (held && delta.y != 0.0f)
            ImGui::SetScrollY(window, window->Scroll.y + delta.y);
    }
    void ScrollWhenDraggingOnVoid()
    {
        ImVec2 mouse_delta = ImGui::GetIO().MouseDelta;
        ScrollWhenDraggingOnVoid_Internal(ImVec2(0.0f, -mouse_delta.y), ImGuiMouseButton_Left);
    }

    bool IsItemHeld(float holdTime)
    {
        ImGuiContext &g = *GImGui;
        if (ImGui::IsItemActive())
        {
            if (g.HoveredIdTimer >= holdTime)
            {
                return true;
            }
        }
        return false;
    }

    void FpsGraph_Internal(const char *label, const std::vector<float> &fpsBuffer, const ImVec2 &graphSize)
    {
        static float dt = 0.f;
        static float currentFps = 0.f;
        dt += ImGui::GetIO().DeltaTime;
        if (dt >= .5f)
        {
            currentFps = 1.f / ImGui::GetIO().DeltaTime;
            dt = 0.f;
        }
        ImGui::Text("FPS %.1f", currentFps);
        PlotLines(label, fpsBuffer.data(), static_cast<int>(fpsBuffer.size()), 0, NULL, 0.0f, FLT_MAX, graphSize);
    }

    class FpsTracker
    {
      public:
        FpsTracker() : bufferSize(100), currentFrame(0)
        {
            fpsBuffer.resize(bufferSize, 0.0f);
        }

        void update(float deltaTime)
        {
            fpsBuffer[currentFrame] = 1.0f / deltaTime;
            currentFrame = (currentFrame + 1) % bufferSize;
        }

        const std::vector<float> &getFpsBuffer() const
        {
            return fpsBuffer;
        }

      private:
        int bufferSize;
        int currentFrame;
        std::vector<float> fpsBuffer;
    };

    FpsTracker fpsTracker;
    void FpsGraph()
    {
        fpsTracker.update(ImGui::GetIO().DeltaTime);
        ImGui::FpsGraph_Internal("FPS", fpsTracker.getFpsBuffer());
    }
} // namespace ImGui
