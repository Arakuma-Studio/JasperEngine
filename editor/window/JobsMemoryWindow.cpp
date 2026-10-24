// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "JobsMemoryWindow.h"

#include "io/AssetIOService.h"
#include "jobs/JobSystem.h"
#include "memory/MemoryStats.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    std::string formatBytes(uint64_t bytes) {
        char text[32];
        if (bytes >= 1024ull * 1024 * 1024)
            std::snprintf(text, sizeof(text), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
        else if (bytes >= 1024ull * 1024)
            std::snprintf(text, sizeof(text), "%.2f MB", bytes / (1024.0 * 1024.0));
        else if (bytes >= 1024)
            std::snprintf(text, sizeof(text), "%.1f KB", bytes / 1024.0);
        else
            std::snprintf(text, sizeof(text), "%llu B", (unsigned long long)bytes);
        return text;
    }

    bool containsCaseInsensitive(const std::string& text, const char* pattern) {
        if (!pattern || pattern[0] == '\0')
            return true;
        const std::string needle(pattern);
        auto it = std::search(text.begin(), text.end(), needle.begin(), needle.end(), [](char a, char b) {
            return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
        });
        return it != text.end();
    }

}

void JobsMemoryWindow::setOpen(bool open) {
    if (open && !windowOpen) focusRequested = true;
    windowOpen = open;
}

