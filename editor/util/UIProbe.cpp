// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UIProbe.h"
#include "io/AtomicFile.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <array>

using namespace jasperengine;

namespace {

    using Rect = std::array<float, 4>;

    const std::string& probePath() {
        static const std::string path = [] {
            const char* value = std::getenv("JASPER_UI_PROBE");
            return std::string(value ? value : "");
        }();
        return path;
    }

    std::map<std::string, Rect>& current() {
        static std::map<std::string, Rect> items;
        return items;
    }

    std::map<std::string, Rect>& written() {
        static std::map<std::string, Rect> items;
        return items;
    }

    using Scroll = std::array<float, 2>;

    std::map<std::string, Scroll>& currentScroll() {
        static std::map<std::string, Scroll> scrolls;
        return scrolls;
    }

    std::map<std::string, Scroll>& writtenScroll() {
        static std::map<std::string, Scroll> scrolls;
        return scrolls;
    }

    std::map<std::string, int>& writtenBegins() {
        static std::map<std::string, int> begins;
        return begins;
    }

    // ImGuiWindow::BeginCount holds its last active frame's count until the next first Begin, so
    // it is whole here however late in the frame the window draws
    std::map<std::string, int> multipleBegins() {
        std::map<std::string, int> begins;
        const ImGuiContext& g = *ImGui::GetCurrentContext();
        for (const ImGuiWindow* window : g.Windows) {
            // (a docked panel carries the child flag too, and is what this is for)
            if (((window->Flags & ImGuiWindowFlags_ChildWindow) && !window->DockIsActive) || window->BeginCount < 2) continue;
            if (window->LastFrameActive < g.FrameCount - 1) continue;
            begins[window->Name] = window->BeginCount;
        }
        return begins;
    }

    bool visibleInWindow(const ImVec2& min, const ImVec2& max) {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (!window || window->SkipItems) return false;
        const ImRect clip = window->ClipRect;
        return clip.Contains(ImRect(min, max)) && max.x > min.x && max.y > min.y;
    }

    std::string jsonString(const std::string& text) {
        std::string out = "\"";
        for (char c : text) {
            if (c == '"' || c == '\\') out += '\\';
            if ((unsigned char)c < 0x20) continue;
            out += c;
        }
        return out + "\"";
    }

}

bool editor::UIProbe::enabled() {
    return !probePath().empty();
}

void editor::UIProbe::item(const std::string& name) {
    if (!enabled()) return;
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    if (!visibleInWindow(min, max)) return;
    current()[name] = {min.x, min.y, max.x, max.y};
}

void editor::UIProbe::rect(const std::string& name, float x0, float y0, float x1, float y1) {
    if (!enabled()) return;
    if (!visibleInWindow(ImVec2(x0, y0), ImVec2(x1, y1))) return;
    current()[name] = {x0, y0, x1, y1};
}

void editor::UIProbe::scroll(const std::string& name) {
    if (!enabled()) return;
    const ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window) return;
    currentScroll()[name] = {window->Scroll.y, window->ScrollMax.y};
}

void editor::UIProbe::flush() {
    if (!enabled()) return;
    static unsigned long frame = 0;
    frame++;
    const std::map<std::string, int> begins = multipleBegins();
    if (current() != written() || currentScroll() != writtenScroll() || begins != writtenBegins()) {
        std::ostringstream json;
        json << "{\"frame\": " << frame << ", \"items\": {";
        bool first = true;
        for (const auto& [name, r] : current()) {
            json << (first ? "" : ", ") << jsonString(name) << ": [" << r[0] << ", " << r[1] << ", " << r[2] << ", " << r[3] << "]";
            first = false;
        }
        json << "}, \"scroll\": {";
        first = true;
        for (const auto& [name, s] : currentScroll()) {
            json << (first ? "" : ", ") << jsonString(name) << ": [" << s[0] << ", " << s[1] << "]";
            first = false;
        }
        json << "}, \"begins\": {";
        first = true;
        for (const auto& [name, count] : begins) {
            json << (first ? "" : ", ") << jsonString(name) << ": " << count;
            first = false;
        }
        json << "}}\n";
        // whole files only (a reader never sees half of one): an atomic Cache write
        io::AtomicWriteOptions options;
        options.durability = io::Durability::Cache;
        io::writeFileAtomic(probePath(), json.str(), options);
        written() = current();
        writtenScroll() = currentScroll();
        writtenBegins() = begins;
    }
    current().clear();
    currentScroll().clear();
}
