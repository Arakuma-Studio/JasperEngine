// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_H
#define TERRAIN_H

#include "Mesh.h"
#include "component/TerrainComponent.h"

namespace jasperengine{

    class JASPERENGINE_API Terrain: public Mesh{

    public:

        // component checkbox (Unity Behaviour.enabled): a disabled Terrain is skipped by its system
        void setEnabled(bool enabled);
        bool isEnabled() const;
        Terrain(Scene* scene);
        Terrain(Scene* scene, Entity entity);
        virtual ~Terrain();

        Terrain(const Terrain& rhs);
        Terrain& operator=(const Terrain& rhs);
        Terrain(Terrain&& rhs) noexcept;
        Terrain& operator=(Terrain&& rhs) noexcept;

        bool createTerrain();

        void setHeightMap(const std::string& path);
        void setHeightMap(Framebuffer* framebuffer);

        void setBlendMap(const std::string& path);
        void setBlendMap(Framebuffer* framebuffer);

        void setBlendMap(unsigned int index, const std::string& path);

        // Only the layer color: a PBR layer keeps the rest of its surface
        void setTextureLayer(unsigned int index, const std::string& path);

        void setTextureDetailRed(const std::string& path);
        void setTextureDetailGreen(const std::string& path);
        void setTextureDetailBlue(const std::string& path);

        void setSurfaceLayer(unsigned int index, const TerrainSurfaceLayer& layer);
        TerrainSurfaceLayer getSurfaceLayer(unsigned int index) const;
        // Copies what a terrain layer can render, and turns the layer into a PBR one
        void setLayerFromMaterial(unsigned int index, const Material& material);
        void removeSurfaceLayer(unsigned int index);
        unsigned int getNumLayers() const;

        void setSize(float size);
        float getSize() const;

        void setMaxHeight(float maxHeight);
        float getMaxHeight() const;

        void setResolution(int resolution);
        int getResolution() const;

        void setTextureBaseTiles(int textureBaseTiles);
        int getTextureBaseTiles() const;

        void setTextureDetailTiles(int textureDetailTiles);
        int getTextureDetailTiles() const;

        void setRootGridSize(int rootGridSize);
        int getRootGridSize() const;

        void setLevels(int levels);
        int getLevels() const;

        // T3 R48: the terrain material's Quality and features
        void setTerrainQuality(TerrainQualitySetting quality);
        TerrainQualitySetting getTerrainQuality() const;
        // the tier it draws at (read only: R2's resolution): "high", "medium" or "low"
        std::string getEffectiveQuality() const;
        void setLimitLayersPerPixel(bool limit);
        bool isLimitLayersPerPixel() const;
        void setOffscreenShadows(TerrainOffscreenShadows shadows);
        TerrainOffscreenShadows getOffscreenShadows() const;
        void setMacroMode(TerrainMacroMode mode);
        TerrainMacroMode getMacroMode() const;
        void setMacroScale(float scale);
        float getMacroScale() const;
        void setMacroStrength(float strength);
        float getMacroStrength() const;
        void setResampleEnabled(bool enabled);
        bool isResampleEnabled() const;
        void setResampleNear(float metres);
        float getResampleNear() const;
        void setResampleFar(float metres);
        float getResampleFar() const;
        void setHeightBlendSharpness(float sharpness);
        float getHeightBlendSharpness() const;
        void setMacroNormal(bool enabled);
        bool isMacroNormal() const;
        // an 8-bit single channel image, 0 = hole, 255 = solid; an empty path takes the map away
        void setHoleMap(const std::string& path);
        std::string getHoleMapPath() const;
    };

}

#endif //TERRAIN_H