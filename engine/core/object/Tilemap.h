// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TILEMAP_H
#define TILEMAP_H

#include "Mesh.h"
#include "component/TilemapComponent.h"

namespace jasperengine{

    class JASPERENGINE_API Tilemap: public Mesh{

    public:

        // component checkbox (Unity Behaviour.enabled): a disabled Tilemap is skipped by its system
        void setEnabled(bool enabled);
        bool isEnabled() const;
        Tilemap(Scene* scene);
        Tilemap(Scene* scene, Entity entity);
        virtual ~Tilemap();

        Tilemap(const Tilemap& rhs);
        Tilemap& operator=(const Tilemap& rhs);
        Tilemap(Tilemap&& rhs) noexcept;
        Tilemap& operator=(Tilemap&& rhs) noexcept;

        bool createTilemap();

        int findRectByString(const std::string& name);
        int findTileByString(const std::string& name);

        void setTextureScaleFactor(float textureScaleFactor);
        float getTextureScaleFactor() const;

        void addRect(int id, const std::string& name, const std::string& texture, TextureFilter texFilter, Rect rect);
        void addRect(int id, const std::string& name, const std::string& texture, Rect rect);
        void addRect(int id, const std::string& name, Rect rect);
        void addRect(const std::string& name, float x, float y, float width, float height);
        void addRect(float x, float y, float width, float height);
        void addRect(Rect rect);
        void removeRect(int id);
        void removeRect(const std::string& name);
        void clearRects();

        TileRectData& getRect(int id);
        TileRectData& getRect(const std::string& name);

        void addTile(int id, const std::string& name, int rectId, Vector2 position, float width, float height);
        void addTile(const std::string& name, int rectId, Vector2 position, float width, float height);
        void addTile(int rectId, Vector2 position, float width, float height);
        void addTile(const std::string& name, const std::string& rectString, Vector2 position, float width, float height);
        void addTile(const std::string& rectString, Vector2 position, float width, float height);
        void removeTile(int id);
        void removeTile(const std::string& name);
        void clearTiles();

        TileData& getTile(int id);
        TileData& getTile(const std::string& name);

        void setReserveTiles(unsigned int reserveTiles);
        unsigned int getReserveTiles() const;

        unsigned int getWidth();
        unsigned int getHeight();

        void clearAll();

    };

}

#endif //TERRAIN_H