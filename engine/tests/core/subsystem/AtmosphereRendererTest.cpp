// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The volumetric fog's choice of point and spot lights (docs/specs/S101-vapor.md R13, AC6): lit ones
// whose bounding sphere is in the view frustum and within the volumetric distance, the nearest
// first, capped by the tier, with the reason of every light left out.

#include "subsystem/AtmosphereRenderer.h"
#include "component/CameraComponent.h"
#include "math/Plane.h"

#include <gtest/gtest.h>

#include <vector>

using namespace jasperengine;

namespace {

    // a camera at the origin looking down -Z, 90 degrees wide and high, 0.1 to 100 m (inward normals)
    void cameraPlanes(Plane planes[6]){
        const float s = 0.70710678f;
        planes[FRUSTUM_PLANE_NEAR] = Plane(0.0f, 0.0f, -1.0f, -0.1f);
        planes[FRUSTUM_PLANE_FAR] = Plane(0.0f, 0.0f, 1.0f, 100.0f);
        planes[FRUSTUM_PLANE_LEFT] = Plane(s, 0.0f, -s, 0.0f);
        planes[FRUSTUM_PLANE_RIGHT] = Plane(-s, 0.0f, -s, 0.0f);
        planes[FRUSTUM_PLANE_BOTTOM] = Plane(0.0f, s, -s, 0.0f);
        planes[FRUSTUM_PLANE_TOP] = Plane(0.0f, -s, -s, 0.0f);
    }

    FogLightCandidate light(Entity entity, const Vector3& position, float range){
        FogLightCandidate candidate;
        candidate.entity = entity;
        candidate.position = position;
        candidate.range = range;
        candidate.intensity = 1.0f;
        candidate.volumetricIntensity = 1.0f;
        return candidate;
    }

    const AtmosphereFogLight* recordOf(const std::vector<AtmosphereFogLight>& considered, Entity entity){
        for (const AtmosphereFogLight& record : considered){
            if (record.entity == entity)
                return &record;
        }
        return nullptr;
    }

}

TEST(AtmosphereRendererTest, FogLightFrustumCull){
    Plane planes[6];
    cameraPlanes(planes);
    std::vector<FogLightCandidate> lights;
    // in view: ahead at 20 m (entity 1), and to the right at 10 m (2)
    lights.push_back(light(1, Vector3(0.0f, 0.0f, -20.0f), 5.0f));
    lights.push_back(light(2, Vector3(6.0f, 0.0f, -10.0f), 2.0f));
    // behind the camera with a small reach: culled although it is the nearest (3)
    lights.push_back(light(3, Vector3(0.0f, 0.0f, 4.0f), 2.0f));
    // behind the camera but reaching into the view: kept (4)
    lights.push_back(light(4, Vector3(0.0f, 0.0f, 3.0f), 6.0f));
    // far to the left, outside the side plane by more than its range (5)
    lights.push_back(light(5, Vector3(-40.0f, 0.0f, -20.0f), 3.0f));
    // unlimited range behind the camera: it reaches the whole view (6)
    lights.push_back(light(6, Vector3(0.0f, 0.0f, 10.0f), 0.0f));
    // beyond the volumetric distance (7)
    lights.push_back(light(7, Vector3(0.0f, 0.0f, -95.0f), 5.0f));
    // dark ones: no intensity (8), no volumetric intensity (9)
    lights.push_back(light(8, Vector3(0.0f, 0.0f, -8.0f), 5.0f));
    lights.back().intensity = 0.0f;
    lights.push_back(light(9, Vector3(0.0f, 0.0f, -8.0f), 5.0f));
    lights.back().volumetricIntensity = 0.0f;

    std::vector<int> selected;
    std::vector<AtmosphereFogLight> considered;
    AtmosphereRenderer::selectFogLights(lights, planes, 100.0f, Vector3(0.0f, 0.0f, 0.0f), 60.0f, 4, selected, considered);

    ASSERT_EQ(considered.size(), lights.size());
    for (size_t i = 0; i < lights.size(); i++)
        EXPECT_EQ(considered[i].entity, lights[i].entity);
    // the nearest first by the distance to their reach: 4 (0 m), 2 (~9.7 m), 6 (10 m), 1 (15 m)
    ASSERT_EQ(selected.size(), 4u);
    EXPECT_EQ(lights[selected[0]].entity, 4u);
    EXPECT_EQ(lights[selected[1]].entity, 2u);
    EXPECT_EQ(lights[selected[2]].entity, 6u);
    EXPECT_EQ(lights[selected[3]].entity, 1u);
    for (Entity entity : {Entity(1), Entity(2), Entity(4), Entity(6)}){
        const AtmosphereFogLight* record = recordOf(considered, entity);
        ASSERT_NE(record, nullptr);
        EXPECT_TRUE(record->scattered) << entity;
        EXPECT_TRUE(record->reason.empty()) << entity;
        EXPECT_FALSE(record->shadowed) << entity;
    }
    EXPECT_EQ(recordOf(considered, 3)->reason, "frustum");
    EXPECT_EQ(recordOf(considered, 5)->reason, "frustum");
    EXPECT_EQ(recordOf(considered, 7)->reason, "distance");
    EXPECT_EQ(recordOf(considered, 8)->reason, "intensity");
    EXPECT_EQ(recordOf(considered, 9)->reason, "volumetric_intensity");
    for (Entity entity : {Entity(3), Entity(5), Entity(7), Entity(8), Entity(9)})
        EXPECT_FALSE(recordOf(considered, entity)->scattered) << entity;

    // the Low tier's one light: the nearest; the others in view are over the cap
    AtmosphereRenderer::selectFogLights(lights, planes, 100.0f, Vector3(0.0f, 0.0f, 0.0f), 60.0f, 1, selected, considered);
    ASSERT_EQ(selected.size(), 1u);
    EXPECT_EQ(lights[selected[0]].entity, 4u);
    for (Entity entity : {Entity(1), Entity(2), Entity(6)})
        EXPECT_EQ(recordOf(considered, entity)->reason, "tier_cap") << entity;
    EXPECT_EQ(recordOf(considered, 3)->reason, "frustum");

    // past the far plane, within the distance: culled, unless the camera has no far clip (the far
    // plane is not tested then)
    const std::vector<FogLightCandidate> past = {light(10, Vector3(0.0f, 0.0f, -150.0f), 10.0f)};
    AtmosphereRenderer::selectFogLights(past, planes, 100.0f, Vector3(0.0f, 0.0f, 0.0f), 200.0f, 4, selected, considered);
    EXPECT_TRUE(selected.empty());
    EXPECT_EQ(considered[0].reason, "frustum");
    AtmosphereRenderer::selectFogLights(past, planes, 0.0f, Vector3(0.0f, 0.0f, 0.0f), 200.0f, 4, selected, considered);
    EXPECT_EQ(selected.size(), 1u);

    // nothing to consider: no records
    AtmosphereRenderer::selectFogLights({}, planes, 100.0f, Vector3(0.0f, 0.0f, 0.0f), 60.0f, 4, selected, considered);
    EXPECT_TRUE(selected.empty());
    EXPECT_TRUE(considered.empty());
}
