#include "Logger.h"

#include "imgui/imgui.h"
#include <cstdio>
#include <mutex>
#include <string.h>
#include <vector>

namespace logger
{
    // Buf / LineOffsets 会被多个线程访问：
    //   - 游戏线程的 hook 回调（被 hook 的方法里就能打日志）
    //   - 后台扫描线程
    //   - 渲染线程的 Draw / Clear
    // 而 ImVector 在扩容时会 realloc 并释放旧缓冲 —— 渲染线程正拿着
    // Buf.begin() 遍历时另一个线程 realloc 掉那块内存，就是堆破坏。
    //
    // 所以 AddLog/Clear 与 Draw 全程持锁，Draw 还在锁内把内容拷进
    // 局部快照后再交给 ImGui 渲染：ImGui 的绘制代码会跨多帧持有
    // 那个指针，不可能整段都锁着。
    std::mutex g_logMutex;
    ImGuiTextBuffer Buf;
    ImGuiTextFilter Filter;
    ImVector<int> LineOffsets; // Index to lines offset. We maintain this with AddLog() calls.
    bool AutoScroll = true;    // Keep scrolling if already at the bottom.
    bool WordWrap = true;

    void Clear()
    {
        std::lock_guard<std::mutex> guard(g_logMutex);
        Buf.clear();
        LineOffsets.clear();
        LineOffsets.push_back(0);
    }

    // void AddLog(const char *fmt, ...)
    // {
    //     int old_size = Buf.size();
    //     va_list args;
    //     va_start(args, fmt);
    //     Buf.appendfv(fmt, args);
    //     va_end(args);
    //     for (int new_size = Buf.size(); old_size < new_size; old_size++)
    //         if (Buf[old_size] == '\n')
    //             LineOffsets.push_back(old_size + 1);
    // }

    void AddLog(const char *prefix, const char *fmt, ...)
    {
        // 整段临界区：old_size 的读取、append、扫描 '\n' 填充 LineOffsets
        // 必须原子完成，否则 LineOffsets 会和 Buf 对不上（行号指向错位的
        // 位置，Draw 里会切出乱码甚至越界）。
        std::lock_guard<std::mutex> guard(g_logMutex);

        int old_size = Buf.size();
        // 需要 prefix + fmt + '\n' + '\0' 四部分。
        // 旧代码只分配了 strlen(prefix)+strlen(fmt)+1，sprintf 必然多写 1 字节
        // （'\n' 之后还要写 '\0'），等于每次打日志都堆溢出 1 字节。
        int modifiedFmtLength = strlen(prefix) + strlen(fmt);
        char *modifiedBuf = new char[modifiedFmtLength + 2];

        sprintf(modifiedBuf, "%s%s\n", prefix, fmt);

        va_list args;
        va_start(args, fmt);
        Buf.appendfv(modifiedBuf, args);
        va_end(args);

        delete[] modifiedBuf;
        for (int new_size = Buf.size(); old_size < new_size; old_size++)
            if (Buf[old_size] == '\n')
                LineOffsets.push_back(old_size + 1);
    }

