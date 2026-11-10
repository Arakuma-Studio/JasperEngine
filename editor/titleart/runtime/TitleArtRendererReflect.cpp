// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Title Art Renderer on S08's reflection tables (docs/specs/S81-title-art-runtime.md R4,
// R58, §6.1): the scene codec (defaults omitted), the Catalog properties, MCP, the exported
// code and the asset locators all read this table. `overrides` is saved as a list of entries
// and listed as one property per Exposed parameter, "overrides.<name>" (extraProperty), so a
// prefab variant overrides `text` alone and keeps following its base's color scheme; the
// entries are matched by name, never by index.

#include "reflect/Components.h"

#include "Factory.h"
#include "Stream.h"
#include "component/TitleArtRendererComponent.h"
#include "subsystem/TitleArtSystem.h"
#include "titleart/runtime/TitleArtOverrides.h"

#include <cstring>

namespace jasperengine::editor::reflect {

namespace {

    template<typename E>
    constexpr int v(E value) { return static_cast<int>(value); }

    constexpr EnumItem kTitleArtRenderModeItems[] = {
        {v(TitleArtRenderMode::BAKED), "baked", "Baked", "TitleArtRenderMode::BAKED"},
        {v(TitleArtRenderMode::REALTIME), "realtime", "Realtime", "TitleArtRenderMode::REALTIME"},
        {v(TitleArtRenderMode::GLYPH_ATLAS), "glyph_atlas", "Glyph Atlas", "TitleArtRenderMode::GLYPH_ATLAS"},
    };

    constexpr EnumItem kTitleArtTransitionItems[] = {
        {v(TitleArtTransition::CUT), "cut", "Cut", "TitleArtTransition::CUT"},
        {v(TitleArtTransition::CROSSFADE), "crossfade", "Crossfade", "TitleArtTransition::CROSSFADE"},
    };

    // the entries of `overrides` (saved; listed only as "overrides.<name>")
    constexpr FieldDesc kTitleArtOverrideFields[] = {
        JE_FIELD(TitleArtOverride, name),
        JE_FIELD(TitleArtOverride, type),
        JE_FIELD(TitleArtOverride, value, .flags = OmitIfDefault),
        JE_FIELD(TitleArtOverride, text, .flags = OmitIfDefault),
    };

}

const EnumDesc kTitleArtRenderMode{"TitleArtRenderMode", kTitleArtRenderModeItems, v(TitleArtRenderMode::BAKED)};
const EnumDesc kTitleArtTransition{"TitleArtTransition", kTitleArtTransitionItems, v(TitleArtTransition::CUT)};

JE_REFLECT_STRUCT_TRAITS(TitleArtOverride, kTitleArtOverrideStruct);
const StructDesc kTitleArtOverrideStruct = JE_STRUCT_DESC(TitleArtOverride, "TitleArtOverride", kTitleArtOverrideFields);

namespace {

    // R61: the Realtime and Glyph Atlas rows apply to those modes (drawn disabled, never hidden)
    bool liveMode(const void* owner) {
        return static_cast<const TitleArtRendererComponent*>(owner)->renderMode != TitleArtRenderMode::BAKED;
    }
    bool atlasMode(const void* owner) {
        return static_cast<const TitleArtRendererComponent*>(owner)->renderMode == TitleArtRenderMode::GLYPH_ATLAS;
    }
    bool crossfades(const void* owner) {
        const TitleArtRendererComponent* t = static_cast<const TitleArtRendererComponent*>(owner);
        return t->renderMode != TitleArtRenderMode::BAKED && t->transition == TitleArtTransition::CROSSFADE;
    }

    constexpr const char* kLive = "Live Effects";
    constexpr const char* kAnimation = "Animation";

