// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MESHSYSTEM_H
#define MESHSYSTEM_H

#include "SubSystem.h"


#include "ecs/ComponentFwd.h"
#include "math/Ray.h"
#include "math/Rect.h"
#include "texture/Material.h"
#include "texture/MaterialPbr.h"
#include "texture/Texture.h"
#include "texture/TexturePack.h"
#include "subsystem/GltfNormalize.h"

#include "jobs/JobSystem.h"

#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "io/watch/FileIdentity.h"
#include "math/Rect.h"
#include "texture/Material.h"
#include "texture/MaterialPbr.h"
#include "texture/Texture.h"
#include "util/OutOfLineValue.h"

namespace tinygltf {
    struct Accessor;
    struct Image;
    struct Material;
    class Model;
    class TinyGLTF;
}

namespace jasperengine{

    struct InstanceData;
    struct Submesh;
    struct TerrainFoliageLayer;

    class JASPERENGINE_API MeshSystem : public SubSystem {

    private:
        struct AsyncModelLoadResult;
        typedef std::unordered_map<std::string, jobs::Task<std::shared_ptr<AsyncModelLoadResult>>> async_model_loads_t;

        static std::mutex& getAsyncModelMutex();
        static async_model_loads_t& getPendingModelLoads();

        // the project Pixels Per Unit the sprites and tilemaps that take it were built with (S41)
        int builtPixelsPerUnit = 1;

        // Files that already failed to load, so createOrUpdateModel stops retrying them.
        std::set<std::string> failedModelLoads;

        // One instanced entity per chunk, so each is culled on its own AABB. Slots are recycled as
        // the ring follows the camera: a chunk that stays in it keeps its mesh and its instances.
        struct TerrainFoliageChunk{
            Entity entity = NULL_ENTITY;
            int chunkX = 0;
            int chunkZ = 0;
            bool assigned = false; //false until the slot holds the resolve of the coordinate above
            bool meshLoaded = false;
        };

        struct TerrainFoliageInstances{
            std::vector<TerrainFoliageChunk> chunks; //grid indexed by wrapped coordinates
            std::string loadedMeshPath;
            bool loadFailed = false;
            float chunkSize = 0; //the size the slot coordinates are in
            // the scaled distances the slots were filled for: a slot past the span keeps its
            // instances and fade, so a new distance (the quality scale too) refills every slot
            float appliedDrawDistance = -1.0f;
            float appliedNearFadeEnd = -1.0f;
            bool needUpdate = true;
            bool pending = false;
        };

        Entity foliagePreviewEntity = NULL_ENTITY;

        // Resolve of each terrain's foliageLayers. Kept here so the authored component stays copiable.
        std::unordered_map<Entity, std::vector<TerrainFoliageInstances>> terrainFoliage;

        static void decodeGLTFImage(tinygltf::Image& image, size_t index, int maxDimension);
        static void decodeGLTFImagesParallel(tinygltf::Model& model, int maxDimension);
        static std::shared_ptr<std::array<TextureData,6>> buildGLTFTextureFaces(tinygltf::Model& model, int textureIndex);

        bool createSprite(SpriteComponent& sprite, MeshComponent& mesh, CameraComponent& camera);
        // the sprite as grid x grid cells while a lattice deforms it (spec lattice §6.4)
        void createSpriteGrid(SpriteComponent& sprite, MeshComponent& mesh, unsigned int texWidth, unsigned int texHeight,
                              Vector2 pivotPos, int grid);
        bool createMeshPolygon(MeshPolygonComponent& polygon, MeshComponent& mesh);
        bool createTilemap(TilemapComponent& tilemap, MeshComponent& mesh);

        void changeFlipY(bool& flipY, CameraComponent& camera, MeshComponent& mesh);
        static Rect normalizeTileRect(Rect tileRect, unsigned int texWidth, unsigned int texHeight);