void JobsMemoryWindow::show() {
    windowFocused = false;
    if (!windowOpen) return;

    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)) {
        ImGui::End();
        return;
    }
    windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    if (ImGui::BeginTabBar("##JobsMemoryTabs")) {
        if (ImGui::BeginTabItem("Jobs")) {
            drawJobs();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Memory")) {
            drawMemory();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

void JobsMemoryWindow::drawJobs() {
    jobs::JobSystem& system = jobs::system();
    const jobs::Stats stats = system.stats(60);

    ImGui::Text("%d worker(s)%s", stats.workers, stats.singleThread ? " (single-thread mode: jobs run from the frame)" : "");
    ImGui::TextDisabled("Executed %llu, stolen %llu, cancelled %llu, failed %llu, asset jobs %u",
                        (unsigned long long)stats.totals.executed, (unsigned long long)stats.totals.stolen,
                        (unsigned long long)stats.totals.cancelled, (unsigned long long)stats.totals.failed, stats.assetJobs);

    if (ImGui::CollapsingHeader("Workers", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (int worker = 0; worker < stats.workers; worker++) {
            const std::vector<float> history = system.workerHistory(worker);
            const float busy = worker < (int)stats.perWorker.size() ? stats.perWorker[(size_t)worker].busyPct : 0.0f;
            char label[48];
            std::snprintf(label, sizeof(label), "je-worker-%d  %5.1f%%", worker, busy);
            ImGui::PlotLines(("##worker" + std::to_string(worker)).c_str(), history.empty() ? &busy : history.data(),
                             history.empty() ? 1 : (int)history.size(), 0, label, 0.0f, 100.0f, ImVec2(-1.0f, 28.0f));
        }
    }

    if (ImGui::CollapsingHeader("Lanes", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("##Lanes", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Lane");
            ImGui::TableSetupColumn("Queued");
            ImGui::TableSetupColumn("Running");
            ImGui::TableHeadersRow();
            for (size_t lane = 0; lane < jobs::kLaneCount; lane++) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(jobs::laneName((jobs::Lane)lane));
                ImGui::TableNextColumn();
                ImGui::Text("%u", stats.lanes[lane].queued);
                ImGui::TableNextColumn();
                ImGui::Text("%u", stats.lanes[lane].running);
            }
            ImGui::EndTable();
        }
        ImGui::Text("Background gate: %d / %d taken, Heavy %d / %d, %d reserved for the frame",
                    stats.gate.taken, stats.gate.max, stats.gate.heavyTaken, stats.gate.heavyMax, stats.gate.reserve);
        ImGui::TextDisabled("Peak over the window: %d taken, Heavy %d", stats.gate.peakTaken, stats.gate.peakHeavyTaken);
    }

    if (ImGui::CollapsingHeader("Channels", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("##Channels", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Channel");
            ImGui::TableSetupColumn("Queued");
            ImGui::TableSetupColumn("Running");
            ImGui::TableSetupColumn("Cap");
            ImGui::TableHeadersRow();
            for (const jobs::ChannelStats& channel : stats.channels) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(channel.name);
                ImGui::TableNextColumn();
                ImGui::Text("%u", channel.queued);
                ImGui::TableNextColumn();
                ImGui::Text("%u", channel.running);
                ImGui::TableNextColumn();
                ImGui::Text("%u", channel.cap);
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("%u blocking thread(s)", stats.blockingThreads);
    }

    if (ImGui::CollapsingHeader("Asset I/O", ImGuiTreeNodeFlags_DefaultOpen)) {
        AssetIOService& io = AssetIOService::instance();
        const AssetIOService::IOStats ioStats = io.stats();
        ImGui::Text("%d reader thread(s)%s. Queued: Critical %u, High %u, Normal %u, Low %u", ioStats.readers,
                    ioStats.inlineMode ? " (reads run in the decode job)" : "",
                    ioStats.queuedByPriority[(size_t)IOPriority::Critical], ioStats.queuedByPriority[(size_t)IOPriority::High],
                    ioStats.queuedByPriority[(size_t)IOPriority::Normal], ioStats.queuedByPriority[(size_t)IOPriority::Low]);
        if (!ioStats.inFlight.empty() && ImGui::BeginTable("##Reads", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                                                           ImVec2(0.0f, 160.0f))) {
            ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Priority");
            ImGui::TableSetupColumn("State");
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            for (const AssetIOService::ReadInfo& read : ioStats.inFlight) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(read.path.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(ioPriorityName(read.priority));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(read.claimed ? "Reading" : "Queued");
                ImGui::TableNextColumn();
                ImGui::PushID((int)read.id);
                if (ImGui::SmallButton("Cancel"))
                    io.cancelRead(read.id);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
}

void JobsMemoryWindow::drawMemory() {
    std::vector<memory::StatsEntry> entries = memory::snapshot();

    uint64_t totals[3] = {0, 0, 0};
    for (const memory::StatsEntry& entry : entries)
        totals[(size_t)entry.residency] += entry.current;
    ImGui::Text("Cpu %s, Gpu %s, Mapped %s", formatBytes(totals[0]).c_str(), formatBytes(totals[1]).c_str(), formatBytes(totals[2]).c_str());

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##MemoryFilter", "Filter by name", filter, sizeof(filter));

    entries.erase(std::remove_if(entries.begin(), entries.end(), [this](const memory::StatsEntry& entry) {
        return !containsCaseInsensitive(entry.name, filter);
    }), entries.end());

    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable
        | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("##MemoryStats", 7, flags))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Kind");
    ImGui::TableSetupColumn("Residency");
    ImGui::TableSetupColumn("Current", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending);
    ImGui::TableSetupColumn("Peak", ImGuiTableColumnFlags_PreferSortDescending);
    ImGui::TableSetupColumn("Capacity", ImGuiTableColumnFlags_PreferSortDescending);
    ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_PreferSortDescending);
    ImGui::TableHeadersRow();

    if (const ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
        if (specs->SpecsCount > 0) {
            const int column = specs->Specs[0].ColumnIndex;
            const bool ascending = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            std::sort(entries.begin(), entries.end(), [column, ascending](const memory::StatsEntry& a, const memory::StatsEntry& b) {
                auto compare = [](auto x, auto y) { return x < y ? -1 : (y < x ? 1 : 0); };
                int order = 0;
                switch (column) {
                    case 0: order = compare(a.name, b.name); break;
                    case 1: order = compare((int)a.kind, (int)b.kind); break;
                    case 2: order = compare((int)a.residency, (int)b.residency); break;
                    case 4: order = compare(a.peak, b.peak); break;
                    case 5: order = compare(a.capacity, b.capacity); break;
                    case 6: order = compare(a.count, b.count); break;
                    default: order = compare(a.current, b.current); break;
                }
                if (order == 0)
                    order = compare(a.name, b.name);
                return ascending ? order < 0 : order > 0;
            });
        }
    }

    for (const memory::StatsEntry& entry : entries) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(entry.name.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(memory::kindName(entry.kind));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(memory::residencyName(entry.residency));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(formatBytes(entry.current).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(formatBytes(entry.peak).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(entry.capacity ? formatBytes(entry.capacity).c_str() : "-");
        ImGui::TableNextColumn();
        ImGui::Text("%llu", (unsigned long long)entry.count);
    }
    ImGui::EndTable();
}
