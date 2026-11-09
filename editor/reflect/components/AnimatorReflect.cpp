// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animator (docs/specs/S21-animation.md R1-R15, S08 D7): every field in the tables with the
// generic YAML codec; the IK stack and the clip overrides as lists of structs. A field an
// Animator binds with (controller, overrides, IK, written bones, retargeting, root motion bone)
// sets UpdateFlags_Animator_Reload: the next update binds it again, keeping its states.

#include "reflect/Components.h"

#include "Factory.h"
#include "component/AnimatorComponent.h"

namespace jasperengine::editor::reflect {

namespace {

    constexpr uint64_t kReload = UpdateFlags_Animator_Reload;

    bool isTwoBone(const void* c) { return static_cast<const AnimatorIKConstraint*>(c)->type == AnimatorIKType::TwoBone; }
    bool isFabrik(const void* c) { return static_cast<const AnimatorIKConstraint*>(c)->type == AnimatorIKType::Fabrik; }
    bool isLookAt(const void* c) { return static_cast<const AnimatorIKConstraint*>(c)->type == AnimatorIKType::LookAt; }
    bool isFootIK(const void* c) { return static_cast<const AnimatorIKConstraint*>(c)->type == AnimatorIKType::FootIK; }
    bool isFootLock(const void* c) { return static_cast<const AnimatorIKConstraint*>(c)->type == AnimatorIKType::FootLock; }
    bool hasTarget(const void* c) {
        const AnimatorIKType type = static_cast<const AnimatorIKConstraint*>(c)->type;
        return type == AnimatorIKType::TwoBone || type == AnimatorIKType::Fabrik || type == AnimatorIKType::LookAt;
    }
    bool isFoot(const void* c) { return isFootIK(c) || isFootLock(c); }
    bool isLeg(const void* c) { return isTwoBone(c) || isFoot(c); }