        // Mesh aux
        std::vector<float> getCylinderSideNormals(float baseRadius, float topRadius, float height, float slices);
        std::vector<float> buildUnitCircleVertices(float slices);

        // Model
        static std::string readFileToString(const char* filename);
        static bool fileExists(const std::string &abs_filename, void *);
        static bool readWholeFile(std::vector<unsigned char> *out, std::string *err, const std::string &filepath, void *);
        static bool getFileSizeInBytes(size_t *filesize_out, std::string *err, const std::string &filepath, void *userdata);
        // GltfNormalize::run on a parse; false with the error in `err` when the model cannot be used
        static bool normalizeGLTF(tinygltf::Model& model, const std::string& filename, std::string* err);
        static std::string getAsyncModelLoadScenePrefix(const Scene* scene);
        static std::string getAsyncModelLoadKey(const Scene* scene, const std::string& filename);
        std::string getAsyncModelLoadKey(const std::string& filename) const;
        static std::shared_ptr<AsyncModelLoadResult> pollOrStartAsyncModelLoad(const Scene* scene, const std::string& filename, bool obj);
        static void cancelAsyncModelLoadByKey(const std::string& key);
        // A finished load of this scene that nobody took for UNCLAIMED_LOAD_FRAMES updates in a
        // row (its GameObject names another file now, or found the parse in ModelPool): dropped,
        // its build closed. A waiting GameObject or editor command takes a result on the frame
        // it is ready, so only abandoned ones get that old.
        void collectUnclaimedAsyncModelLoads();
        static constexpr int UNCLAIMED_LOAD_FRAMES = 60;
        std::unordered_map<std::string, int> unclaimedLoadFrames;
        bool isAsyncModelLoadPending(const std::string& filename) const;
        static bool isAsyncModelLoadPending(const Scene* scene, const std::string& filename);
        static bool isModelPreloading(const std::string& filename);
        // parses the model file from `bytes`, its main file as the asset reader read it
        static std::shared_ptr<AsyncModelLoadResult> loadModelFileOnWorker(const std::string& filename, bool obj, uint64_t buildId, std::vector<uint8_t>& bytes);
        template<typename T>
        static bool isValidGLTFIndex(int index, const std::vector<T>& values) {
            return index >= 0 && static_cast<size_t>(index) < values.size();
        }
        static void applyDefaultObjMaterial(Submesh& submesh);
        std::vector<std::pair<MeshComponent*, unsigned int>> getSubmeshRuns(Entity entity, const ModelComponent& model) const;
        void addSubmeshAttribute(Submesh& submesh, const std::string& bufferName, AttributeType attribute, unsigned int elements, AttributeDataType dataType, size_t size, size_t offset, bool normalized);
        // false as well when the view has no buffer to add
        bool loadGLTFBuffer(int bufferViewIndex, MeshComponent& mesh, ModelComponent& model, const int stride, std::vector<std::string>& loadedBuffers);
        int convertGLTFByteIndicesToShort(const tinygltf::Accessor& indexAccessor, ModelComponent& model);
        int materializeGLTFSparseAccessor(tinygltf::Accessor& accessor, ModelComponent& model);
        int bakeGLTFTransformedAttribute(const tinygltf::Accessor& accessor, const Matrix4& matrix, const Matrix3& normalMatrix, bool isNormal, ModelComponent& model);
        int bakeGLTFTransformedTangent(const tinygltf::Accessor& accessor, const Matrix3& tangentMatrix, ModelComponent& model);
        // The bake helpers need float positions, normals and tangents, and no morph targets
        bool canBakeGLTFMesh(const ModelComponent& model, int meshIndex, std::string* reason = nullptr) const;
        int convertGLTFColorToVec4(const tinygltf::Accessor& accessor, ModelComponent& model);
        bool loadGLTFTexture(int textureIndex, ModelComponent& model, Texture& texture, const std::string& textureName);
        // glTF material extensions (MeshSystemGltfPbr.cpp): the KHR_materials_* lobes into
        // Material::pbr and emissiveStrength, KHR_materials_unlit as the library Unlit shader
        // (vertexColor: the primitive has COLOR_0). False only when a texture cannot be made.
        bool loadGLTFMaterialExtensions(const tinygltf::Material& gltfMaterial, ModelComponent& model, Material& material, bool vertexColor);
        // a lobe's map from its glTF textures: one as it is, two (or one in another channel
        // than the lobe's layout puts it) packed into the layout on a job
        struct GltfLobeMap{
            int first = -1;
            int second = -1;
            int firstTexCoord = 0;
            int secondTexCoord = 0;
            const TexturePack::Layout* layout = nullptr; // null: a lobe with one texture
            const char* name = "";
        };
        bool loadGLTFLobeMap(ModelComponent& model, const GltfLobeMap& lobeMap, Texture& map, int& texCoord);
        std::string getBufferName(int bufferViewIndex, ModelComponent& model);
        Matrix4 getGLTFNodeMatrix(int nodeIndex, ModelComponent& model);
        Matrix4 getGLTFMeshGlobalMatrix(int nodeIndex, ModelComponent& model, std::map<int, int>& nodesParent);
        Matrix4 getGLTFInverseBindMatrix(const ModelComponent& model, int skinIndex, size_t jointIndex);
        bool buildGLTFNodeHierarchy(Entity entity, ModelComponent& model, const std::map<int, int>& nodesParent);
        // Children left without a node stay in the scene, dropped from the mapping
        void remapMeshNodesByName(ModelComponent& model, const std::vector<int>& meshNodes);
        void buildGLTFSkinBindings(Entity entity, ModelComponent& model);
        Entity generateSketetalStructure(Entity entity, ModelComponent& model, int nodeIndex, int skinIndex);
        TextureFilter convertFilter(int filter);
        TextureWrap convertWrap(int wrap);

