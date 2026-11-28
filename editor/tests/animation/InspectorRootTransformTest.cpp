// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector's Root Transform rows (docs/specs/S94-rendering-anim-gaps.md F32): an edit sets
// only its own field on every selected clip, and a mixed field is found per member.

#include <gtest/gtest.h>

#include "window/InspectorWindowClips.h"

namespace InspectorRootTransformTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;

TEST(InspectorRootTransform, AnEditSetsOnlyItsField){
    anim::ClipRootMotion drawn;
    drawn.rotationBasedUpon = anim::RootRotationBasis::BodyOrientation;
    drawn.rotationOffset = 15.0f;
    inspectorclips::RootTransformEdit edit;
    edit.value = drawn;
    edit.field = inspectorclips::RootTransformField::RotationBasedUpon;
    anim::ClipRootMotion other;
    other.bakePositionXZ = true;
    edit.applyTo(other);
    EXPECT_EQ(other.rotationBasedUpon, anim::RootRotationBasis::BodyOrientation);
    EXPECT_EQ(other.rotationOffset, 0.0f) << "the drawn clip's offset stays on it";
    EXPECT_TRUE(other.bakePositionXZ) << "the other clip's own settings stay";
}

TEST(InspectorRootTransform, MixedPerMember){
    anim::ClipRootMotion a, b;
    b.positionYBasedUpon = anim::RootPositionYBasis::Feet;
    inspectorclips::RootTransformMixed mixed;
    mixed.add(a, b);
    EXPECT_TRUE(mixed.positionYBasedUpon);
    EXPECT_FALSE(mixed.rotationBasedUpon);
    EXPECT_FALSE(mixed.positionYOffset);
    EXPECT_STREQ(inspectorclips::rootPositionYBasisLabel(anim::RootPositionYBasis::CenterOfMass), "Center of Mass");
}

}  // namespace InspectorRootTransformTest_cpp
