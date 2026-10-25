// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef GpuMemory_h
#define GpuMemory_h

// The VRAM tracker (spec S15 R6-R9): every sokol image and buffer from creation to destruction,
// with its kind, category, owner, label, size, format and estimated bytes, totals per category
// and per owner, the peak since reset and the driver's own figure where it reports one.
//
// Code that creates GPU resources says what they are for with JE_GPU_ALLOC_SCOPE(category,
// owner) (Profile.h); without a scope the category comes from the resource's usage and the owner
// from its label. Bytes are an estimate (the driver pads, tiles and compresses), labelled so.
//
// Compiled with the profiler only (JASPERENGINE_PROFILER): release games track nothing.

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::gpumem {

    enum class Category : uint8_t {
        Textures,
        RenderTargets,
        ShadowMaps,
        PostFX,
        Meshes,
        Uniforms,       // uniform and streaming buffers
        UI,             // RmlUi / HTML UI
        Particles,
        Terrain,
        Editor,
        Other,
        Count
    };
    constexpr size_t CATEGORY_COUNT = (size_t)Category::Count;

    enum class Kind : uint8_t {
        Texture,
        RenderTarget,
        DepthTarget,
        Buffer
    };

    // "Render Targets", "Meshes (VB/IB)" (the panel's names) and their snake_case keys for MCP
    JASPERENGINE_API const char* categoryName(Category category);
    JASPERENGINE_API const char* categoryKey(Category category);
    JASPERENGINE_API bool categoryFromKey(std::string_view key, Category& out);
    JASPERENGINE_API const char* kindName(Kind kind);

    // the scope a creation happens in (0xFF: none)
    struct ScopeState {
        uint8_t category = 0xFF;
        uint32_t owner = 0;     // internOwner id (0: none)
        bool active() const { return category != 0xFF; }
    };

#if JASPERENGINE_PROFILER

    JASPERENGINE_API uint32_t internOwner(std::string_view owner);
    JASPERENGINE_API std::string ownerName(uint32_t owner);

    // the calling thread's current scope (SokolCmdQueue keeps it with a queued creation)
    JASPERENGINE_API ScopeState currentScope();

    // JE_GPU_ALLOC_SCOPE: sets the calling thread's scope until destroyed (scopes nest)
    class JASPERENGINE_API Scope {
    public:
        Scope(Category category, const char* owner);
        Scope(Category category, std::string_view owner);
        explicit Scope(ScopeState state);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        ScopeState saved;
    };

    // one tracked resource (≤ 128 bytes, R29)
    struct Entry {
        uint32_t id = 0;
        Kind kind = Kind::Texture;
        Category category = Category::Other;
        bool tagged = false;        // made inside a JE_GPU_ALLOC_SCOPE
        uint8_t mips = 1;
        uint8_t samples = 1;
        uint8_t usage = 0;          // USAGE_* bits
        uint16_t slices = 1;        // array layers, cube faces or 3D depth
        int32_t width = 0;
        int32_t height = 0;
        uint32_t owner = 0;
        const char* format = "";
        uint64_t bytes = 0;
        char label[40] = {};
    };
    static constexpr uint8_t USAGE_IMMUTABLE = 1u << 0;
    static constexpr uint8_t USAGE_DYNAMIC = 1u << 1;
    static constexpr uint8_t USAGE_STREAM = 1u << 2;
    static constexpr uint8_t USAGE_ATTACHMENT = 1u << 3;
    static constexpr uint8_t USAGE_STORAGE = 1u << 4;
    static constexpr uint8_t USAGE_VERTEX = 1u << 5;
    static constexpr uint8_t USAGE_INDEX = 1u << 6;

    // the backend's tracker (renders/sokol/SokolVramTracker); a second track of an id replaces it
    JASPERENGINE_API void track(const Entry& entry, bool image);
    JASPERENGINE_API void untrack(uint32_t id, bool image);
    JASPERENGINE_API const char* label(uint32_t id, bool image);
    JASPERENGINE_API void clearAll();
    JASPERENGINE_API void resetPeak();
    // changes with every track/untrack (a panel rebuilds its table only when it moved)
    JASPERENGINE_API uint64_t generation();

    // what the driver says it holds, where it says it (Vulkan VK_EXT_memory_budget, Metal
    // currentAllocatedSize, D3D11 QueryVideoMemoryInfo, GL NVX/ATI meminfo)
    struct DriverMemory {
        bool known = false;
        const char* source = "";
        uint64_t usedBytes = 0;     // 0 with usedKnown false: only the free memory is known
        bool usedKnown = false;
        uint64_t budgetBytes = 0;
        uint64_t availableBytes = 0;
    };
    using DriverQuery = DriverMemory (*)();
    JASPERENGINE_API void setDriverQuery(DriverQuery query);

    struct Allocation {
        uint32_t id = 0;
        bool image = true;
        Kind kind = Kind::Texture;
        Category category = Category::Other;
        bool tagged = false;
        std::string owner;
        std::string label;
        int width = 0;
        int height = 0;
        int slices = 1;
        int mips = 1;
        int samples = 1;
        std::string format;
        uint8_t usage = 0;
        uint64_t bytes = 0;
    };

    struct Group {
        std::string name;
        uint64_t bytes = 0;
        uint32_t count = 0;
    };

    struct Report {
        uint64_t totalBytes = 0;
        uint64_t peakBytes = 0;
        uint32_t count = 0;
        uint64_t untaggedBytes = 0;     // made outside every JE_GPU_ALLOC_SCOPE
        uint64_t categoryBytes[CATEGORY_COUNT] = {};
        uint32_t categoryCount[CATEGORY_COUNT] = {};
        std::vector<Allocation> allocations;    // largest first
        DriverMemory driver;
    };
    JASPERENGINE_API Report report(bool withAllocations = true);
    // the allocations grouped by owner, largest first
    JASPERENGINE_API std::vector<Group> groupByOwner(const Report& report);

#endif

}

#endif /* GpuMemory_h */