        // Terrain
        size_t getTerrainGridArraySize(int rootGridSize, int levels);
        void getTerrainHeightRangeArea(TerrainComponent& terrain, float x, float z, float w, float h, float& minHeight, float& maxHeight);
        void createPlaneNodeSubmesh(unsigned int submeshIndex, MeshComponent& mesh, int width, int height, int widthSegments, int heightSegments);
        void resetTerrainToUnbuilt(TerrainComponent& terrain, MeshComponent& mesh);
        bool createTerrain(TerrainComponent& terrain, MeshComponent& mesh);
        void createTerrainNode(TerrainComponent& terrain, float x, float y, float size, int lodDepth);
        void updateTerrainAutoRanges(TerrainComponent& terrain);

        Entity createFoliageEntity(unsigned int capacity);
        void destroyFoliageEntity(TerrainFoliageChunk& chunk);
        void destroyFoliageInstances(TerrainFoliageInstances& instances);
        void destroyTerrainFoliage(Entity entity);
        bool loadFoliageMesh(Entity entity, const std::string& path);
        float sampleFoliageDensity(TerrainComponent& terrain, TerrainFoliageLayer& layer, float localX, float localZ);
        void appendFoliageCell(TerrainComponent& terrain, TerrainFoliageLayer& layer, int cellX, int cellZ, std::vector<InstanceData>& instances);
        void updateFoliageLayer(TerrainComponent& terrain, TerrainFoliageLayer& layer, TerrainFoliageInstances& instances, const Vector3& viewLocal, bool preview);
        void updateTerrainFoliage(Entity entity, TerrainComponent& terrain, Transform& transform);

    public:
        MeshSystem(Scene* scene);
        virtual ~MeshSystem();

        // Frames are authored in pixels, textureRect is in UV space
        static void setSpriteFrameRect(MeshComponent& mesh, SpriteComponent& sprite, Rect frameRect);
        // What a Terrain needs of its height and blend maps, whatever a file said: a save and the
        // generated code hold them too (the editor applies them on load and before a save)
        static void applyTerrainMapSettings(TerrainComponent& terrain);

