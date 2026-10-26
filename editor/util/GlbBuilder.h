// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "ufbx.h"
#include "tiny_gltf.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    // Converts a loaded FBX scene into a binary glTF
    class GlbBuilder {
    public:
        // A texture file as the FBX names it
        struct TextureRef {
            std::string filename;
            std::string relative;
            std::string absolute;
        };
        // Where a texture was found, empty when nowhere
        struct TextureLookup {
            TextureRef texture;
            std::filesystem::path found;
        };

        GlbBuilder(const ufbx_scene* scene, const std::filesystem::path& fbxPath, const std::filesystem::path& assetsRoot);

        bool build(std::string& error);
        // The .glb contents, with extras in its asset info; empty on failure
        std::vector<unsigned char> write(const tinygltf::Value& extras);

        // Texture files the .glb was made from. Linked ones are referenced by a URI relative to the FBX.
        const std::set<std::filesystem::path>& getDependencies() const;
        bool isLinked(const std::filesystem::path& path) const;
        // Every texture file the FBX names and where it was found
        std::vector<TextureLookup> getTextureLookups() const;
        const std::vector<std::string>& getWarnings() const;

        // Prefers a copy inside the assets, then one found by name near the FBX; empty when there is none
        static std::filesystem::path findTextureFile(const TextureRef& texture, const std::filesystem::path& fbxPath,
                                                     const std::filesystem::path& assetsRoot);

    private:
        struct Pixels;

        // Vertex data of a ufbx mesh, shared by every glTF mesh made from it
        struct MeshGeometry {
            std::string name;
            std::vector<tinygltf::Primitive> primitives;
            std::vector<uint32_t> slots; // material slot of each primitive
            std::vector<const ufbx_blend_channel*> channels;
            int skin = -1;
        };

        const ufbx_scene* scene;
        std::filesystem::path fbxPath;
        std::filesystem::path assetsRoot;
        std::vector<std::string> warnings;
        std::set<std::string> warned;

        tinygltf::Model model;
        std::set<std::filesystem::path> dependencies;
        std::set<std::filesystem::path> linkedFiles;
        std::map<uint32_t, TextureLookup> textureLookups;

        std::map<uint32_t, MeshGeometry> geometries;
        std::map<std::pair<uint32_t, std::vector<const ufbx_material*>>, int> meshIndices;
        std::map<std::pair<const ufbx_material*, std::vector<size_t>>, int> materialIndices;
        std::map<uint32_t, int> fileImages;
        std::map<std::string, std::pair<int, std::shared_ptr<Pixels>>> maskedImages;
        std::map<std::string, int> packedImages;
        std::map<std::pair<int, int>, int> textureIndices;
        std::map<std::pair<int, int>, int> samplerIndices;
        std::map<uint32_t, std::shared_ptr<Pixels>> pixelCache;

        void warn(const std::string& message);

        int addBufferView(const void* data, size_t size, int target);
        int addAccessor(int view, int componentType, size_t count, int type);
        int addFloatAccessor(const std::vector<float>& values, int type, int target, bool bounds);

        bool resolveTexture(uint32_t fileIndex, std::vector<unsigned char>& bytes, std::filesystem::path& path);
        std::shared_ptr<Pixels> loadPixels(uint32_t fileIndex);
        int addEmbeddedImage(const std::vector<unsigned char>& bytes, const std::string& mimeType, const std::string& name);
        int addPngImage(const Pixels& pixels, const std::string& name);
        int imageForFile(uint32_t fileIndex);
        int textureFor(int image, const ufbx_texture* texture);
        int combineOpacity(const ufbx_texture* base, const ufbx_texture* opacity, std::shared_ptr<Pixels>& pixels);
        int packMetallicRoughness(const ufbx_texture* roughness, const ufbx_texture* metalness, bool glossiness);
        int texCoordFor(const ufbx_texture* texture, const ufbx_mesh* mesh);
        int materialFor(const ufbx_material* material, const ufbx_mesh* mesh);

        void buildGeometry(const ufbx_mesh* mesh);
        int meshFor(const ufbx_node* node);
        void buildNodes();
        void buildAnimations();

        static std::shared_ptr<Pixels> resized(const std::shared_ptr<Pixels>& pixels, int width, int height);
        static const char* detectAlphaMode(const Pixels& pixels, bool emptyAlphaIsJunk);
    };

}
