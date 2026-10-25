// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "AppSettings.h"
#include "profiler/GpuMemory.h"
#include "profiler/Profiler.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class App;

    // Window > Analysis > Profiler (spec S15 R31, Unity's Profiler): a toolbar (Record, Clear,
    // Deep Profile Lua, the frame budget, Load/Save of .jetrace.json, the target, the frame
    // scrub and "pause on frame over X ms"), the module charts (CPU Usage stacked by category,
    // GPU Usage stacked by pass family, Rendering, GPU Memory stacked by category, Monitors) where
    // a click selects that frame in every module, and the selected module's detail pane: the CPU
    // Hierarchy (sortable, searchable, per thread) and Timeline (a lane per thread, zoom, pan,
    // tooltips), the GPU pass table, the render counters, the VRAM table and the monitor list.
    // It edits no project data (no undo); its layout lives in the editor settings.
    class ProfilerWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Profiler";

        enum class Module { Cpu, Gpu, Rendering, Memory, Monitors };

        explicit ProfilerWindow(App* app);

        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }
        bool isFocused() const { return windowFocused; }

        // Record: the profiler records and GPU timing runs
        void setRecording(bool recording);
        bool isRecording() const;
        // Load: a .jetrace.json becomes the target
        bool loadCapture(const std::string& path, std::string& error);
        // the target: 0 the editor's live recording, else a capture id
        void setTarget(uint32_t captureId);
        uint32_t getTarget() const { return target; }
        // the target's frames, and the one the modules show (0: the newest)
        size_t targetFrameCount() const;
        uint64_t getSelectedFrame() const { return selectedFrame; }
        void selectFrame(uint64_t frame);
        void selectModule(Module module) { selected = module; }

    private:
        App* app = nullptr;
        bool windowOpen = false;
        bool windowFocused = false;
        bool focusRequested = false;
        bool gpuHeld = false;
        uint32_t target = 0;
        uint64_t selectedFrame = 0;
        Module selected = Module::Cpu;
        ProfilerSettings settings;
        bool settingsLoaded = false;
        uint32_t cpuThread = 0;
        char search[128] = {};
        std::string selectedPath;       // the Hierarchy row the Timeline picked
        std::set<std::string> collapsed;
        float timelineZoom = 1.0f;
        float timelinePan = 0.0f;
        std::string statusMessage;
        // the GPU Memory table, rebuilt when the tracker's generation moves
        uint64_t memoryGeneration = ~0ull;
        gpumem::Report memoryReport;
        char memoryFilter[128] = {};
        int memorySortColumn = 6;
        bool memorySortAscending = false;

        std::vector<const profiler::Frame*> frames() const;
        const profiler::Frame* currentFrame(const std::vector<const profiler::Frame*>& list) const;
        std::string nameOf(uint32_t nameId) const;
        std::vector<profiler::ThreadInfo> threadList() const;
        std::vector<std::string> monitorNameList() const;

        void drawToolbar(const std::vector<const profiler::Frame*>& list);
        void drawModules(const std::vector<const profiler::Frame*>& list, float height);
        void drawDetail(const std::vector<const profiler::Frame*>& list);
        void drawCpuHierarchy(const profiler::Frame& frame);
        void drawCpuTimeline(const profiler::Frame& frame);
        void drawGpuDetail(const profiler::Frame& frame);
        void drawRenderingDetail(const profiler::Frame& frame);
        void drawMemoryDetail();
        void drawMonitorsDetail(const std::vector<const profiler::Frame*>& list);
        void saveTarget();
        void loadFromDialog();
        void storeSettings();
    };

}