        // What each generator makes of its mesh before filling the geometry: the submesh
        // settings and the vertex attributes. The editor reads the shader variants of a
        // generated mesh off them, without generating it.
        static void prepareSpriteMesh(MeshComponent& mesh);
        // How many joints the skinned vertices name: their largest bone id plus one (0 without
        // skinned vertices). A palette shorter than that is read past its end
        static size_t skinJointCount(MeshComponent& mesh);
        static void prepareMeshPolygonMesh(MeshComponent& mesh);
        static void prepareTilemapMesh(MeshComponent& mesh);
        // Both terrain submeshes (full and half resolution) draw with this layout
        static void prepareTerrainMesh(MeshComponent& mesh);
        // A texture rect other than the whole texture turns on the texture rect path (update())
        static bool usesTextureRect(const Submesh& submesh);
        // Puts the fields a model load must keep (Submesh::overrideFields) on a loaded submesh
        static void applySubmeshOverride(const Submesh& saved, Submesh& submesh);
        // The edited lobes over the ones a load made: all of them (one override bit), but a map
        // that is still the file's keeps the loaded texture, which knows how to make its pixels
        // (a map packed from two of the file's images, TexturePack)
        static void restorePbrExtensions(const MaterialPbrExtensions& saved, OutOfLineValue<MaterialPbrExtensions>& loaded);
        // true when a frame of another size needs the sprite rebuilt for its texture scale factor
        static bool changesSpriteInset(const SpriteComponent& sprite, const Rect& oldRect, const Rect& newRect);
        // The handedness (tangent w) of the flat 2D meshes (sprite, sprite grid, MeshPolygon,
        // Tilemap: N = +Z, T = +X along u): the bitangent cross(N, T) * w points up the image,
        // where v shrinks, so normal maps read OpenGL style (green up), as Lit. v shrinks toward
        // +y exactly when flipY is on (world cameras): +1 then, -1 without flipY (UI cameras)
        static float flatTangentHandedness(bool flipY);

        bool setFoliagePreviewEntity(Entity entity);
        bool hasPendingFoliageUpdates() const;

        // Foliage ownership, for editor shader collection and selection.
        std::vector<Entity> getFoliageEntities(Entity terrainEntity) const;
        // The instances the chunks of one detail layer (foliageLayers[layer]) hold now (MCP readback)
        size_t getFoliageInstanceCount(Entity terrainEntity, size_t layer) const;
        Entity getFoliageOwner(Entity foliageEntity) const;
        // Reloads the foliage chunks drawn from a model file that changed on disk
        void reloadFoliageMesh(const std::string& filename);
        // Retries the failed loads of a file that appeared or changed on disk
        void retryFailedModelLoads(const std::string& filename);

        void createPlane(MeshComponent& mesh, float width=1, float depth=1, unsigned int tiles=1);
        // createPlane split into segments x segments quads (1..255), for a surface the vertex
        // stage displaces (library:water's WAVES); no vertex colour
        void createGrid(MeshComponent& mesh, float width=1, float depth=1, unsigned int segments=64, unsigned int tiles=1);
        void createWall(MeshComponent& mesh, float width=1, float height=1, unsigned int tiles=1);
        void createBox(MeshComponent& mesh, float width=1, float height=1, float depth=1, unsigned int tiles=1);
        void createSphere(MeshComponent& mesh, float radius=1, unsigned int slices=36, unsigned int stacks=18);
        void createCylinder(MeshComponent& mesh, float baseRadius=1, float topRadius=1, float height=2, unsigned int slices=36, unsigned int stacks=18);
        void createCapsule(MeshComponent& mesh, float baseRadius=1, float topRadius=1, float height=2, unsigned int slices=36, unsigned int stacks=18);
        void createTorus(MeshComponent& mesh, float radius=1, float ringRadius=0.5, unsigned int sides=36, unsigned int rings=16);
        bool canMergeStaticModel(const ModelComponent& model, std::string* reason = nullptr) const;
        bool canEditModelHierarchy(const ModelComponent& model, std::string* reason = nullptr) const;
        // Whether a part or local group can move without breaking the model's animation.
        bool canEditModelPart(const ModelComponent& model, Entity part, std::string* reason = nullptr) const;
        // Where the file puts each node (by glTF index): its parent's entity, or the model on the flat path
        std::map<int, Entity> getModelNodeDefaultParents(Entity modelEntity, const ModelComponent& model) const;
        // The glTF node name, empty when the file has none or the model is not loaded
        static std::string getModelNodeName(const ModelComponent& model, int nodeIdx);
        bool hasCustomMeshParenting(Entity modelEntity, const ModelComponent& model) const;

