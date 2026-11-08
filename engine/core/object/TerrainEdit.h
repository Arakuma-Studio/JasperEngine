// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_EDIT_H
#define TERRAIN_EDIT_H

// The ECS side of the terrain engine (T1 R3, R14, R42, R91, R93): what engine/core/terrain cannot
// know because it knows no component. Callers query the surface and write maps through here, and
// the seams of later specs meet here (T4 adds its world lookup to querySurface).

#include "Engine.h"
#include "Export.h"
#include "ecs/Entity.h"
#include "math/Matrix4.h"
#include "math/Ray.h"
#include "math/Vector3.h"
#include "terrain/RawMapStore.h"
#include "terrain/TerrainMapStore.h"
#include "terrain/TerrainSurface.h"
#include "util/Signal.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine {

    class Scene;
    class Texture;
    struct TerrainComponent;
    struct Transform;

    // The view over what the component has loaded right now: pointers into its maps, valid until the
    // maps change. size and maxHeight are always filled.
    JASPERENGINE_API terrain::SurfaceView terrainSurfaceView(TerrainComponent& terrain);

    // T3 R52: makes the hole map what its readers need (kept CPU texels, NEAREST, clamped, no mips) and
    // asks for its pixels. No map is Finished, for one test of a string; main thread.
    JASPERENGINE_API ResourceLoadState prepareTerrainHoleMap(TerrainComponent& terrain);

    // Where a terrain is in the world, for code that reads nothing else of its Transform (so that
    // code need not include it: tools/check_fanout.py). valid is false without a Transform.
    struct TerrainWorldPlacement {
        bool valid = false;
        Matrix4 model;
        Vector3 position;
        Vector3 scale = Vector3(1.0f, 1.0f, 1.0f);
        bool rotated = false;
    };
    JASPERENGINE_API TerrainWorldPlacement terrainWorldPlacement(Scene* scene, Entity entity);

    // R14: the one function callers use. Terrain-local; no map loaded gives height 0, up and present
    // false. Read-only: particle collisions call it from workers.
    JASPERENGINE_API terrain::SurfaceSample querySurface(TerrainComponent& terrain, float localX, float localZ);

    // A world ray against the surface, loading the height map if it must. skipHoles: T1 R16
    JASPERENGINE_API bool raycastTerrain(TerrainComponent& terrain, const Transform& transform, const Ray& ray, bool skipHoles, Vector3& worldPoint);

    // R93: the abstract form over one component, for whoever takes a TerrainSurfaceQuery
    class JASPERENGINE_API TerrainComponentQuery final : public terrain::TerrainSurfaceQuery {
    public:
        explicit TerrainComponentQuery(TerrainComponent& terrain): component(&terrain) {}
        terrain::SurfaceSample sample(float localX, float localZ) const override;
        float heightAt(float localX, float localZ) const override;
        bool raycast(const Vector3& origin, const Vector3& direction, float maxT, bool skipHoles, Vector3& point, float& t) const override;

    private:
        TerrainComponent* component;
    };

    // R93, T5 R7: the query's copied-texel form, which a job may read while the map changes on the
    // main thread. view points into this object: keep it alive, and do not copy it.
    struct JASPERENGINE_API TerrainSurfaceSnapshot {
        TerrainSurfaceSnapshot() = default;
        TerrainSurfaceSnapshot(const TerrainSurfaceSnapshot&) = delete;
        TerrainSurfaceSnapshot& operator=(const TerrainSurfaceSnapshot&) = delete;

        std::vector<uint8_t> pixels;
        std::vector<uint8_t> holes;
        terrain::SurfaceView view;
    };
    JASPERENGINE_API void snapshotTerrainSurface(TerrainComponent& terrain, TerrainSurfaceSnapshot& out);

    // R91: the one place a new terrain gets its defaults, called by every route that makes one for a
    // person or an agent. A struct default stays what an old scene loads with, so the settings only
    // new terrains should have go here (T3 R53, T5 F12).
    JASPERENGINE_API void applyNewTerrainDefaults(TerrainComponent& terrain);

    // R13: remap_layers moved the paint; this moves the layers, in the same step. order holds the new
    // index of each layer, as the op did.
    JASPERENGINE_API void applyLayerOrder(TerrainComponent& terrain, std::span<const int> order);

    // R42: every height, hole, blend and density write, whichever route it took, reaches subscribers
    // as (terrain, map, rectangle), once per frame per terrain and map. With no subscriber nothing is
    // kept and nothing allocated. Main thread only.
    class JASPERENGINE_API TerrainEditEvents {
    public:
        using Handler = std::function<void(Entity terrain, terrain::MapId map, terrain::TexelRect rect)>;

        // Every scene's terrains. Entities number per scene, so a subscriber living in one scene
        // takes the overload below.
        [[nodiscard]] static Subscription subscribe(Handler handler);
        [[nodiscard]] static Subscription subscribe(const Scene* scene, Handler handler);

        // The stores call this for what they wrote
        static void publish(const Scene* scene, Entity terrain, terrain::MapId map, terrain::TexelRect rect);
        // The guard MeshSystem::update reads each frame: one load when nothing was edited
        static bool hasPending();
        // Delivers what this scene's edits left since the last flush
        static void flush(const Scene* scene);
        // The scene is going away, with whatever it had not delivered
        static void forgetScene(const Scene* scene);
    };

    // R23, DC10: what a terrain's edits leave for the next MeshSystem::update. Made by the first edit:
    // a terrain never edited has only a null pointer (R53). The rest of T1 keeps its stream images here.
    struct TerrainEditState {
        // R23: during a stroke the surface revision advances at most this often
        static constexpr double REVISION_SECONDS = 0.25;

        // height texels whose nodes wait for new ranges
        terrain::TexelRect refit;
        // a height changed since the last revision
        bool surfaceMoved = false;
        double lastRevisionAt = -REVISION_SECONDS;
        int strokes = 0;
        // R79: the terrain-local rectangle whose heights or holes the colliders have yet to take,
        // before the next physics step (physics/JoltTerrainRefresh)
        bool collisionDirty = false;
        float collisionMinX = 0.0f, collisionMinZ = 0.0f, collisionMaxX = 0.0f, collisionMaxZ = 0.0f;

        // DC9: an image goes up at most once a frame, and a map bigger than the frame's budget at
        // most every ceil(bytes / budget) frames; the last edit always goes
        struct Upload {
            terrain::MapId map;
            uint64_t lastFrame = 0;
            bool waiting = false;
        };
        std::vector<Upload> uploads;
        uint64_t frame = 0;
        // bytes a frame's uploads may take; 0: Engine::defaultTerrainUploadBytes() (R64: the editor
        // sets its own)
        size_t uploadBudget = 0;
        // R78: the maps were moved under private ids before the first runtime write
        bool runtimeDetached = false;
    };

    // DC9: the frames between two uploads of a map of this many bytes
    JASPERENGINE_API uint64_t terrainUploadInterval(size_t mapBytes, size_t budget);

    // R78: before a game's first write, every map of the terrain moves under a private id
    // (__terrain_runtime_<n>_<entity>_<map>), so the authored files and another terrain sharing
    // them never see the change. Once per terrain; false when a map could not be copied.
    JASPERENGINE_API bool detachTerrainMapsForRuntime(TerrainComponent& terrain, Entity entity);

    // The map a store's id names on this component; null when the terrain has no such slot
    JASPERENGINE_API Texture* terrainMapTexture(TerrainComponent& terrain, terrain::MapId map);

    // A map's bytes changed under this rectangle, whoever wrote them: the terrain refits instead of
    // rebuilding (R23), its image follows, and subscribers hear of it (R42). For a terrain sharing
    // the file of the one written (R41), this is all it needs.
    JASPERENGINE_API void noteTerrainMapEdited(const Scene* scene, Entity entity, TerrainComponent& terrain, terrain::MapId map, terrain::TexelRect rect);

    // Between these the surface revision is held to its stroke rate, and it always advances after the
    // last end (R23)
    JASPERENGINE_API void beginTerrainStroke(TerrainComponent& terrain);
    JASPERENGINE_API void endTerrainStroke(TerrainComponent& terrain);

    // R3: the store over a component's maps. Main thread only: a job takes a snapshot (R69). Writes
    // change the map in memory, mark the terrain for the refresh a map change needs, and publish
    // TerrainEditEvents. EditorTerrainStore (editor/terrain/) adds persistence and sharing.
    class JASPERENGINE_API TerrainComponentStore : public terrain::TerrainMapStore {
    public:
        TerrainComponentStore(Scene* scene, Entity entity, TerrainComponent& terrain);

        TerrainComponent& component() { return *terrainPtr; }
        // The component's array moved: this store goes on over its new place (a store a stroke holds
        // across frames)
        void rebind(TerrainComponent& moved) { terrainPtr = &moved; }

        terrain::TerrainFrame frame() const override;
        terrain::MapFormat format(terrain::MapId map) const override;
        bool read(terrain::MapId map, terrain::TexelRect rect, std::span<float> out) const override;
        terrain::WriteResult write(terrain::MapId map, terrain::TexelRect rect, std::span<const float> in) override;
        bool readBytes(terrain::MapId map, terrain::TexelRect rect, std::span<uint8_t> out) const override;
        terrain::WriteResult writeBytes(terrain::MapId map, terrain::TexelRect rect, std::span<const uint8_t> in) override;
        bool canWrite(terrain::MapId map, terrain::TexelRect rect, std::string* why) const override;
        terrain::WriteResult createMap(terrain::MapId map, terrain::MapFormat format, float fill) override;
        bool canCreateMap(terrain::MapId map, std::string* why) const override;
        bool canRemoveMap(terrain::MapId map) const override;
        terrain::WriteResult removeMap(terrain::MapId map) override;
        void takeDirty(std::vector<std::pair<terrain::MapId, terrain::TexelRect>>& out) override;
        Vector2 worldOfTexel(terrain::MapId map, int x, int y) const override;
        Vector2 texelOfWorld(terrain::MapId map, float localX, float localZ) const override;
        terrain::TexelRect preferredBlock(terrain::MapId map) const override;
        std::unique_ptr<terrain::TerrainLargeEdit> beginLarge(terrain::MapId map, terrain::TexelRect rect) override;
        std::unique_ptr<terrain::TerrainStoreUndoHook> takeUndoHook() override;

    private:
        // Points the byte store at the component's current pixels; false when the map has none
        bool sync(terrain::MapId map) const;
        void afterWrite();

    protected:
        Scene* scene;
        Entity entity;
        TerrainComponent* terrainPtr;
        mutable terrain::RawMapStore bytes;
        std::vector<std::pair<terrain::MapId, terrain::TexelRect>> dirty;
    };

}

#endif // TERRAIN_EDIT_H
