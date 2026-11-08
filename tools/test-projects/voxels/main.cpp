// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6's test project (docs/specs/T6-voxel-volumes.md R54, AC27, AC43): a volume of ground drawn in
// WebGL2 on CPU jobs alone. WEB_PROJECT=voxels tools/build.sh web, GATE=voxels tools/build.sh web-gate,
// single-thread and pthreads: the first set activates, a dig lands, a body on the ground stays up until it,
// and the hash of the fixed script prints for AC27 (equal to VoxelDeterminism.GoldenHash on native).

#include "JasperEngine.h"
#include "component/VoxelVolumeComponent.h"

#include "subsystem/VoxelSystem.h"
#include "voxel/VoxelEdit.h"
#include "voxel/VoxelSample.h"

#include <cinttypes>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::voxel;

Scene scene;
Camera sceneCamera(&scene);
Light sun(&scene);
GameObject terrainObject(&scene);
GameObject ballObject(&scene);

namespace {

    std::shared_ptr<VoxelAsset> asset;
    int frame = 0;
    int dugAtFrame = -1;
    bool reportedReady = false;
    bool reportedDig = false;
    bool reportedBall = false;
    int collisionUpdates = 0;

    // The bytes of this script must be the same on every target (R28): the same ops as the unit test
    uint64_t goldenScriptHash(){
        VolumeDesc desc;
        desc.dimensions = Int3{64, 64, 64};
        desc.levels = 3;
        Store store(desc);
        class Base final : public BaseSource {
        public:
            void evaluate(const Int3& origin, int spacing, int16_t* sdf, uint16_t* material) const override{
                for (int z = 0; z < BLOCK_SIZE; z++){
                    for (int y = 0; y < BLOCK_SIZE; y++){
                        const int16_t value = encodeSdf((static_cast<double>(origin.y + y * spacing) - 30.0) * 0.75);
                        for (int x = 0; x < BLOCK_SIZE; x++){
                            sdf[sampleIndex(x, y, z)] = value;
                            material[sampleIndex(x, y, z)] = 0;
                        }
                    }
                }
            }
        };
        store.setBase(std::make_shared<Base>());
        EditOp ops[6];
        ops[0].kind = EditKind::Dig;
        ops[0].shape.center = Vec3d{20.5, 30.25, 20.75};
        ops[0].shape.radius = 7.125;
        ops[1].kind = EditKind::Add;
        ops[1].shape.center = Vec3d{44.5, 33.0, 40.25};
        ops[1].shape.radius = 6.5;
        ops[1].material = 3;
        ops[2].kind = EditKind::Smooth;
        ops[2].shape.center = Vec3d{32.0, 30.0, 32.0};
        ops[2].shape.radius = 9.0;
        ops[2].amount = 0.6;
        ops[2].iterations = 3;
        ops[3].kind = EditKind::Paint;
        ops[3].shape.center = Vec3d{30.0, 30.0, 30.0};
        ops[3].shape.radius = 12.0;
        ops[3].material = 7;
        ops[3].weight = 0.7;
        ops[3].falloff = 0.5;
        ops[4].kind = EditKind::Dig;
        ops[4].shape.kind = ShapeKind::Capsule;
        ops[4].shape.from = Vec3d{10.0, 28.0, 50.0};
        ops[4].shape.to = Vec3d{50.0, 31.0, 12.0};
        ops[4].shape.radius = 2.75;
        ops[4].blend = 1.5;
        ops[5].kind = EditKind::Add;
        ops[5].material = 9;
        ops[5].shape.kind = ShapeKind::Box;
        ops[5].shape.center = Vec3d{50.0, 34.0, 50.0};
        ops[5].shape.halfExtents = Vec3d{3.0, 4.0, 5.0};
        ops[5].shape.rotation = Quatd{0.8660254037844386, 0.0, 0.5, 0.0};
        applyEdit(store, ops);
        return contentHash(store);
    }

    void onFrame(){
        frame++;
        auto system = scene.getSystem<VoxelSystem>();
        const Entity owner = terrainObject.getEntity();
        if (!reportedReady && system->isVolumeReady(owner)){
            reportedReady = true;
            Log::print("[gate] voxels first set active");
        }
        if (reportedReady && dugAtFrame < 0 && frame > 30){
            dugAtFrame = frame;
            EditOp op;
            op.kind = EditKind::Dig;
            op.shape.center = Vec3d{64.0, 32.0, 64.0};
            op.shape.radius = 3.0;
            const EditResult result = system->edit(owner, std::span<const EditOp>(&op, 1), EditOptions{});
            Log::print("[gate] voxels dig ok %d", result.ok ? 1 : 0);
        }
        if (!reportedBall && frame > 20){
            reportedBall = true;
            // R20: the ball was put on the ground before the first step and has not gone through it
            const float y = ballObject.getWorldPosition().y;
            if (y > 31.0f){
                Log::print("[gate] voxels ground collision holds");
            }else{
                Log::error("[gate] voxels the ball fell through: y %.2f", y);
            }
        }
        if (!reportedDig && collisionUpdates > 0){
            reportedDig = true;
            const int frames = frame - dugAtFrame;
            if (frames <= 10){
                Log::print("[gate] voxels dig to collision within budget");
            }else{
                Log::error("[gate] voxels dig to collision took %d frames", frames);
            }
        }
    }

}

JASPERENGINE_INIT void init(){
    Engine::setCanvasSize(640, 360);
    Log::print("[gate] voxels golden %016" PRIx64, goldenScriptHash());

    scene.setBackgroundColor(0.55f, 0.68f, 0.82f, 1.0f);
    sceneCamera.setType(CameraType::CAMERA_PERSPECTIVE);
    sceneCamera.setPosition(64.0f, 60.0f, 110.0f);
    sceneCamera.setTarget(64.0f, 32.0f, 64.0f);
    sceneCamera.activate();
    sun.setType(LightType::DIRECTIONAL);
    sun.setDirection(-0.5f, -1.0f, -0.35f);
    sun.setIntensity(2.0f);

    GeneratorDesc generator;
    generator.kind = "solid_ground";
    generator.height = 32.0;
    VolumeDesc desc;
    desc.dimensions = Int3{128, 64, 128};
    desc.levels = 3;
    asset = VoxelAssets::create("mem://voxels-gate", desc, generator, 1);

    terrainObject.setName("Ground");
    VoxelVolumeComponent volume;
    volume.volumeData.path = "mem://voxels-gate";
    volume.quality = VoxelQuality::Low;
    VoxelMaterialSlot slot;
    slot.name = "ground";
    slot.layer.colorFactor = Vector3(0.45f, 0.40f, 0.30f);
    volume.materialSlots.push_back(slot);
    volume.focus = sceneCamera.getEntity();
    volume.onCollisionUpdated.add("gate", [](AABB){ collisionUpdates++; });
    scene.addComponent<VoxelVolumeComponent>(terrainObject.getEntity(), volume);

    ballObject.setPosition(64.0f, 33.0f, 80.0f);
    Rigidbody ball = ballObject.getRigidbody();
    ball.setType(BodyType::DYNAMIC);
    ball.createSphereCollider(0.5f);

    Engine::onPostUpdate = onFrame;
    Engine::setScene(&scene);
}