        // Canonical form of a model path: the same file spelled in different ways maps to one key.
        static std::string getModelFilenameKey(const std::string& filename);
        // The key that starts the ids of the textures a model makes, which scenes store: a file
        // in the assets directory by its path there, never this machine's absolute path
        static std::string getModelTextureKey(const std::string& filename);
        // A model texture id ("<model key>|...") as getModelTextureKey keys it now. Scenes saved
        // before ids were relative carry the model's absolute path: inside assetsRoot it becomes
        // relative; from a project since moved, the longest tail of it that names a file in
        // assetsRoot. Any other id is returned unchanged.
        static std::string migrateModelTextureId(const std::string& id, const std::string& assetsRoot);
        // The file a reader loads for a model (S53 R12, every parse site asks here): its cooked
        // .glb when there is one (CookedModels: the export's index, the editor's cook cache),
        // else an FBX's imported .glb, else the file itself. A cooked .glb keeps the source's
        // image URIs: they resolve against the source's directory.
        static std::string getImportedModelPath(const std::string& filename);
        // The .glb an FBX is imported into: in the editor's import cache (System::getImportPath),
        // next to the FBX in an exported game (<name>.fbx.glb)
        static std::string getFbxImportPath(const std::string& filename);
        // Parses a model file through getImportedModelPath (a cooked .glb or an FBX's import is
        // read instead, with texture and buffer URIs still relative to the source): every glTF
        // parse site goes through it
        static bool readGLTFFile(tinygltf::TinyGLTF& loader, tinygltf::Model* model, std::string* err, std::string* warn, const std::string& filename);
        // readGLTFFile, then normalizeGLTF: the model as core glTF
        static bool loadGLTFFile(tinygltf::TinyGLTF& loader, tinygltf::Model* model, std::string* err, std::string* warn, const std::string& filename);

        // Submesh edits (Submesh::overrideFields) carried across a load, keyed by the ordinal of
        // the primitive that built each one. The loaders do this themselves; it is public for the
        // editor, which destroys the generated mesh entities before reloading.
        typedef std::map<unsigned int, Submesh> SubmeshOverrides;
        SubmeshOverrides collectSubmeshOverrides(Entity entity, const ModelComponent& model) const;
        void applySubmeshOverrides(const SubmeshOverrides& overrides, Entity entity, const ModelComponent& model) const;

        // bakeSceneRoot: the entity does not carry the file's scene root transform (a foliage
        // chunk), so a lone mesh node bakes its whole global transform into the vertices
        bool loadGLTF(Entity entity, const std::string filename, bool asyncLoad=false, bool skipEntities=false, bool changeRootTransform=true, bool bakeSceneRoot=false);
        bool loadOBJ(Entity entity, const std::string filename, bool asyncLoad=false);

