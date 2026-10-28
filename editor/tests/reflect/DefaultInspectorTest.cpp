// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The default Inspector's multi-object comparison (docs/specs/S08-reflection.md R9, AC8): a field
// differs per value (floats within 1e-4), a vector or colour per lane, an array by its length
// (its size then read-only) and then element by element.

#include "reflect/Reflect.h"

#include "component/Joint3DComponent.h"
#include "component/LightComponent.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor;

// (a named namespace, tests included: a unity build merges the test sources)
namespace default_inspector_test {

    TEST(DefaultInspector, MixedDetection) {
        const reflect::ComponentDesc& light = *reflect::find(ComponentType::LightComponent);
        LightComponent a, b, c;
        a.intensity = 1.0f;
        b.intensity = 1.0f;
        c.intensity = 2.0f;
        a.color = b.color = c.color = Vector3(1.0f, 0.5f, 0.25f);
        const std::vector<void*> lights = {&a, &b, &c};
        EXPECT_TRUE(reflect::mixedValues(light, lights, "intensity").any);
        EXPECT_FALSE(reflect::mixedValues(light, lights, "color").any);

        // within the Inspector's 1e-4 the same value
        c.intensity = 1.00001f;
        EXPECT_FALSE(reflect::mixedValues(light, lights, "intensity").any);

        // one lane differs: only that lane is mixed
        b.color = Vector3(1.0f, 0.7f, 0.25f);
        const reflect::Mixed color = reflect::mixedValues(light, lights, "color");
        EXPECT_TRUE(color.any);
        EXPECT_FALSE(color.lanes[0]);
        EXPECT_TRUE(color.lanes[1]);
        EXPECT_FALSE(color.lanes[2]);

        // arrays of different lengths: mixed, their size read-only; of one length, by element
        const reflect::ComponentDesc& joint = *reflect::find(ComponentType::Joint3DComponent);
        Joint3DComponent j1, j2;
        j1.pathPoints = {Vector3(0, 0, 0)};
        j2.pathPoints = {Vector3(0, 0, 0), Vector3(1, 1, 1)};
        const std::vector<void*> joints = {&j1, &j2};
        const reflect::Mixed lengths = reflect::mixedValues(joint, joints, "pathPoints");
        EXPECT_TRUE(lengths.any);
        EXPECT_TRUE(lengths.sizeDiffers);
        j2.pathPoints.pop_back();
        EXPECT_FALSE(reflect::mixedValues(joint, joints, "pathPoints").any);
        j2.pathPoints[0] = Vector3(0, 2, 0);
        const reflect::Mixed elements = reflect::mixedValues(joint, joints, "pathPoints");
        EXPECT_TRUE(elements.any);
        EXPECT_FALSE(elements.sizeDiffers);
        // one element's lanes
        const reflect::Mixed element = reflect::mixedValues(joint, joints, "pathPoints[0]");
        EXPECT_TRUE(element.lanes[1]);
        EXPECT_FALSE(element.lanes[0]);
    }

}