    constexpr FieldDesc kIKConstraintFields[] = {
        JE_FIELD(AnimatorIKConstraint, name, .updateFlags = kReload, .tooltip = "Scripts set its target and weight by this name."),
        JE_FIELD(AnimatorIKConstraint, type, .enumDesc = &enums::kAnimatorIKType, .updateFlags = kReload),
        JE_FIELD(AnimatorIKConstraint, enabled),
        JE_FIELD(AnimatorIKConstraint, weight, .flags = Clamped, .min = 0.0f, .max = 1.0f),
        JE_FIELD(AnimatorIKConstraint, weightParameter, .tooltip = "A Float parameter of the controller that multiplies the weight."),
        JE_FIELD(AnimatorIKConstraint, bones, .flags = ListWhole, .updateFlags = kReload, .tooltip = "Paths from the Animator's GameObject or humanoid slots (left_foot). Two Bone: upper, mid, end; "
                            "FABRIK: the chain root first; Look At: up to 4, parents first; Foot IK / Foot Lock: upper leg, lower leg, foot."),
        JE_FIELD(AnimatorIKConstraint, boneWeights, .flags = ListWhole, .visibleIf = &isLookAt),
        JE_FIELD(AnimatorIKConstraint, target, .ops = &ops::Entity, .tooltip = "Its world position (and rotation) is the target.", .visibleIf = &hasTarget),
        JE_FIELD(AnimatorIKConstraint, targetPosition, .tooltip = "World position, without a target GameObject.", .visibleIf = &hasTarget),
        JE_FIELD(AnimatorIKConstraint, useTargetRotation, .visibleIf = &isTwoBone),
        JE_FIELD(AnimatorIKConstraint, pole, .ops = &ops::Entity, .visibleIf = &isTwoBone),
        JE_FIELD(AnimatorIKConstraint, polePosition, .visibleIf = &isTwoBone),
        JE_FIELD(AnimatorIKConstraint, usePole, .tooltip = "Bend toward the pole position (off: keep the current bend).", .visibleIf = &isTwoBone),
        JE_FIELD(AnimatorIKConstraint, soften, .flags = Clamped | OmitIfDefault, .min = 0.0f, .max = 1.0f,
                 .tooltip = "The part of the reach over which the bone eases toward full extension, so the joint keeps moving (0: a hard limit; try 0.03).", .visibleIf = &isLeg),
        JE_FIELD(AnimatorIKConstraint, hingeAxis, .flags = OmitIfDefault,
                 .tooltip = "The middle bone's local axis it only bends about; a positive turn straightens it. Zero: auto, toward the pole or the current bend.", .visibleIf = &isLeg),
        JE_FIELD(AnimatorIKConstraint, twist, .flags = Clamped | OmitIfDefault, .unit = Unit::Degrees, .min = -180.0f, .max = 180.0f,
                 .tooltip = "Turns the chain about the line from its first bone to the target.", .visibleIf = &isLeg),
        JE_FIELD(AnimatorIKConstraint, iterations, .flags = Clamped, .min = 1.0f, .max = 64.0f, .visibleIf = &isFabrik),
        JE_FIELD(AnimatorIKConstraint, tolerance, .flags = Clamped, .unit = Unit::Meters, .min = 0.0f, .max = 1.0f, .visibleIf = &isFabrik),
        JE_FIELD(AnimatorIKConstraint, forwardAxis, .tooltip = "The last bone's local axis that turns to the target.", .visibleIf = &isLookAt),
        JE_FIELD(AnimatorIKConstraint, clampAngle, .flags = Clamped, .unit = Unit::Degrees, .min = 0.0f, .max = 180.0f, .visibleIf = &isLookAt),
        JE_FIELD(AnimatorIKConstraint, aimOffset, .flags = OmitIfDefault, .tooltip = "The eyes, from the last bone in its local space: their forward ray ends on the target.", .visibleIf = &isLookAt),
        JE_FIELD(AnimatorIKConstraint, pelvis, .updateFlags = kReload, .tooltip = "The bone that drops (empty: the hips).", .visibleIf = &isFootIK),
        JE_FIELD(AnimatorIKConstraint, footHeight, .unit = Unit::Meters, .tooltip = "The ankle above the sole.", .visibleIf = &isFootIK),
        JE_FIELD(AnimatorIKConstraint, rayStart, .unit = Unit::Meters, .visibleIf = &isFootIK),
        JE_FIELD(AnimatorIKConstraint, rayLength, .unit = Unit::Meters, .visibleIf = &isFootIK),
        JE_FIELD(AnimatorIKConstraint, layerMask, .ops = &ops::LayerMask, .visibleIf = &isFootIK),
        JE_FIELD(AnimatorIKConstraint, maxFootAngle, .flags = Clamped, .unit = Unit::Degrees, .min = 0.0f, .max = 90.0f, .visibleIf = &isFootIK),
        JE_FIELD(AnimatorIKConstraint, smoothing, .flags = Clamped, .unit = Unit::Seconds, .min = 0.0f, .max = 2.0f, .visibleIf = &isFootIK),
        JE_FIELD(AnimatorIKConstraint, contactParameter, .tooltip = "A Float parameter: 0.5 or more is contact (empty: by speed).", .visibleIf = &isFootLock),
        JE_FIELD(AnimatorIKConstraint, speedThreshold, .unit = Unit::MetersPerSecond, .visibleIf = &isFootLock),
        JE_FIELD(AnimatorIKConstraint, releaseTime, .unit = Unit::Seconds, .visibleIf = &isFoot),
    };

    constexpr FieldDesc kClipOverrideFields[] = {
        JE_FIELD(AnimatorClipOverride, original, .updateFlags = kReload, .tooltip = "A clip path as the controller references it."),
        JE_FIELD(AnimatorClipOverride, replacement, .updateFlags = kReload, .tooltip = "An .anim or a model clip (model.gltf#Walk) played instead."),
    };

}

JE_REFLECT_STRUCT_TRAITS(AnimatorIKConstraint, kAnimatorIKConstraintStruct);
const StructDesc kAnimatorIKConstraintStruct = JE_STRUCT_DESC(AnimatorIKConstraint, "AnimatorIKConstraint", kIKConstraintFields);
JE_REFLECT_STRUCT_TRAITS(AnimatorClipOverride, kAnimatorClipOverrideStruct);
const StructDesc kAnimatorClipOverrideStruct = JE_STRUCT_DESC(AnimatorClipOverride, "AnimatorClipOverride", kClipOverrideFields);

namespace {

