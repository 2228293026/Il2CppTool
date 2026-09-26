#include "Util.h"
#include "Il2cpp/Il2cpp.h"
#include <cstring>
#include <sstream>
#include "imgui//imgui_internal.h"

namespace Util
{
    // https://stackoverflow.com/a/2328191
    void prependStringToBuffer(char *buffer, size_t capacity, const char *string)
    {
        if (!buffer || capacity == 0 || !string)
        {
            return;
        }
        size_t string_length = strlen(string);
        size_t buffer_length = strnlen(buffer, capacity);

        // 放不下就整体截断，而不是写出缓冲区。
        // 写完还要保证以 '\0' 结尾 —— 旧实现 memcpy 之后不补结尾，
        // 后续按 C 字符串用它就会读过界。
        if (string_length >= capacity || buffer_length + 1 > capacity - string_length)
        {
            if (capacity >= 1)
            {
                buffer[0] = '\0';
            }
            return;
        }

        memmove(buffer + string_length, buffer, buffer_length + 1);
        memcpy(buffer, string, string_length);
    }

    std::string extractClassNameFromTypename(const char *typeName)
    {
        // std::string(nullptr) 是 UB。调用方里既有类型名，也有可能传进来
        // 一个解析失败的空指针。
        if (typeName == nullptr)
        {
            return {};
        }
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
                    // 泛型参数**之前**根本没有点（比如 "<>c__DisplayClass0"）：
                    // find_last_of 找不到，返回 npos。而 npos + 1 会回绕成 0，
                    // substr(0) 返回整个串，把 "<...>" 一起带进类名。
                    // 旧代码没判这一点。
                    if (dotIndex == std::string::npos)
                    {
                        return nameStr;
                    }
                    classNamespace = nameStr.substr(0, dotIndex);
                }
            }
        }
        return nameStr.substr(dotIndex + 1);
    }

    // 数据目录。**只缓存成功**。
    //
    // 原来的写法是三个地方各来一句 `static auto path = Il2cpp::getDataPath();`
    // —— 第一次调用如果拿不到（游戏还没就绪），"unknown_data_path" 会被
    // **永久缓存**，之后每一次 FileWriter / FileReader 都写到那个
    // 不存在的路径里。用户看到的是：配置存不进去、预设丢了、
    // dump 完找不到文件 —— 而且没有任何错误提示指向真正的原因。
    //
    // getApplicationString 已经改成「失败不缓存、会重试」，这里不能
    // 再用 static 把它冻结掉（同一个坑：上层一次缓存抵消下层修复）。
    static const std::string &DataPath()
    {
        static const std::string kFallback = "unknown_data_path";
        static std::string cached;
        if (cached.empty() || cached == kFallback)
        {
            cached = Il2cpp::getDataPath();
        }
        return cached;
    }

    FileWriter::FileWriter(const std::string &fileName)
    {
        this->fileName = DataPath() + "/" + fileName;
        this->open();
    }

    void FileWriter::init(const std::string &fileName)
    {
        this->fileName = DataPath() + "/" + fileName;
    }

    void FileWriter::open()
    {
        fileStream.open(this->fileName);
        // 静默失败是这个工具里最伤的一种 bug：用户点了 Dump，进度条跑满、
        // 提示成功，然后去游戏目录找不到文件 —— 因为数据目录不可写
        // （权限、路径不存在），ofstream 构造失败却没人告诉任何人。
        // 这里至少明确报错，并在 write() 里阻止继续写空文件。
        if (!fileStream.is_open())
        {
            LOGE("无法写入文件: %s", this->fileName.c_str());
        }
    }

    void FileWriter::write(const char *data)
    {
        if (!fileStream.is_open() || data == nullptr)
        {
            return;
        }
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
        this->fileName = DataPath() + "/" + fileName;
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
        // GetCurrentContext() 在 ImGui 还没初始化完（或正在销毁）时返回
        // nullptr，而这里立刻解引用。CurrentWindow 同理：在 Begin/End
        // 配对之外调用（比如已经在 EndChild 之后）就是空。
        ImGuiContext *ctx = ImGui::GetCurrentContext();
        if (ctx == nullptr)
        {
            return;
        }
        ImGuiContext &g = *ctx;
        ImGuiWindow *window = g.CurrentWindow;
        if (window == nullptr || !window->DC.NavWindowHasScrollY)
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
        // 同上：GImGui 可能为空。
        ImGuiContext *ctx = ImGui::GetCurrentContext();
        if (ctx == nullptr)
        {
            return false;
        }
        ImGuiContext &g = *ctx;
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
            // DeltaTime 为 0（首帧、或一帧被卡到零时长）时 1/x 是 inf，
            // 印出来是 "FPS inf"，而 inf 再进 PlotLines 会把坐标轴的
            // min/max 一起污染成 NaN，整张图可能直接画不出来。
            const float delta = ImGui::GetIO().DeltaTime;
            currentFps = (delta > 0.0f) ? (1.f / delta) : 0.f;
            dt = 0.f;
        }
        ImGui::Text("FPS %.1f", currentFps);
        PlotLines(label, fpsBuffer.data(), static_cast<int>(fpsBuffer.size()), 0, NULL, 0.0f, FLT_MAX, graphSize);
    }

    // 注意：这是**命名空间作用域的全局对象**，构造函数里有 std::vector::resize，
    // 会在库加载时（也就是在游戏进程里）执行。一旦分配失败抛 bad_alloc，
    // 加载期就会直接 terminate —— 用户看到的现象是「游戏一启动就闪退」，
    // 而且和本工具毫无关联，完全无法定位。
    //
    // 改成惰性初始化：只在第一次真正用到时才构造，构造失败也只影响这次
    // 绘制，不会拖垮整个进程。
    class FpsTracker
    {
      public:
        FpsTracker() : bufferSize(100), currentFrame(0)
        {
            fpsBuffer.resize(bufferSize, 0.0f);
        }

        void update(float deltaTime)
        {
            // 同上：deltaTime 为 0 时 1/0 = inf，会被存进缓冲区并污染
            // PlotLines 的纵轴。写 0 表示「这一帧没有有效数据」。
            fpsBuffer[currentFrame] = (deltaTime > 0.0f) ? (1.0f / deltaTime) : 0.0f;
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

    void FpsGraph()
    {
        // 函数内 static：构造推迟到第一次调用，库加载期不做任何分配。
        // 构造异常（比如 bad_alloc）只在这里被 C++ 规则终结 —— 实际上
        // std::vector 分配失败我们也无能为力，但至少不再发生在**加载期**，
        // 那才是「游戏无理由闪退」最难查的时机。
        static FpsTracker fpsTracker;
        fpsTracker.update(ImGui::GetIO().DeltaTime);
        ImGui::FpsGraph_Internal("FPS", fpsTracker.getFpsBuffer());
    }
} // namespace ImGui
