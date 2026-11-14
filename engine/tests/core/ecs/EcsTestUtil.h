// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ECS_TEST_UTIL_H
#define ECS_TEST_UTIL_H

#include "Scene.h"

#include <string>
#include <vector>

namespace jasperengine::test {

    // the SubSystems a Scene schedules, in registration order, under the same build options as Scene::init
    inline const std::vector<std::string>& subSystemNames(){
        static const std::vector<std::string> names{
            "ActionSystem", "AnimatorSystem", "PoseAnimatorSystem", "SplineSystem",
#if JASPERENGINE_VOXEL
            "VoxelSystem",
#endif
            "MeshSystem", "FoliageSystem",
#if JASPERENGINE_TERRAIN_WORLD
            "TerrainWorldSystem",
#endif
            "LatticeSystem", "VertexAnimatorSystem", "CameraControlSystem", "UISystem", "UIDocumentSystem", "FluidSystem", "TitleArtSystem", "VideoPlayerSystem", "RenderSystem", "TrailSystem", "WaterInteractionSystem",
            "NavigationSystem", "TilemapSystem", "PhysicsSystem", "ParticleSubsystem", "AudioSystem"
        };
        return names;
    }

    // a scene whose phases run only the tests' system functions (no view is loaded)
    inline void disableSubSystems(Scene& scene){
        for (const std::string& name : subSystemNames()){
            scene.getScheduler().setEnabled(name, false);
        }
    }

    inline Entity makeTransformEntity(EntityRegistry& registry, Entity parent = NULL_ENTITY){
        const Entity entity = registry.createEntity();
        registry.addComponent<Transform>(entity);
        if (parent != NULL_ENTITY){
            registry.addEntityChild(parent, entity, false);
        }
        return entity;
    }

}

#endif // ECS_TEST_UTIL_H
