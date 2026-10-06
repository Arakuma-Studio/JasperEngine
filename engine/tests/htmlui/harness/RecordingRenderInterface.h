// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_TEST_RECORDING_RENDER_INTERFACE_H
#define HTMLUI_TEST_RECORDING_RENDER_INTERFACE_H

#include <RmlUi/Core/RenderInterface.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace jasperengine { namespace test {

    // A CPU-only Rml::RenderInterface for tests without a GPU: it keeps compiled geometry,
    // generated and loaded textures (pixels, sources) and the draw list of a frame.
    // It must outlive every RmlUi context using it and Rml::Shutdown.
    class RecordingRenderInterface : public Rml::RenderInterface {
    public:
        struct Geometry {
            std::vector<Rml::Vertex> vertices;
            std::vector<int> indices;
        };

        struct Texture {
            std::string source;             // LoadTexture path; empty for GenerateTexture
            Rml::Vector2i dimensions{0, 0};
            std::vector<uint8_t> pixels;    // GenerateTexture data (premultiplied RGBA8)
        };

        struct Draw {
            Rml::CompiledGeometryHandle geometry = 0;
            size_t vertexCount = 0;
            size_t indexCount = 0;
            Rml::Vector2f translation{0, 0};
            Rml::TextureHandle texture = 0;
            bool scissorEnabled = false;
            Rml::Rectanglei scissor = Rml::Rectanglei::MakeInvalid();
            bool hasTransform = false;
            Rml::Matrix4f transform = Rml::Matrix4f::Identity();
        };

        ~RecordingRenderInterface() override;

        Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
        void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
        void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
        // dimensions read with stb_image through the engine's file system; 0 if it cannot be read
        Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
        Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override;
        void ReleaseTexture(Rml::TextureHandle texture) override;
        void EnableScissorRegion(bool enable) override;
        void SetScissorRegion(Rml::Rectanglei region) override;
        void SetTransform(const Rml::Matrix4f* transform) override;

        // clears the draw list (call before Context::Render)
        void beginFrame();
        const std::vector<Draw>& getDraws() const;

        size_t getLiveGeometryCount() const;
        size_t getLiveTextureCount() const;
        size_t getCompiledGeometryTotal() const;   // every CompileGeometry call
        const Geometry* getGeometry(Rml::CompiledGeometryHandle handle) const;
        const Texture* getTexture(Rml::TextureHandle handle) const;
        // sources of every LoadTexture call, in order, including failed ones
        const std::vector<std::string>& getLoadRequests() const;

    private:
        std::map<uintptr_t, Geometry> geometries;
        std::map<uintptr_t, Texture> textures;
        std::vector<Draw> draws;
        std::vector<std::string> loadRequests;
        uintptr_t nextHandle = 1;
        size_t compiledTotal = 0;
        bool scissorEnabled = false;
        Rml::Rectanglei scissor = Rml::Rectanglei::MakeInvalid();
        bool hasTransform = false;
        Rml::Matrix4f transform = Rml::Matrix4f::Identity();
    };

} }

#endif //HTMLUI_TEST_RECORDING_RENDER_INTERFACE_H