    bool exposes(const void* c) { return static_cast<const AnimatorComponent*>(c)->writeBones == AnimatorWriteBones::Exposed; }

    constexpr TypeOps kExposedBonesOps = ops::PushBackList<std::string>;

    constexpr FieldDesc kAnimatorFields[] = {
        JE_FIELD(AnimatorComponent, enabled),
        JE_FIELD(AnimatorComponent, controller, .updateFlags = kReload, .asset = AssetKind::AnimatorController, .tooltip = "The Animator Controller (.controller) it plays."),
        JE_FIELD(AnimatorComponent, speed, .tooltip = "Multiplies every state's speed."),
        JE_FIELD(AnimatorComponent, poseFrameRate, .flags = OmitIfDefault | CodegenIfChanged, .min = 0.0f, .max = 120.0f, .label = "Pose Frame Rate",
                 .tooltip = "Frames per second the poses hold on (0: smooth). Each track steps from its previous key, so every key shows; transitions step too. Root motion and events stay at the true time.", .drawer = "poseFrameRate"),
        JE_FIELD(AnimatorComponent, applyRootMotion, .tooltip = "The clips' root motion moves the GameObject (or its Character Controller)."),
        JE_FIELD(AnimatorComponent, rootMotionBone, .updateFlags = kReload, .tooltip = "Path or humanoid slot (empty: the clip's, the hips, the shallowest moving bone)."),
        JE_FIELD(AnimatorComponent, updateMode, .enumDesc = &enums::kAnimatorUpdateMode),
        JE_FIELD(AnimatorComponent, cullingMode, .enumDesc = &enums::kAnimatorCullingMode, .tooltip = "Cull Update Transforms: while no camera sees it, time, states, events and root motion advance; nothing is written."),
        JE_FIELD(AnimatorComponent, writeBones, .enumDesc = &enums::kAnimatorWriteBones, .updateFlags = kReload, .tooltip = "Exposed: only the listed bones and bones with attachments get their GameObject's transform (skins always follow)."),
        JE_FIELD(AnimatorComponent, exposedBones, .ops = &kExposedBonesOps, .flags = ListWhole, .updateFlags = kReload, .visibleIf = &exposes),
        JE_FIELD(AnimatorComponent, eventMode, .enumDesc = &enums::kAnimatorEventMode, .tooltip = "Leader Only: per layer the clip with the highest weight fires its events."),
        JE_FIELD(AnimatorComponent, sendEventsToScripts, .tooltip = "Also calls the scripts' methods named like the events."),
        JE_FIELD(AnimatorComponent, retarget, .enumDesc = &enums::kAnimatorRetarget, .updateFlags = kReload, .tooltip = "Auto: humanoid clips made on another humanoid model play retargeted."),
        JE_FIELD(AnimatorComponent, clipOverrides, .flags = ListWhole | ListTemplate | NoCodegen, .updateFlags = kReload),
        JE_FIELD(AnimatorComponent, ik, .flags = ListWhole | ListTemplate | NoCodegen, .updateFlags = kReload, .label = "IK"),
    };

    std::string literal(const std::string& text){ return Factory::formatString(text); }