        // Parses a model file into ModelPool without a scene, for SceneManager::preloadScene().
        // False while it loads; data then holds the model, or stays empty when it failed.
        static bool preloadModel(const std::string& filename, std::shared_ptr<void>& data);
        static void cancelPreloadModel(const std::string& filename);
        // Drops a preloaded model and its images from the pools, unless a scene uses them
        static void releasePreloadedModel(const std::string& filename);
        // Drops the cached parse and images of a file that changed on disk, even while in use
        // (and the file's sub-clips in AnimationClipPool), for a reload of the entities using it
        // that follows (their images are gone from the pools)
        static void invalidateModelFile(const std::string& filename);
        // Drops only the cached parse and the file's sub-clips: they are built again from the file
        // (animation clips spec 9.R13) while the entities that use it, not reloaded, keep showing
        // their images. A later invalidateModelFile still drops those images.
        static void invalidateModelParse(const std::string& filename);

        // the name of the buffers a load appends to a pooled parse (converted, baked copies): not
        // the file's, so they stay when the file's bytes are released (ModelCpuData)
        static constexpr const char* kSyntheticBufferName = "JasperEngineSynthetic";

        // Loads of a pooled tinygltf::Model take turns under this mutex: loadGLTF appends
        // synthetic buffers to it, so a reader of the shared model (the sub-clip builder) holds it too
        static std::recursive_mutex& getPooledGLTFMutex();
        // Parses a glTF/GLB/FBX (its import) into `model` without decoding images and without
        // ModelPool, on any thread
        static bool parseGLTFFile(const std::string& filename, tinygltf::Model& model, std::string* err);

        // Animation clips spec 9.R3: a model on or under an entity with an Animation Player loads in
        // the node-hierarchy layout (one entity per glTF node: the paths sub-clips address)
        bool isNodeHierarchyForced(Entity entity) const;
        // The model is forced into the node-hierarchy layout but loaded in another one: reloading
        // its file (the editor's ModelLoadCmd) rebuilds it
        bool needsNodeHierarchyReload(Entity entity) const;
        // A loaded model in the legacy layout (no node hierarchy) that the node-hierarchy layout
        // would change: not an OBJ, not a merged static model (those animate only at the root).
        // needsNodeHierarchyReload without the player condition; the editor's one rule for it.
        static bool isLegacyLayout(const ModelComponent& model);
        // A loaded model in the legacy layout whose entity stands in for the glTF scene root (a
        // lone mesh, or a skin without the node hierarchy): true with that node's global matrix.
        // The node hierarchy gives the root node an entity of its own, so a switch takes it off
        // the model entity (local * inverse(sceneRoot)) to keep everything where it was.
        bool getLegacyRootTransform(Entity entity, Matrix4& sceneRoot);

        // Caps the resolution glTF images are decoded to on the CALLING thread (0 = full resolution).
        // Used by the thumbnail worker so previews don't decode/upload full-size 4K maps.
        static void setImageDecodeMaxDimension(int dimension);
        // What a load on the calling thread adds to the ids of a model's embedded images: ""
        // at full resolution, "|max<N>" for a load capped by setImageDecodeMaxDimension (a
        // thumbnail's preview). The preview's pixels are another image than the scene's: under
        // the scene's ids it shared (and freed) the scene's pooled pixels and took over the
        // generator that makes them again, bound to the preview's model (BF15).
        static std::string getImageKeySuffix();

        // tests: JASPER_MESH_LOAD_HOLD_FILE=<path> holds the first load (RenderSystem::loadMesh)
        // of the meshes of a scene that loaded a model in the background, while that file exists:
        // what else loads the same model meanwhile (a Project window thumbnail) comes first
        static bool isMeshLoadHeld(const Scene* scene);

        void createInstancedMesh(Entity entity);
        // an EXT_mesh_gpu_instancing node's copies as the entity's instances (S53 R3)
        void applyGLTFInstances(Entity entity, const std::vector<GltfInstance>& instances);
        void removeInstancedMesh(Entity entity);
        bool hasInstancedMesh(Entity entity) const;

        void clearBoneMapping(ModelComponent& model);
        void clearAnimationMapping(ModelComponent& model);
        void clearMeshNodeMapping(ModelComponent& model);
        void destroyModel(ModelComponent& model);

        void resetModelToBindPose(Entity entity, ModelComponent& model);