    constexpr FieldDesc kTitleArtFields[] = {
        JE_FIELD(TitleArtRendererComponent, enabled),
        JE_FIELD(TitleArtRendererComponent, title, .ops = &ops::AssetRefIfSet, .asset = AssetKind::TitleArt, .label = "Title Art", .uiOrder = 1),
        JE_FIELD(TitleArtRendererComponent, renderMode, .enumDesc = &kTitleArtRenderMode, .flags = OmitIfDefault, .uiOrder = 2),
        JE_FIELD(TitleArtRendererComponent, overrides, .flags = OmitIfDefault | NullIfEmpty | NotListed | Hidden, .label = "Parameters", .uiOrder = 3),
        JE_FIELD(TitleArtRendererComponent, language, .flags = OmitIfDefault,
                 .tooltip = "The language this title shows; empty is Auto (TitleArt.setLanguage, the device, the project default)", .uiOrder = 4),
        JE_FIELD(TitleArtRendererComponent, pivotFromTitle, .flags = OmitIfDefault, .uiOrder = 10),
        JE_FIELD(TitleArtRendererComponent, uiScale, .flags = OmitIfDefault, .min = 0.01f, .max = 100.0f, .step = 0.01f, .label = "UI Scale", .uiOrder = 11),
        JE_FIELD(TitleArtRendererComponent, pixelsPerUnit, .flags = OmitIfDefault, .min = 0.0f, .max = 100000.0f, .step = 1.0f,
                 .tooltip = "World size of Set Native Size: frame pixels per unit (0: 1 pixel per unit, a 2D scene's unit)", .uiOrder = 12),
        JE_FIELD(TitleArtRendererComponent, resolutionScale, .flags = OmitIfDefault | Clamped, .min = 0.25f, .max = 2.0f, .step = 0.05f,
                 .group = "Realtime", .enabledIf = &liveMode, .uiOrder = 20),
        JE_FIELD(TitleArtRendererComponent, priority, .flags = OmitIfDefault | Clamped, .min = -100.0f, .max = 100.0f, .step = 1.0f,
                 .group = "Realtime", .enabledIf = &liveMode, .uiOrder = 21),
        JE_FIELD(TitleArtRendererComponent, renderWhenHidden, .flags = OmitIfDefault, .group = "Realtime", .enabledIf = &liveMode, .uiOrder = 22),
        JE_FIELD(TitleArtRendererComponent, prebakeInitialState, .flags = OmitIfDefault, .group = "Realtime", .enabledIf = &liveMode, .uiOrder = 23),
        JE_FIELD(TitleArtRendererComponent, transition, .enumDesc = &kTitleArtTransition, .flags = OmitIfDefault, .group = "Realtime",
                 .enabledIf = &liveMode, .uiOrder = 24),
        JE_FIELD(TitleArtRendererComponent, transitionDuration, .flags = OmitIfDefault | Clamped, .unit = Unit::Seconds, .min = 0.0f, .max = 2.0f,
                 .step = 0.01f, .group = "Realtime", .enabledIf = &crossfades, .uiOrder = 25),
        JE_FIELD(TitleArtRendererComponent, additionalCharacters, .flags = OmitIfDefault, .group = "Characters", .enabledIf = &liveMode, .uiOrder = 30),
        JE_FIELD(TitleArtRendererComponent, atlasPageSize, .flags = OmitIfDefault, .min = 512.0f, .max = 2048.0f, .group = "Glyph Atlas",
                 .drawer = "po2Slider", .enabledIf = &atlasMode, .uiOrder = 31),
        JE_FIELD(TitleArtRendererComponent, atlasMaxPages, .flags = OmitIfDefault | Clamped, .min = 1.0f, .max = 8.0f, .step = 1.0f,
                 .label = "Atlas Max Pages", .group = "Glyph Atlas", .enabledIf = &atlasMode, .uiOrder = 32),
        JE_FIELD(TitleArtRendererComponent, qualityTier, .flags = OmitIfDefault, .group = "Realtime", .enabledIf = &liveMode, .uiOrder = 26),
        JE_FIELD(TitleArtRendererComponent, runtimeEnvironments, .flags = OmitIfDefault | NullIfEmpty, .group = "Realtime", .enabledIf = &liveMode,
                 .uiOrder = 27),
        JE_FIELD(TitleArtRendererComponent, liveTime, .flags = OmitIfDefault, .unit = Unit::Seconds, .min = 0.0f, .step = 0.01f,
                 .label = "Live Time", .group = kLive, .uiOrder = 40),
        JE_FIELD(TitleArtRendererComponent, liveUnscaledTime, .flags = OmitIfDefault, .label = "Unscaled Time", .group = kLive, .uiOrder = 41),
        JE_FIELD(TitleArtRendererComponent, sweepEnabled, .flags = OmitIfDefault, .label = "Sweep", .group = kLive, .uiOrder = 42),
        JE_FIELD(TitleArtRendererComponent, sweepPosition, .flags = OmitIfDefault | Clamped, .min = 0.0f, .max = 1.0f, .step = 0.01f,
                 .label = "Sweep Position", .group = kLive, .uiOrder = 43),
        JE_FIELD(TitleArtRendererComponent, sweepWidth, .flags = OmitIfDefault | Clamped, .min = 0.0f, .max = 4.0f, .step = 0.01f,
                 .label = "Sweep Width", .group = kLive, .uiOrder = 44),
        JE_FIELD(TitleArtRendererComponent, sweepAngle, .flags = OmitIfDefault | Clamped, .unit = Unit::Degrees, .min = -360.0f, .max = 360.0f,
                 .label = "Sweep Angle", .group = kLive, .uiOrder = 45),
        JE_FIELD(TitleArtRendererComponent, sweepSoftness, .flags = OmitIfDefault | Clamped, .min = 0.0f, .max = 1.0f, .step = 0.01f,
                 .label = "Sweep Softness", .group = kLive, .uiOrder = 46),
        JE_FIELD(TitleArtRendererComponent, sweepColor, .flags = OmitIfDefault | ColorLinear | Hdr, .label = "Sweep Color", .group = kLive, .uiOrder = 47),
        JE_FIELD(TitleArtRendererComponent, sweepAuto, .flags = OmitIfDefault, .label = "Sweep Auto", .group = kLive, .uiOrder = 48),
        JE_FIELD(TitleArtRendererComponent, sweepSpeed, .flags = OmitIfDefault, .min = 0.0f, .step = 0.05f,
                 .label = "Sweep Speed", .tooltip = "Passes per second", .group = kLive, .uiOrder = 49),
        JE_FIELD(TitleArtRendererComponent, sweepInterval, .flags = OmitIfDefault, .unit = Unit::Seconds, .min = 0.01f, .step = 0.05f,
                 .label = "Sweep Interval", .tooltip = "Seconds between sweep starts", .group = kLive, .uiOrder = 50),
        JE_FIELD(TitleArtRendererComponent, glowEnabled, .flags = OmitIfDefault, .label = "Glow", .group = kLive, .uiOrder = 51),
        JE_FIELD(TitleArtRendererComponent, glowPulse, .flags = OmitIfDefault, .min = 0.0f, .max = 16.0f, .step = 0.01f,
                 .label = "Glow Pulse", .group = kLive, .uiOrder = 52),
        JE_FIELD(TitleArtRendererComponent, glowColor, .flags = OmitIfDefault | ColorLinear | Hdr, .label = "Glow Color", .group = kLive, .uiOrder = 53),
        JE_FIELD(TitleArtRendererComponent, tintEnabled, .flags = OmitIfDefault, .label = "Tint", .group = kLive, .uiOrder = 54),
        JE_FIELD(TitleArtRendererComponent, tintColor, .flags = OmitIfDefault | ColorLinear, .label = "Tint Color", .group = kLive, .uiOrder = 55),
        JE_FIELD(TitleArtRendererComponent, tintAmount, .flags = OmitIfDefault | Clamped, .min = 0.0f, .max = 1.0f, .step = 0.01f,
                 .label = "Tint Amount", .group = kLive, .uiOrder = 56),
        JE_FIELD(TitleArtRendererComponent, glintsEnabled, .flags = OmitIfDefault, .label = "Glints", .group = kLive, .uiOrder = 57),
        JE_FIELD(TitleArtRendererComponent, glintAmount, .flags = OmitIfDefault, .min = 0.0f, .max = 16.0f, .step = 0.01f,
                 .label = "Glint Amount", .group = kLive, .uiOrder = 58),
        JE_FIELD(TitleArtRendererComponent, glintSize, .flags = OmitIfDefault | Clamped, .min = 0.0f, .max = 1.0f, .step = 0.005f,
                 .label = "Glint Size", .tooltip = "Fraction of the frame height", .group = kLive, .uiOrder = 59),
        JE_FIELD(TitleArtRendererComponent, glintRate, .flags = OmitIfDefault, .min = 0.0f, .max = 60.0f, .step = 0.01f,
                 .label = "Glint Rate", .group = kLive, .uiOrder = 60),
        JE_FIELD(TitleArtRendererComponent, glintSeed, .flags = OmitIfDefault, .label = "Glint Seed", .group = kLive, .uiOrder = 61),
        JE_FIELD(TitleArtRendererComponent, glintColor, .flags = OmitIfDefault | ColorLinear | Hdr, .label = "Glint Color", .group = kLive, .uiOrder = 62),
        // S85's animations (docs/specs/S85-title-animation.md §6.5, R49)
        JE_FIELD(TitleArtRendererComponent, playAutomatically, .flags = OmitIfDefault, .label = "Play Automatically", .group = kAnimation, .uiOrder = 70),
        JE_FIELD(TitleArtRendererComponent, defaultAnimation, .flags = OmitIfDefault, .label = "Default Animation", .group = kAnimation, .uiOrder = 71),
        JE_FIELD(TitleArtRendererComponent, animationSpeed, .flags = OmitIfDefault | Clamped, .min = 0.0f, .max = 10.0f, .step = 0.01f,
                 .label = "Animation Speed", .group = kAnimation, .uiOrder = 72),
        JE_FIELD(TitleArtRendererComponent, animationUnscaledTime, .flags = OmitIfDefault, .label = "Animation Update", .group = kAnimation,
                 .uiOrder = 73),
        JE_FIELD(TitleArtRendererComponent, driveAnimation, .flags = OmitIfDefault, .label = "Drive Animation", .group = kAnimation, .uiOrder = 74),
        JE_FIELD(TitleArtRendererComponent, animationTime, .flags = NoSave | Hidden | NoCodegen, .unit = Unit::Seconds, .min = 0.0f, .step = 0.01f,
                 .label = "Animation Time", .group = kAnimation, .uiOrder = 75),
    };