    // the lists of structs as statements (the generic codegen writes scalar fields only)
    void animatorCodegen(const void* comp, const CodegenContext& ctx, std::string& out){
        const AnimatorComponent& animator = *static_cast<const AnimatorComponent*>(comp);
        const std::string ind = Factory::indentation(ctx.indentSpaces);
        const std::string& v = ctx.variable;
        for (const AnimatorClipOverride& entry : animator.clipOverrides){
            out += ind + v + ".clipOverrides.push_back({" + literal(entry.original) + ", " + literal(entry.replacement) + "});\n";
        }
        for (size_t i = 0; i < animator.ik.size(); i++){
            const AnimatorIKConstraint& c = animator.ik[i];
            const AnimatorIKConstraint d;
            const std::string var = v + "_ik" + std::to_string(i);
            out += ind + "{\n";
            const std::string in = ind + "    ";
            out += in + "AnimatorIKConstraint " + var + ";\n";
            auto set = [&](const char* name, const std::string& value){ out += in + var + "." + name + " = " + value + ";\n"; };
            set("name", literal(c.name));
            const EnumItem* type = enumItem(enums::kAnimatorIKType, (int)c.type);
            set("type", type ? type->cppName : "AnimatorIKType::TwoBone");
            if (c.enabled != d.enabled) set("enabled", c.enabled ? "true" : "false");
            if (c.weight != d.weight) set("weight", Factory::formatFloat(c.weight));
            if (!c.weightParameter.empty()) set("weightParameter", literal(c.weightParameter));
            for (const std::string& bone : c.bones) out += in + var + ".bones.push_back(" + literal(bone) + ");\n";
            for (float w : c.boneWeights) out += in + var + ".boneWeights.push_back(" + Factory::formatFloat(w) + ");\n";
            if (c.target != NULL_ENTITY) set("target", Factory::formatEntity(c.target, ctx.entityVarNames));
            if (c.targetPosition != d.targetPosition) set("targetPosition", Factory::formatVector3(c.targetPosition));
            if (c.useTargetRotation) set("useTargetRotation", "true");
            if (c.pole != NULL_ENTITY) set("pole", Factory::formatEntity(c.pole, ctx.entityVarNames));
            if (c.polePosition != d.polePosition) set("polePosition", Factory::formatVector3(c.polePosition));
            if (c.usePole) set("usePole", "true");
            if (c.soften != d.soften) set("soften", Factory::formatFloat(c.soften));
            if (c.hingeAxis != d.hingeAxis) set("hingeAxis", Factory::formatVector3(c.hingeAxis));
            if (c.twist != d.twist) set("twist", Factory::formatFloat(c.twist));
            if (c.iterations != d.iterations) set("iterations", std::to_string(c.iterations));
            if (c.tolerance != d.tolerance) set("tolerance", Factory::formatFloat(c.tolerance));
            if (c.forwardAxis != d.forwardAxis) set("forwardAxis", Factory::formatVector3(c.forwardAxis));
            if (c.clampAngle != d.clampAngle) set("clampAngle", Factory::formatFloat(c.clampAngle));
            if (c.aimOffset != d.aimOffset) set("aimOffset", Factory::formatVector3(c.aimOffset));
            if (!c.pelvis.empty()) set("pelvis", literal(c.pelvis));
            if (c.footHeight != d.footHeight) set("footHeight", Factory::formatFloat(c.footHeight));
            if (c.rayStart != d.rayStart) set("rayStart", Factory::formatFloat(c.rayStart));
            if (c.rayLength != d.rayLength) set("rayLength", Factory::formatFloat(c.rayLength));
            if (c.layerMask != d.layerMask) set("layerMask", std::to_string(c.layerMask) + "u");
            if (c.maxFootAngle != d.maxFootAngle) set("maxFootAngle", Factory::formatFloat(c.maxFootAngle));
            if (c.smoothing != d.smoothing) set("smoothing", Factory::formatFloat(c.smoothing));
            if (!c.contactParameter.empty()) set("contactParameter", literal(c.contactParameter));
            if (c.speedThreshold != d.speedThreshold) set("speedThreshold", Factory::formatFloat(c.speedThreshold));
            if (c.releaseTime != d.releaseTime) set("releaseTime", Factory::formatFloat(c.releaseTime));
            out += in + v + ".ik.push_back(" + var + ");\n";
            out += ind + "}\n";
        }
        out += ind + v + ".needsLoad = true;\n";
    }

    // what it played and its subscriptions are runtime: a decoded component starts afresh
    void animatorPostDecode(void* comp, const DecodeContext&){
        AnimatorComponent& animator = *static_cast<AnimatorComponent*>(comp);
        animator.needsLoad = true;
    }

}

const ComponentDesc kAnimatorDesc{
    .type = ComponentType::AnimatorComponent, .cppName = "AnimatorComponent", .key = "animator",
    .displayName = "Animator", .icon = ICON_FA_DIAGRAM_PROJECT,
    .body = JE_STRUCT_DESC(AnimatorComponent, "AnimatorComponent", kAnimatorFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(AnimatorComponent),
    .postDecode = &animatorPostDecode,
    .decodeApply = DecodeApply::MarkOnly,
    .codegenVar = "animator",
    .codegenExtra = &animatorCodegen,
};

}
