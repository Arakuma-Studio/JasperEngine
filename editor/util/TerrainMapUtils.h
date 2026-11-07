// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Project.h"
#include "terrain/TerrainMapStore.h"

#include <algorithm>
#include <string>
#include <vector>

namespace jasperengine{
    struct TerrainComponent;
}

namespace jasperengine::editor{

    enum class TerrainMapTarget{
        HeightMap,
        BlendMap,
        DensityMap,
        HoleMap
    };

    // Names one editable map. "layer" picks the blend map or the foliage layer.
    struct TerrainMapRef{
        TerrainMapTarget target = TerrainMapTarget::HeightMap;
        int layer = 0;

        TerrainMapRef() = default;
        TerrainMapRef(TerrainMapTarget target, int layer = 0): target(target), layer(layer){}

        bool operator==(const TerrainMapRef& other) const{
            return target == other.target && layer == other.layer;
        }
    };

    // Inclusive texel bounds of what a stroke wrote.
    struct TerrainMapRegion{
        int minX = 0;
        int minY = 0;
        int maxX = -1;
        int maxY = -1;

        bool empty() const{
            return maxX < minX || maxY < minY;
        }

        int width() const{
            return empty() ? 0 : maxX - minX + 1;
        }

        int height() const{
            return empty() ? 0 : maxY - minY + 1;
        }

        bool fitsIn(int mapWidth, int mapHeight) const{
            return !empty() && minX >= 0 && minY >= 0 && maxX < mapWidth && maxY < mapHeight;
        }

        void merge(int x0, int y0, int x1, int y1){
            if (empty()){
                minX = x0;
                minY = y0;
                maxX = x1;
                maxY = y1;
                return;
            }
            minX = std::min(minX, x0);
            minY = std::min(minY, y0);
            maxX = std::max(maxX, x1);
            maxY = std::max(maxY, y1);
        }
    };

    // A whole map as a structural undo step keeps it (TerrainTextureEditCmd, T1 R28)
    struct TerrainMapSnapshot{
        bool empty = true;
        std::string path;
        std::string id;
        TextureFilter minFilter = TextureFilter::LINEAR;
        TextureFilter magFilter = TextureFilter::LINEAR;
        TextureWrap wrapU = TextureWrap::REPEAT;
        TextureWrap wrapV = TextureWrap::REPEAT;
        ColorFormat colorFormat = ColorFormat::RGBA;
        int width = 0;
        int height = 0;
        int channels = 0;
        std::vector<unsigned char> pixels;
    };

    // Terrain map storage shared by the terrain editor and its undo commands: both bind, read,
    // persist and refresh the same height, blend and foliage density maps.
    class TerrainMapUtils{

    public:
        // Null when a density map is asked for a foliage layer the terrain no longer has.
        static Texture* findTexture(TerrainComponent& terrain, const TerrainMapRef& ref);
        static std::string getPropertyName(const TerrainMapRef& ref);
        // The list a map lives in, empty when it is not in one. An indexed name alone does
        // not carry a newly created slot to a prefab instance.
        static std::string getContainerPropertyName(const TerrainMapRef& ref);
        static bool hasLoadedData(Texture& texture);
        static bool writeFile(Project* project, const std::string& relativePath, int width, int height, int channels, int bytesPerChannel, const std::vector<unsigned char>& pixels);
        static void refresh(SceneProject* sceneProject, Entity entity, const TerrainMapRef& ref);
        // The map's file is an asset (Unity's TerrainData): every other loaded terrain bound
        // to the same file takes the new content, a prefab's other instances included
        static void refreshSharing(Project* project, uint32_t sceneId, Entity entity, const TerrainMapRef& ref);

        static terrain::MapId mapOfRef(const TerrainMapRef& ref);
        static TerrainMapRef refOfMap(terrain::MapId map);

        // ---- the files the engine writes are named terrain_edit_* under terrain_maps/ (T1 R32)

        static std::string makeEditableTextureId(uint32_t sceneId, Entity entity, const TerrainMapRef& ref);
        static std::string makeEditableTexturePath(Project* project, uint32_t sceneId, Entity entity, const TerrainMapRef& ref);
        static bool isEditableTexturePath(const std::string& path);
        static bool isOwnedEditableTexturePath(const std::string& path, uint32_t sceneId, Entity entity, const TerrainMapRef& ref);

        static bool isScalarTarget(TerrainMapTarget target);
        static int expectedChannels(TerrainMapTarget target);
        static ColorFormat expectedFormat(TerrainMapTarget target);
        static void encodeHeightTexel(unsigned char* pixels, size_t texelIndex, int bytesPerChannel, float value);

        // A disk read of a map: a pending edit reaches the file first (T1 R30)
        static bool loadTextureDataFromPath(Project* project, const std::string& path, TextureData& data);
        // The texture bound to the file, with the pixels in the pool: the running editor never
        // depends on the file being current
        static bool setFileBackedTextureData(Project* project, Texture& texture, const std::string& relativePath, int width, int height, ColorFormat format, int channels, const std::vector<unsigned char>& pixels);
        static void setOwnedTextureData(Texture& texture, const std::string& id, int width, int height, ColorFormat format, int channels, const std::vector<unsigned char>& pixels);
        static std::vector<unsigned char> copyTexturePixels(TextureData& data);
        static std::vector<unsigned char> convertTexturePixels(TextureData& data, TerrainMapTarget target);

        static TerrainMapSnapshot captureSnapshot(Project* project, Texture& texture, bool forcePixels);
        static bool snapshotsEqual(const TerrainMapSnapshot& a, const TerrainMapSnapshot& b);
        static void applySnapshotToTexture(Project* project, Texture& texture, const TerrainMapSnapshot& snapshot);

        // Heightmaps optionally start at the middle, so they can be raised or lowered
        static std::vector<unsigned char> makeInitialMapPixels(TerrainMapTarget target, int width, int height, bool heightMapStartAtMiddle);
        // Blend maps past the first only exist once a layer on them is painted
        static void ensureMapSlot(TerrainComponent& terrain, const TerrainMapRef& ref);
        // Whether ensureEditableMap would move this map to a file of the terrain's own, or convert it
        static bool needsEditableCopy(Texture& texture, uint32_t sceneId, Entity entity, const TerrainMapRef& ref);
        // The map in the format an edit writes, in a file this terrain owns. A generated or authored
        // file is copied on the first edit (R32), and a missing map is made.
        static bool ensureEditableMap(Project* project, SceneProject* sceneProject, Entity entity, const TerrainMapRef& ref, int resolution, bool heightMapStartAtMiddle);
    };

}