    // the state stays with the component (Lua's events, the live render, what it drives of its
    // companion); a decode changes values, never who owns them
    void titleArtPostDecode(void* comp, const DecodeContext& ctx) {
        TitleArtRendererComponent& title = *static_cast<TitleArtRendererComponent*>(comp);
        if (const TitleArtRendererComponent* old = static_cast<const TitleArtRendererComponent*>(ctx.old)) {
            title.runtime = old->runtime;
        }
        title.needUpdate = true;
    }

    PropertyData titleArtExtraProperty(void* comp, const std::string& path) {
        return titleArtOverrideProperty(*static_cast<TitleArtRendererComponent*>(comp), path);
    }

    void titleArtExtraProperties(void* comp, std::map<std::string, PropertyData>& out) {
        if (comp) listTitleArtOverrideProperties(*static_cast<TitleArtRendererComponent*>(comp), out);
    }

}

const ComponentDesc kTitleArtRendererDesc{
    .type = ComponentType::TitleArtRendererComponent, .cppName = "TitleArtRendererComponent", .key = "titleart",
    .displayName = "Title Art Renderer", .icon = ICON_FA_WAND_MAGIC_SPARKLES,
    .body = JE_STRUCT_DESC(TitleArtRendererComponent, "TitleArtRendererComponent", kTitleArtFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(TitleArtRendererComponent),
    .postDecode = &titleArtPostDecode,
    .codegenVar = "titleArt",
    .extraProperty = &titleArtExtraProperty,
    .extraProperties = &titleArtExtraProperties,
    .extraUpdateFlags = 0,
};

}