        bool raycastTerrainSurface(const Ray& ray, TerrainComponent& terrain, Transform& transform, Vector3& worldPoint);
        // Height and normal in terrain-local space, the surface the foliage scatter uses
        // (static and read-only: particle collisions call it from the job system's workers)
        static void sampleTerrainSurface(TerrainComponent& terrain, float localX, float localZ, float& height, Vector3& normal);

        // Heightmap lookups as the terrain vertex shader does them (so collider, ray casts,
        // height queries and brushes meet the rendered surface): linear filtering with texel
        // centres at (i + 0.5) / size, clamped to the edge texels.
        // Texel coordinate of a terrain-local position along one axis of a size-texel map
        static float terrainTexelCoord(float local, float terrainSize, int size);
        // Normalized [0,1] height of the R channel (8 or 16-bit) at a texel coordinate
        static float sampleTerrainHeightTexels(const unsigned char* pixels, int width, int height, int channels, int bytesPerChannel, float texelX, float texelY);
        // Normalized [0,1] height at a terrain-local position; 0 without pixels
        static float sampleTerrainHeight(const unsigned char* pixels, int width, int height, int channels, int bytesPerChannel, float terrainSize, float localX, float localZ);

        // The part entities of a model whose geometry is on child entities (one per glTF mesh
        // node, ModelComponent::meshNodesMapping) that exist and have a mesh, in node-index
        // order; empty for any other entity (a model on its own mesh too)
        std::vector<Entity> getModelParts(Entity model) const;

        bool hasPendingAsyncModelLoads() const;
        // Test hook: each asynchronous parse of a model file waits this long first, so a load is still
        // running when a test acts (0, the default: none)
        static void setModelLoadDelayForTest(int milliseconds);
        void cancelAsyncModelLoads();
        static void cancelAllAsyncModelLoads();
        bool isAsyncModelLoadPending(Entity entity, const std::string& filename) const;
        void cancelAsyncModelLoad(Entity entity, const std::string& filename);

        bool createOrUpdateSprite(SpriteComponent& sprite, MeshComponent& mesh);
        bool createOrUpdateTerrain(TerrainComponent& terrain, MeshComponent& mesh, Transform& transform);
        bool createOrUpdateMeshPolygon(MeshPolygonComponent& polygon, MeshComponent& mesh);
        bool createOrUpdateTilemap(TilemapComponent& tilemap, MeshComponent& mesh);
        bool createOrUpdateModel(Entity entity, ModelComponent& model, MeshComponent& mesh);

        // Hot reload of a model file (S05 R11). parseModelForReload reads and parses it again
        // on any thread (throws with the parser's error); on the main thread,
        // modelHierarchyMatches says whether its nodes (names and parents) are the loaded
        // ones, and adoptModelReload puts the parse and its textures in the pools. Then every
        // MeshSystem's reloadModelFile rebuilds the meshes, skins, morphs and animations of
        // the ModelComponents using the file on their existing GameObjects (transforms, added
        // components and submesh overrides are kept).
        struct ModelReload;
        static std::shared_ptr<ModelReload> parseModelForReload(const std::string& filename);
        static bool modelHierarchyMatches(const ModelReload& reload, std::string& why);
        static void adoptModelReload(const std::shared_ptr<ModelReload>& reload);
        // The components of `filename` load again (true: there was one)
        bool reloadModelFile(const std::string& filename);
        // The same, after the GameObjects the load generated are destroyed: a hierarchy that
        // changed is created again from the file (what was edited on those GameObjects goes)
        bool rebuildModelFile(const std::string& filename);

        void calculateMeshAABB(MeshComponent& mesh);

        void load() override;
        void draw() override;
        void destroy() override;
        void update(double dt) override;

        void onComponentAdded(Entity entity, ComponentId componentId) override;
        void onComponentRemoved(Entity entity, ComponentId componentId) override;
    };

}

#endif //MESHSYSTEM_H