    // NEVER CALL LOG HERE
    void Draw(const char *title, bool *p_open)
    {
        // if (!ImGui::Begin(title, p_open))
        //{
        //     ImGui::End();
        //     return;
        // }

        // Options menu
        if (ImGui::BeginPopup("Options"))
        {
            ImGui::Checkbox("Auto-scroll", &AutoScroll);
            ImGui::Checkbox("Word-Wrap", &WordWrap);
            ImGui::EndPopup();
        }

        // Main window
        if (ImGui::Button("Options"))
            ImGui::OpenPopup("Options");

        ImGui::SameLine();
        bool clear = ImGui::Button("Clear##log");
        ImGui::SameLine();
        ImGui::SameLine();
        // Filter.Draw("Filter", -100.0f);

        ImGui::Separator();

        static float fontScale = 0.5f;
        ImGui::SliderFloat("Scale", &fontScale, 0.1f, 1.f, "%.2f");
        static auto font = ImGui::GetFont();
        static auto origScale = font->Scale;
        font->Scale = fontScale;
        ImGui::PushFont(font);
        if (ImGui::BeginChild("scrolling", ImVec2(0, 0), false, WordWrap ? 0 : ImGuiWindowFlags_HorizontalScrollbar))
        {
            if (clear)
                Clear();

            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

            // 关键：在锁内把内容拷进本地快照，之后再交给 ImGui。
            // 不能直接用 Buf.begin() —— ImGui 的绘制会一直持有那个指针，
            // 而另一个线程随时可能在 Buf 里追加日志触发 realloc，
            // 旧缓冲被释放后 ImGui 还在读它，就是读已释放内存。
            static std::vector<char> bufSnapshot;
            static std::vector<int> offsetsSnapshot;
            {
                std::lock_guard<std::mutex> guard(g_logMutex);
                const char *begin = Buf.begin();
                int total = Buf.size();
                if (total > 0)
                {
                    bufSnapshot.assign(begin, begin + total);
                }
                else
                {
                    bufSnapshot.clear();
                }
                offsetsSnapshot.resize(LineOffsets.Size);
                for (int i = 0; i < LineOffsets.Size; i++)
                {
                    offsetsSnapshot[i] = LineOffsets[i];
                }
            }
            // 以 '\0' 结尾，满足 TextUnformatted / strncmp 的要求
            bufSnapshot.push_back('\0');
            const char *buf = bufSnapshot.data();
            const char *buf_end = buf + bufSnapshot.size() - 1;
            const int offsetCount = (int)offsetsSnapshot.size();

            static auto Text = [](const char *line_start, const char *line_end)
            {
                if (Filter.PassFilter(line_start, line_end))
                {
                    ImVec4 color{1.f, 0.2f, 1.f, 1.0f};
                    if (strncmp(line_start, "[D]", strlen("[D]")) == 0)
                        color = {1.f, 1.f, 0.5, 1.f};
                    else if (strncmp(line_start, "[E]", strlen("[E]")) == 0)
                        color = {1.f, 0.5f, 0.5f, 1.f};
                    else if (strncmp(line_start, "[I]", strlen("[I]")) == 0)
                        color = {0.f, 1.f, 1.f, 1.f};
                    if (WordWrap)
                        ImGui::PushTextWrapPos();
                    ImGui::PushStyleColor(0, color);
                    ImGui::TextUnformatted(line_start + strlen("[D] "), line_end);
                    ImGui::PopStyleColor();
                    if (WordWrap)
                        ImGui::PopTextWrapPos();
                }
            };

            if (Filter.IsActive())
            {
                // In this example we don't use the clipper when Filter is enabled.
                // This is because we don't have random access to the result of our filter.
                // A real application processing logs with ten of thousands of entries may want to store the result of
                // search/filter.. especially if the filtering function is not trivial (e.g. reg-exp).
                for (int line_no = 0; line_no < offsetCount; line_no++)
                {
                    const char *line_start = buf + offsetsSnapshot[line_no];
                    const char *line_end =
                        (line_no + 1 < offsetCount) ? (buf + offsetsSnapshot[line_no + 1] - 1) : buf_end;
                    Text(line_start, line_end);
                }
            }
            else
            {
                // The simplest and easy way to display the entire buffer:
                //   ImGui::TextUnformatted(buf_begin, buf_end);
                // And it'll just work. TextUnformatted() has specialization for large blob of text and will
                // fast-forward to skip non-visible lines. Here we instead demonstrate using the clipper to only process
                // lines that are within the visible area. If you have tens of thousands of items and their processing
                // cost is non-negligible, coarse clipping them on your side is recommended. Using ImGuiListClipper
                // requires
                // - A) random access into your data
                // - B) items all being the  same height,
                // both of which we can handle since we have an array pointing to the beginning of each line of text.
                // When using the filter (in the block of code above) we don't have random access into the data to
                // display anymore, which is why we don't use the clipper. Storing or skimming through the search result
                // would make it possible (and would be recommended if you want to search through tens of thousands of
                // entries).
                ImGuiListClipper clipper;
                clipper.Begin(offsetCount);
                while (clipper.Step())
                {
                    for (int line_no = clipper.DisplayStart; line_no < clipper.DisplayEnd; line_no++)
                    {
                        const char *line_start = buf + offsetsSnapshot[line_no];
                        const char *line_end =
                            (line_no + 1 < offsetCount) ? (buf + offsetsSnapshot[line_no + 1] - 1) : buf_end;
                        Text(line_start, line_end);
                    }
                }
                clipper.End();
            }
            ImGui::PopStyleVar();

            // Keep up at the bottom of the scroll region if we were already at the bottom at the beginning of the
            // frame. Using a scrollbar or mouse-wheel will take away from the bottom edge.
            if (AutoScroll && ImGui::GetScrollY() + 10.f >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
        }
        font->Scale = origScale;
        ImGui::EndChild();
        ImGui::PopFont();
        // ImGui::End();
    }
}; // namespace logger
