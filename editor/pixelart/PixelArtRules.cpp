// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/PixelArtRules.h"

#include "pixelart/ApplyPalette.h"
#include "pixelart/PixelPerfectLayout.h"
#include "pixelart/PixelRotation.h"
#include "pixelart/PixelSnap.h"
#include "pixelart/RgbaImage.h"
#include "texture/TextureFormat.h"
#include "texture/TextureFormatSelector.h"
#include "texture/TexturePrepare.h"
#include "util/Sha256.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::pixelart;
using Json = nlohmann::json;

namespace {

    constexpr double kTexelTolerance = 1e-4;
    constexpr double kGridTolerance = 1e-3;
    constexpr double kAngleTolerance = 1e-3;     // degrees
    constexpr double kDegrees = 57.29577951308232;

    // the import section as JSON (what a fix shows and the applier writes back as YAML)
    Json yamlJson(const YAML::Node& node){
        if (node.IsMap()){
            Json out = Json::object();
            for (const auto& entry : node)
                out[entry.first.as<std::string>()] = yamlJson(entry.second);
            return out;
        }
        if (node.IsSequence()){
            Json out = Json::array();
            for (const YAML::Node& item : node)
                out.push_back(yamlJson(item));
            return out;
        }
        if (!node.IsScalar())
            return nullptr;
        const std::string text = node.Scalar();
        if (node.Tag() != "!" && (text == "true" || text == "false"))
            return text == "true";
        if (node.Tag() != "!"){
            char* end = nullptr;
            const long long integer = std::strtoll(text.c_str(), &end, 10);
            if (!text.empty() && end && *end == '\0')
                return integer;
            const double real = std::strtod(text.c_str(), &end);
            if (!text.empty() && end && *end == '\0')
                return number(real);
        }
        return text;
    }

    std::string entityRef(const std::string& scene, const std::string& entity){
        return scene + "#" + entity;
    }

    Json entityTarget(const std::string& scene, const std::string& entity, const std::string& field = std::string()){
        Json target = {{"kind", "entity"}, {"scene", scene}, {"entity", entity}};
        if (!field.empty())
            target["field"] = field;
        return target;
    }

    Json textureTarget(const std::string& path){
        return {{"kind", "texture"}, {"path", path}};
    }

    Finding makeFinding(const std::string& rule, const std::string& keyTarget, RuleStatus status, Json target, Json measured,
                        Json expected, std::string message){
        Finding finding;
        finding.rule = rule;
        finding.key = rule + ":" + keyTarget;
        finding.status = status;
        finding.target = std::move(target);
        finding.measured = std::move(measured);
        finding.expected = std::move(expected);
        finding.message = std::move(message);
        return finding;
    }

    FixOption option(std::string id, std::string label, std::vector<FixChange> changes){
        FixOption out;
        out.id = std::move(id);
        out.label = std::move(label);
        out.changes = std::move(changes);
        return out;
    }

    std::string sizeText(int width, int height){
        return std::to_string(width) + "x" + std::to_string(height);
    }

    std::string formatDouble(double value){
        return textureCanonicalNumber((float)value);
    }

    bool nearInteger(double value, double tolerance){
        return std::fabs(value - std::round(value)) <= tolerance;
    }

    int projectPpu(const PixelArtCheckInput& input){
        return std::max(1, input.project.pixelsPerUnit);
    }

    double drawablePpu(const PixelArtCheckInput& input, const PixelArtDrawable& drawable){
        return drawable.pixelsPerUnit > 0.0f ? (double)drawable.pixelsPerUnit : (double)projectPpu(input);
    }

    const PixelArtTexture* textureOf(const PixelArtCheckInput& input, const std::string& path){
        auto it = input.textures.find(path);
        return it == input.textures.end() ? nullptr : &it->second;
    }

    // the world rotation of a drawable about Z, and whether it is tilted about X or Y (90-degree
    // multiples of the tilt are flips, 0 or 180)
    struct Angles {
        double z = 0, x = 0, y = 0;
    };
    Angles anglesOf(const Quaternion& rotation){
        Quaternion q = rotation;
        q.normalize();
        // the rotated x axis gives the drawn angle (what S41's 90-degree rounding reads)
        const Vector3 axisX = q * Vector3(1, 0, 0);
        const Vector3 axisY = q * Vector3(0, 1, 0);
        Angles angles;
        angles.z = std::atan2((double)axisX.y, (double)axisX.x) * kDegrees;
        // tilt: how far the axes leave the XY plane
        angles.y = std::asin(std::clamp((double)-axisX.z, -1.0, 1.0)) * kDegrees;
        angles.x = std::asin(std::clamp((double)axisY.z, -1.0, 1.0)) * kDegrees;
        return angles;
    }

    double nearestStep(double angle, double step){
        return std::round(angle / step) * step;
    }

    bool onStep(double angle, double step){
        return std::fabs(angle - nearestStep(angle, step)) <= kAngleTolerance;
    }

    double cleanAngle(double degrees){
        double a = std::fmod(degrees, 360.0);
        if (a > 180.0) a -= 360.0;
        if (a <= -180.0) a += 360.0;
        if (std::fabs(a) < 1e-6) a = 0.0;
        return a;
    }

    Json vec3(const Vector3& v){
        return Json::array({number(v.x), number(v.y), number(v.z)});
    }

    // a rotation's Euler angles as the Inspector and transform.rotation's "euler" show them
    // (McpValues::eulerDegrees): tiny values 0, near-whole angles snapped
    Vector3 eulerOf(const Quaternion& rotation){
        Vector3 euler = Quaternion(rotation).normalize().getEulerAngles(RotationOrder::ZYX);
        float* parts[3] = {&euler.x, &euler.y, &euler.z};
        for (float* part : parts){
            if (std::fabs(*part) < 1e-4f) *part = 0.0f;
            const float rounded = std::round(*part * 1000.0f) / 1000.0f;
            *part = rounded == 0.0f ? 0.0f : rounded;
        }
        return euler;
    }

    // the scene's own camera, or none (its default view)
    bool hasCameraComponent(const PixelArtScene& scene){
        return scene.camera.exists && scene.camera.hasComponent && scene.camera.component.enabled && !scene.camera.perspective;
    }

    struct ScreenLayout {
        PixelArtScreen screen;
        double scale = 1;
        bool integer = true;
        int barX = 0, barY = 0;
    };

    std::string layoutMode(const PixelArtCheckInput& input, const PixelArtScene& scene){
        (void)input;
        if (hasCameraComponent(scene))
            return "pixel_perfect_camera";
        if (scene.fixedResolution && scene.fixedWidth > 0 && scene.fixedHeight > 0)
            return "fixed_resolution";
        return "scaling";
    }

    std::vector<ScreenLayout> screenLayouts(const PixelArtCheckInput& input, const PixelArtScene& scene){
        std::vector<ScreenLayout> out;
        const PixelArtProjectInfo& p = input.project;
        const std::string mode = layoutMode(input, scene);
        for (const PixelArtScreen& screen : p.screens){
            ScreenLayout layout;
            layout.screen = screen;
            if (mode == "pixel_perfect_camera"){
                const PixelArtCameraSettings& c = scene.camera.component;
                const int refW = c.referenceWidth > 0 ? (int)c.referenceWidth : p.canvasWidth;
                const int refH = c.referenceHeight > 0 ? (int)c.referenceHeight : p.canvasHeight;
                const jasperengine::pixelart::PixelPerfectLayout l = jasperengine::pixelart::computePixelPerfectLayout(
                    screen.width, screen.height, refW, refH, c.upscaleMode, 0, c.subpixelSmoothing, projectPpu(input));
                const int baseW = std::max(1, l.baseWidth);
                layout.scale = c.upscaleMode == PixelUpscaleMode::STRETCH_FILL ? (double)l.viewport.getWidth() / baseW : (double)l.scale;
                layout.integer = c.upscaleMode != PixelUpscaleMode::STRETCH_FILL || nearInteger(layout.scale, kTexelTolerance);
                layout.barX = (int)std::lround(l.viewport.getX());
                layout.barY = (int)std::lround(l.viewport.getY());
            }else{
                int canvasW = 0, canvasH = 0;
                Rect view;
                Engine::computeViewLayout(screen.width, screen.height, p.canvasWidth, p.canvasHeight, p.scaling, canvasW, canvasH, view);
                double sx = 1, sy = 1;
                if (mode == "fixed_resolution"){
                    // the fixed-resolution target is drawn into the view rect
                    sx = (double)view.getWidth() / scene.fixedWidth;
                    sy = (double)view.getHeight() / scene.fixedHeight;
                }else if (canvasW > 0 && canvasH > 0){
                    sx = (double)view.getWidth() / canvasW;
                    sy = (double)view.getHeight() / canvasH;
                }
                layout.scale = std::min(sx, sy);
                layout.integer = nearInteger(sx, kTexelTolerance) && nearInteger(sy, kTexelTolerance) && std::fabs(sx - sy) <= kTexelTolerance;
                layout.barX = (int)std::lround(view.getX());
                layout.barY = (int)std::lround(view.getY());
            }
            out.push_back(layout);
        }
        return out;
    }

    Json layoutsJson(const std::vector<ScreenLayout>& layouts){
        Json out = Json::object();
        for (const ScreenLayout& l : layouts){
            out[sizeText(l.screen.width, l.screen.height)] = {
                {"scale", number(l.scale)}, {"integer", l.integer}, {"bars", Json::array({l.barX, l.barY})}};
        }
        return out;
    }

    // the drawn quad's bottom-left corner in world units (the S41 snap anchor of an unrotated quad)
    Vector3 anchorWorld(const PixelArtCheckInput& input, const PixelArtDrawable& d){
        const double ppu = drawablePpu(input, d);
        const Vector3 offset((float)(-d.pivot.x / ppu), (float)(-d.pivot.y / ppu), 0.0f);
        const Vector3 scaled(offset.x * d.worldScale.x, offset.y * d.worldScale.y, 0.0f);
        return d.worldPosition + d.worldRotation * scaled;
    }

    // what a texture is called in a message: its file name
    std::string fileName(const std::string& path){
        const size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    // TextureFormatSelector's choice for one family of a resolved texture
    struct FamilyFormat {
        std::string name;
        bool lossy = false;
        bool mipmaps = false;
        int width = 0, height = 0;    // mip 0 after the size rules
        size_t vramBytes = 0;
    };
    FamilyFormat familyFormat(const PixelArtTexture& texture, const ResolvedTextureSettings& r){
        FamilyFormat out;
        int width = texture.width, height = texture.height;
        TexturePrepare::targetSize(std::max(1, width), std::max(1, height), r.processing(), out.width, out.height);
        TextureFormatInputs in;
        in.settings = r;
        in.hasAlpha = texture.hasAlpha;
        in.width = out.width;
        in.height = out.height;
        const TextureFormatChoice choice = TextureFormatSelector::select(in);
        if (choice.codec == TextureCodec::ETC1S) out.name = "ETC1S";
        else if (choice.codec == TextureCodec::UASTC) out.name = "UASTC";
        else if (choice.codec == TextureCodec::UASTC_HDR) out.name = "UASTC_HDR";
        else out.name = gpuFormatName(choice.format);
        out.lossy = choice.codec != TextureCodec::Raw || gpuFormatInfo(choice.format).compressed;
        out.mipmaps = r.mipmaps;
        const GpuFormatInfo& info = gpuFormatInfo(choice.format);
        int w = out.width, h = out.height;
        for (int level = 0;; level++){
            const size_t blocksX = (size_t)((w + info.blockDim - 1) / info.blockDim);
            const size_t blocksY = (size_t)((h + info.blockDim - 1) / info.blockDim);
            out.vramBytes += blocksX * blocksY * info.bytesPerBlock;
            if (!r.mipmaps || (w <= 1 && h <= 1))
                break;
            w = std::max(1, w / 2);
            h = std::max(1, h / 2);
        }
        return out;
    }

    std::string fileSha256(const std::filesystem::path& path){
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return std::string();
        std::ostringstream bytes;
        bytes << in.rdbuf();
        return sha256Hex(bytes.str());
    }

    bool isFontDrawn(const PixelArtFontInfo& info){
        return info.kind == "bitmap" && info.nativeSize > 0 && info.size > 0 && (int)info.size % info.nativeSize == 0 &&
               info.partialAlphaShare <= 0.0f && info.vectorFallbacks.empty() && !info.loadError;
    }

}

const char* jasperengine::editor::pixelart::rules::filterName(TextureFilter filter){
    switch (filter){
        case TextureFilter::NEAREST: return "nearest";
        case TextureFilter::NEAREST_MIPMAP_NEAREST: return "nearest_mipmap_nearest";
        case TextureFilter::NEAREST_MIPMAP_LINEAR: return "nearest_mipmap_linear";
        case TextureFilter::LINEAR_MIPMAP_NEAREST: return "linear_mipmap_nearest";
        case TextureFilter::LINEAR_MIPMAP_LINEAR: return "linear_mipmap_linear";
        case TextureFilter::LINEAR: default: return "linear";
    }
}

const char* jasperengine::editor::pixelart::rules::scalingName(Scaling scaling){
    switch (scaling){
        case Scaling::FITHEIGHT: return "fit_height";
        case Scaling::LETTERBOX: return "letterbox";
        case Scaling::CROP: return "crop";
        case Scaling::STRETCH: return "stretch";
        case Scaling::NATIVE: return "native";
        case Scaling::INTEGER: return "integer";
        case Scaling::INTEGER_EXPAND: return "integer_expand";
        case Scaling::FITWIDTH: default: return "fit_width";
    }
}

Json jasperengine::editor::pixelart::rules::cameraJson(const PixelArtCheckInput& input, const PixelArtScene& scene){
    const PixelArtProjectInfo& p = input.project;
    Json out = {
        {"scene", scene.path},
        {"camera", scene.camera.exists ? Json(scene.camera.entity) : Json(nullptr)},
        {"has_component", hasCameraComponent(scene)},
        {"mode", layoutMode(input, scene)},
        {"layouts", layoutsJson(screenLayouts(input, scene))},
    };
    if (hasCameraComponent(scene)){
        const PixelArtCameraSettings& c = scene.camera.component;
        out["reference"] = Json::array({c.referenceWidth > 0 ? (int)c.referenceWidth : p.canvasWidth,
                                        c.referenceHeight > 0 ? (int)c.referenceHeight : p.canvasHeight});
        out["upscale_mode"] = pixelUpscaleModeName(c.upscaleMode);
    }else if (scene.fixedResolution){
        out["reference"] = Json::array({scene.fixedWidth, scene.fixedHeight});
    }else{
        out["reference"] = Json::array({p.canvasWidth, p.canvasHeight});
    }
    return out;
}

std::vector<FixChange> jasperengine::editor::pixelart::rules::projectPresetChanges(const PixelArtProjectInfo& p, int baseWidth, int baseHeight,
                                                                                 Scaling scaling, int pixelsPerUnit, int windowScale){
    std::vector<FixChange> out;
    auto change = [&](const char* key, Json from, Json to){
        if (from != to)
            out.push_back({"project", key, std::move(from), std::move(to)});
    };
    change("pixel_art.enabled", p.modeEnabled, true);
    change("graphics.canvas_width", p.canvasWidth, baseWidth);
    change("graphics.canvas_height", p.canvasHeight, baseHeight);
    change("graphics.scaling_mode", scalingName(p.scaling), scalingName(scaling));
    change("graphics.pixels_per_unit", p.pixelsPerUnit, pixelsPerUnit);
    change("graphics.default_filter_mode", textureFilterModeName(p.defaultFilterMode), "point");
    // windowScale 0: keep a window that already shows the base at one whole scale, else 4x
    int scale = windowScale;
    if (scale <= 0){
        const bool multiple = baseWidth > 0 && baseHeight > 0 && p.windowWidth % baseWidth == 0 && p.windowHeight % baseHeight == 0 &&
                              p.windowWidth / baseWidth == p.windowHeight / baseHeight && p.windowWidth / baseWidth >= 1;
        scale = multiple ? p.windowWidth / baseWidth : 4;
    }
    change("player.window_width", p.windowWidth, baseWidth * scale);
    change("player.window_height", p.windowHeight, baseHeight * scale);
    return out;
}

std::vector<FixChange> jasperengine::editor::pixelart::rules::cameraFixChanges(const PixelArtCheckInput& input, const PixelArtScene& scene,
                                                                             std::string& label){
    if (scene.camera.exists && scene.camera.perspective)
        return {};
    const std::string& cameraEntity = scene.camera.entity;
    if (scene.camera.exists && scene.camera.hasComponent){
        label = "Enable the Pixel Perfect Camera on " + cameraEntity;
        return {{entityRef(scene.path, cameraEntity), "pixelperfectcamera.enabled", false, true}};
    }
    // reference = the fixed resolution if set, else the canvas (0)
    const int refW = scene.fixedResolution && scene.fixedWidth > 0 ? scene.fixedWidth : 0;
    const int refH = scene.fixedResolution && scene.fixedHeight > 0 ? scene.fixedHeight : 0;
    Json ppc = {{"referenceWidth", refW}, {"referenceHeight", refH}};
    if (input.project.upscaleMode != PixelUpscaleMode::INTEGER)
        ppc["upscaleMode"] = pixelUpscaleModeName(input.project.upscaleMode);
    if (scene.camera.exists){
        label = "Add a Pixel Perfect Camera to " + cameraEntity;
        return {{entityRef(scene.path, cameraEntity), "pixelperfectcamera", nullptr, ppc}};
    }
    label = "Add a Main Camera with a Pixel Perfect Camera to " + scene.name;
    return {{scene.path, "camera", nullptr, {{"name", "Main Camera"}, {"pixelperfectcamera", ppc}}}};
}

// ---------------------------------------------------------------- pixel_art_mode

RuleResult jasperengine::editor::pixelart::rules::pixelArtMode(const PixelArtCheckInput& input, std::vector<Finding>& findings){
    RuleResult result;
    const PixelArtProjectInfo& p = input.project;
    bool content = false;
    for (const auto& [path, texture] : input.textures)
        content = content || texture.pixelArt;
    for (const PixelArtScene& scene : input.scenes)
        content = content || scene.pixelArt || scene.drawsSprites;
    if (!content && !p.modeEnabled)
        return result;
    result.checked = 1;

    Json measured = Json::object();
    if (!p.modeEnabled)
        measured["pixel_art.enabled"] = false;
    if (!isPixelArtBase(p.canvasWidth, p.canvasHeight))
        measured["graphics.canvas"] = sizeText(p.canvasWidth, p.canvasHeight);
    const bool integerScaling = p.scaling == Scaling::INTEGER || p.scaling == Scaling::INTEGER_EXPAND;
    if (!integerScaling)
        measured["graphics.scaling_mode"] = scalingName(p.scaling);
    if (p.defaultFilterMode != TextureFilterMode::Point)
        measured["graphics.default_filter_mode"] = textureFilterModeName(p.defaultFilterMode);
    if (measured.empty())
        return result;

    const int baseW = isPixelArtBase(p.canvasWidth, p.canvasHeight) ? p.canvasWidth : 320;
    const int baseH = isPixelArtBase(p.canvasWidth, p.canvasHeight) ? p.canvasHeight : 180;
    Finding finding = makeFinding("pixel_art_mode", "project", RuleStatus::Warn, {{"kind", "project"}}, measured,
                                  {{"pixel_art.enabled", true}, {"graphics.canvas", "at most 640x360"},
                                   {"graphics.scaling_mode", "integer or integer_expand"}, {"graphics.default_filter_mode", "point"}},
                                  !p.modeEnabled ? "The project draws pixel art but Pixel Art Mode is off."
                                                 : "Pixel Art Mode is on but the project's base resolution, scaling or default filter is not a pixel-art layout.");
    finding.options.push_back(option("default", "Apply the Pixel Art preset's project settings (" + sizeText(baseW, baseH) + ")",
                                     projectPresetChanges(p, baseW, baseH, integerScaling ? p.scaling : Scaling::INTEGER, p.pixelsPerUnit, 0)));
    findings.push_back(std::move(finding));
    return result;
}

// ---------------------------------------------------------------- integer_scale

RuleResult jasperengine::editor::pixelart::rules::integerScale(const PixelArtCheckInput& input, std::vector<Finding>& findings){
    RuleResult result;
    for (const PixelArtScene& scene : input.scenes){
        if (!scene.pixelArt)
            continue;
        result.checked++;
        const std::vector<ScreenLayout> layouts = screenLayouts(input, scene);
        const bool component = hasCameraComponent(scene);
        bool fractional = false;
        for (const ScreenLayout& l : layouts)
            fractional = fractional || !l.integer;
        RuleStatus status = RuleStatus::Pass;
        std::string message;
        if (component && scene.camera.component.upscaleMode == PixelUpscaleMode::STRETCH_FILL){
            status = RuleStatus::Warn;
            message = "The Pixel Perfect Camera of " + scene.name + " uses Stretch Fill: its seams are soft by design.";
        }else if (fractional){
            status = RuleStatus::Fail;
            message = scene.name + " scales its pixels by a fraction on some screens: columns of art draw at uneven widths.";
        }else if (!component){
            // integer scaling alone draws whole scales, but nothing snaps to the base grid
            status = RuleStatus::Warn;
            message = scene.name + " has no Pixel Perfect Camera: sprites and the camera are not snapped to whole pixels.";
        }
        if (status == RuleStatus::Pass)
            continue;
        const std::string cameraEntity = scene.camera.exists ? scene.camera.entity : std::string();
        Json target = {{"kind", "scene"}, {"path", scene.path}};
        if (!cameraEntity.empty())
            target["entity"] = cameraEntity;
        Json measured = {{"mode", layoutMode(input, scene)}, {"screens", layoutsJson(layouts)}};
        Finding finding = makeFinding("integer_scale", scene.path, status, target, measured,
                                      {{"integer", true}, {"pixel_perfect_camera", true}}, message);
        if (component){
            finding.options.push_back(option("default", "Set the Pixel Perfect Camera's Upscale Mode to Integer",
                                             {{entityRef(scene.path, cameraEntity), "pixelperfectcamera.upscaleMode", "stretch_fill", "integer"}}));
        }else{
            std::string label;
            std::vector<FixChange> changes = cameraFixChanges(input, scene, label);
            if (changes.empty())
                finding.needsPerson = "The main camera of " + scene.name + " is perspective: a Pixel Perfect Camera needs an orthographic camera.";
            else
                finding.options.push_back(option("default", label, std::move(changes)));
        }
        findings.push_back(std::move(finding));
    }
    return result;
}

// ---------------------------------------------------------------- filtering

RuleResult jasperengine::editor::pixelart::rules::filtering(const PixelArtCheckInput& input, std::vector<Finding>& findings){
    RuleResult result;
    for (const PixelArtScene& scene : input.scenes){
        for (const PixelArtDrawable& d : scene.drawables){
            if (d.kind != DrawableKind::Sprite && d.kind != DrawableKind::Tilemap && d.kind != DrawableKind::Image)
                continue;
            if (d.texture.empty() && !d.sheet)
                continue;
            result.checked++;
            if (d.minFilter == TextureFilter::NEAREST && d.magFilter == TextureFilter::NEAREST)
                continue;
            const std::string shown = d.sheet ? fileName(d.sheetPath) : fileName(d.texture);
            Finding finding = makeFinding("filtering", entityRef(d.scene, d.entity) + ":" + d.textureField, RuleStatus::Fail,
                                          {{"kind", "reference"}, {"scene", d.scene}, {"entity", d.entity}, {"field", d.textureField},
                                           {"path", d.sheet ? d.sheetPath : d.texture}},
                                          {{"min_filter", filterName(d.minFilter)}, {"mag_filter", filterName(d.magFilter)}},
                                          {{"min_filter", "nearest"}, {"mag_filter", "nearest"}},
                                          shown + " is filtered " + std::string(d.magFilter == TextureFilter::NEAREST ? "with mipmaps" : "bilinear") +
                                              " on " + d.entity.substr(d.entity.find_last_of('/') + 1) + ".");
            finding.options.push_back(option("default", "Set both filters of " + d.entity + " to Nearest",
                                             {{entityRef(d.scene, d.entity), d.textureField,
                                               {{"min_filter", filterName(d.minFilter)}, {"mag_filter", filterName(d.magFilter)}},
                                               {{"min_filter", "nearest"}, {"mag_filter", "nearest"}}}}));
            findings.push_back(std::move(finding));
        }
    }
    return result;
}

// ---------------------------------------------------------------- mipmaps_compression

RuleResult jasperengine::editor::pixelart::rules::mipmapsCompression(const PixelArtCheckInput& input, std::vector<Finding>& findings){
    RuleResult result;
    for (const auto& [path, texture] : input.textures){
        if (!texture.pixelArt && !texture.paletteSwap)
            continue;
        if (!texture.sheetSource.empty())
            continue;   // a generated page: its sheet source is checked
        result.checked++;
        if (texture.aseprite || texture.atlas){
            const std::string compression = texture.aseprite ? texture.asepriteCompression : texture.atlasCompression;
            const bool lossless = texture.aseprite ? compression == "none" : (compression == "lossless" || compression == "auto");
            if (lossless)
                continue;
            Finding finding = makeFinding("mipmaps_compression", path, RuleStatus::Fail, textureTarget(path),
                                          {{"compression", compression}},
                                          {{"compression", texture.aseprite ? "none" : "lossless"}},
                                          fileName(path) + "'s sheet pages are block-compressed (" + compression + "): colours shift and edges smear.");
            if (texture.aseprite)
                finding.options.push_back(option("default", "Set the Aseprite importer's texture compression to none",
                                                 {{path, "import.asepriteImporter.texture.compression", compression, "none"}}));
            else
                finding.options.push_back(option("default", "Set the atlas's compression to Lossless",
                                                 {{path, "settings.compression", compression, "lossless"}}));
            findings.push_back(std::move(finding));
            continue;
        }
        const TextureProjectSettings& projectSettings = input.project.textureSettings;
        Json formats = Json::object();
        bool lossy = false, mipmaps = false, resized = false, softResize = false;
        size_t vram = 0;
        int resizedW = 0, resizedH = 0;
        MipFilter resizeFilter = MipFilter::Nearest;
        for (size_t f = 0; f < kTextureFamilyCount; f++){
            const TextureFamily family = (TextureFamily)f;
            const ResolvedTextureSettings r = texture.settings.resolve(family, projectSettings);
            const FamilyFormat ff = familyFormat(texture, r);
            formats[textureFamilyName(family)] = ff.name;
            lossy = lossy || ff.lossy;
            mipmaps = mipmaps || ff.mipmaps;
            if (texture.width > 0 && (ff.width != texture.width || ff.height != texture.height)){
                resized = true;
                resizedW = ff.width;
                resizedH = ff.height;
            }
            if (r.resizeFilter != MipFilter::Nearest){
                softResize = true;
                resizeFilter = r.resizeFilter;
            }
            if (family == TextureFamily::Desktop)
                vram = ff.vramBytes;
        }
        RuleStatus status = (lossy || mipmaps) ? RuleStatus::Fail : (resized || softResize) ? RuleStatus::Warn : RuleStatus::Pass;
        if (status == RuleStatus::Pass)
            continue;
        Json measured = {{"mipmaps", mipmaps}, {"format", formats}, {"resize_filter", mipFilterName(resizeFilter)},
                         {"resized_from", resized ? Json::array({texture.width, texture.height}) : Json(nullptr)},
                         {"vram_bytes", vram}};
        if (resized)
            measured["resized_to"] = Json::array({resizedW, resizedH});
        std::string why;
        if (lossy) why = "is block-compressed";
        if (mipmaps) why += std::string(why.empty() ? "" : " and ") + "has mipmaps";
        if (why.empty() && resized) why = "is resized to " + sizeText(resizedW, resizedH) + " (uneven pixels)";
        if (why.empty()) why = "resizes with " + std::string(mipFilterName(resizeFilter)) + ", not nearest";
        Finding finding = makeFinding("mipmaps_compression", path, status, textureTarget(path), measured,
                                      {{"mipmaps", false}, {"format", "RGBA8"}, {"resize_filter", "nearest"}, {"resized_from", nullptr}},
                                      fileName(path) + " " + why + ".");
        // Pixel Art on, the fields that contradict it cleared (a lossy override or explicit
        // mipmaps would still win), and a max size that keeps the image whole
        TextureImportSettings fixed = texture.settings;
        fixed.pixelArt = true;
        fixed.mipmaps.reset();
        fixed.mipFilter.reset();
        fixed.resizeFilter.reset();
        fixed.quality.reset();
        fixed.streamable.reset();
        fixed.alphaIsTransparency.reset();
        fixed.preserveCoverage.reset();
        if (fixed.type && *fixed.type != TextureImportType::Sprite && *fixed.type != TextureImportType::UI)
            fixed.type = TextureImportType::Sprite;
        const int side = std::max(texture.width, texture.height);
        const int projectMax = *std::min_element(projectSettings.defaultMaxSize.begin(), projectSettings.defaultMaxSize.end());
        if (side > 0 && (fixed.maxSize ? *fixed.maxSize < side : side > projectMax)){
            int size = 32;
            while (size < side && size < 16384) size *= 2;
            fixed.maxSize = size;
        }
        for (TexturePlatformSettings& platform : fixed.platforms){
            platform.format.reset();
            platform.quality.reset();
            platform.rdo.reset();
            if (platform.maxSize && side > 0 && *platform.maxSize < side)
                platform.maxSize.reset();
            if (!platform.maxSize && !platform.effort)
                platform.override = false;
        }
        const Json from = texture.hasSettings ? yamlJson(texture.settings.serialize()) : Json(nullptr);
        finding.options.push_back(option("default", "Turn on Pixel Art in " + fileName(path) + "'s import settings (lossless, no mipmaps)",
                                         {{path, "import.textureImporter", from, yamlJson(fixed.serialize())}}));
        findings.push_back(std::move(finding));
    }
    return result;
}

// ---------------------------------------------------------------- palette

RuleResult jasperengine::editor::pixelart::rules::palette(const PixelArtCheckInput& input, std::vector<Finding>& findings){
    RuleResult result;
    for (const auto& [path, texture] : input.textures){
        if (!texture.pixelArt || texture.aseprite || texture.atlas || !texture.sheetSource.empty() || !texture.exists)
            continue;
        const std::string palettePath = !texture.paletteSwapSource.empty() ? texture.paletteSwapSource : input.project.defaultPalette;
        if (palettePath.empty())
            continue;
        auto colors = input.palettes.find(palettePath);
        if (colors == input.palettes.end() || colors->second.empty())
            continue;
        result.checked++;
        std::string error;
        const std::optional<RgbaImage> image = loadRgbaImage(input.assetsRoot / path, &error);
        if (!image){
            Finding finding = makeFinding("palette", path, RuleStatus::Fail, textureTarget(path), {{"error", error}}, {{"verdict", "clean"}},
                                          fileName(path) + " cannot be read to measure its palette: " + error);
            finding.needsPerson = "the image does not decode";
            findings.push_back(std::move(finding));
            continue;
        }
        ApplyRequest request;
        request.palette = colors->second;
        const ApplyResult applied = applyPalette(*image, request);
        if (applied.verdict == Verdict::Clean)
            continue;
        Json lost = Json::array();
        for (const LostGroup& group : applied.lost)
            lost.push_back({{"count", group.count}, {"box", Json::array({group.x, group.y, group.w, group.h})}});
        Json measured = {{"palette", palettePath}, {"verdict", verdictName(applied.verdict)},
                         {"off_palette_pixels", applied.stats.pixelsChanged}, {"colors", applied.stats.colorsBefore},
                         {"max_delta_e", number(applied.stats.maxDeltaE)}, {"lost_groups", lost}};
        const bool person = applied.verdict == Verdict::NeedsPerson;
        Finding finding = makeFinding("palette", path, person ? RuleStatus::Fail : RuleStatus::Warn, textureTarget(path), measured,
                                      {{"verdict", "clean"}, {"palette", palettePath}},
                                      fileName(path) + " has " + std::to_string(applied.stats.pixelsChanged) + " pixels off the palette " +
                                          fileName(palettePath) + (person ? ", and remapping loses whole colour groups." : "."));
        if (person){
            finding.needsPerson = std::to_string(applied.lost.size()) + " colour group(s) would be lost to the palette: redraw them or extend the palette";
        }else{
            finding.options.push_back(option("default", "Apply the palette " + fileName(palettePath) + " to " + fileName(path),
                                             {{path, "pixels", fileSha256(input.assetsRoot / path), "palette " + palettePath}}));
        }
        findings.push_back(std::move(finding));
    }
    return result;
}

// ---------------------------------------------------------------- texel_size

RuleResult jasperengine::editor::pixelart::rules::texelSize(const PixelArtCheckInput& input, std::vector<Finding>& findings){
    RuleResult result;
    const double ppuProject = projectPpu(input);
    for (const PixelArtScene& scene : input.scenes){
        for (const PixelArtDrawable& d : scene.drawables){
            double x = 0, y = 0;
            if (d.kind == DrawableKind::Sprite || d.kind == DrawableKind::Tilemap){
                if (!d.hasTransform)
                    continue;
                const double ratio = ppuProject / drawablePpu(input, d);
                x = std::fabs((double)d.worldScale.x) * ratio;
                y = std::fabs((double)d.worldScale.y) * ratio;
            }else if (d.kind == DrawableKind::Image){
                const PixelArtTexture* texture = textureOf(input, d.texture);
                if (d.sliced || !texture || texture->width <= 0 || texture->height <= 0 || d.rectWidth <= 0 || d.rectHeight <= 0)
                    continue;
                x = d.rectWidth / texture->width * std::fabs((double)d.worldScale.x);
                y = d.rectHeight / texture->height * std::fabs((double)d.worldScale.y);
            }else{
                continue;
            }
            result.checked++;
            const bool one = std::fabs(x - 1.0) <= kTexelTolerance && std::fabs(y - 1.0) <= kTexelTolerance;
            if (one)
                continue;
            const bool big = nearInteger(x, kTexelTolerance) && x > 1.5 && std::fabs(x - y) <= kTexelTolerance;
            const RuleStatus status = big ? RuleStatus::Warn : RuleStatus::Fail;
            const std::string name = d.entity.substr(d.entity.find_last_of('/') + 1);
            Finding finding = makeFinding("texel_size", entityRef(d.scene, d.entity), status, entityTarget(d.scene, d.entity),
                                          {{"x", number(x)}, {"y", number(y)}}, {{"x", 1}, {"y", 1}},
                                          big ? name + " draws its pixels " + formatDouble(x) + "x bigger than the scene's (deliberate big pixels?)."
                                              : name + "'s pixels are " + formatDouble(x) + " x " + formatDouble(y) + " base pixels, not 1 x 1.");
            if (d.kind == DrawableKind::Image){
                const PixelArtTexture* texture = textureOf(input, d.texture);
                const double sx = std::fabs((double)d.worldScale.x), sy = std::fabs((double)d.worldScale.y);
                if (std::fabs(sx - 1.0) <= kTexelTolerance && std::fabs(sy - 1.0) <= kTexelTolerance){
                    finding.options.push_back(option("default", "Size " + name + " to its texture (" + sizeText(texture->width, texture->height) + ")",
                                                     {{entityRef(d.scene, d.entity), "recttransform.width", number(d.rectWidth), texture->width},
                                                      {entityRef(d.scene, d.entity), "recttransform.height", number(d.rectHeight), texture->height}}));
                }else{
                    finding.needsPerson = name + " is scaled by its parents: size it to its texture where it is drawn";
                }
            }else{
                const double ratio = ppuProject / drawablePpu(input, d);
                const double parentX = std::fabs((double)d.parentWorldScale.x), parentY = std::fabs((double)d.parentWorldScale.y);
                const bool scaleOne = std::fabs((double)std::fabs(d.worldScale.x) - 1.0) <= kTexelTolerance &&
                                      std::fabs((double)std::fabs(d.worldScale.y) - 1.0) <= kTexelTolerance;
                if (std::fabs(ratio - 1.0) <= kTexelTolerance && parentX > 0 && parentY > 0){
                    // the local scale whose world scale is +-1, the signs (flips) kept
                    const float newX = (float)((d.scale.x < 0 ? -1.0 : 1.0) / parentX);
                    const float newY = (float)((d.scale.y < 0 ? -1.0 : 1.0) / parentY);
                    const Vector3 to(newX, newY, d.scale.z);
                    finding.options.push_back(option("default", "Scale " + name + " to " + (parentX == 1.0 && parentY == 1.0 ? "1" : "its parent's inverse"),
                                                     {{entityRef(d.scene, d.entity), "transform.scale", vec3(d.scale), vec3(to)}}));
                }else if (scaleOne && d.pixelsPerUnit > 0.0f){
                    finding.options.push_back(option("default", "Use the project's Pixels Per Unit on " + name,
                                                     {{entityRef(d.scene, d.entity), std::string(d.kind == DrawableKind::Tilemap ? "tilemap" : "sprite") + ".pixelsPerUnit",
                                                       number(d.pixelsPerUnit), 0}}));
                }else{
                    finding.needsPerson = "the art itself is not one texel per pixel at this size: fix the art or its Pixels Per Unit, never the scale";
                }
            }
            findings.push_back(std::move(finding));
        }
    }
    return result;
}

// ---------------------------------------------------------------- off_grid

RuleResult jasperengine::editor::pixelart::rules::offGrid(const PixelArtCheckInput& input, std::vector<Finding>& findings){
    RuleResult result;
    const int ppu = projectPpu(input);
    for (const PixelArtScene& scene : input.scenes){
        for (const PixelArtDrawable& d : scene.drawables){
            if ((d.kind != DrawableKind::Sprite && d.kind != DrawableKind::Tilemap) || !d.hasTransform)
                continue;
            result.checked++;
            const Vector3 anchor = anchorWorld(input, d);
            const double px = (double)anchor.x * ppu, py = (double)anchor.y * ppu;
            const double fx = px - std::round(px), fy = py - std::round(py);
            if (std::fabs(fx) <= kGridTolerance && std::fabs(fy) <= kGridTolerance)
                continue;
            // S41's snap: the anchor moves to the pixel the renderer draws it on
            const double snappedX = jasperengine::pixelart::snap((float)px);
            const double snappedY = jasperengine::pixelart::snap((float)py);
            const double deltaWorldX = (snappedX - px) / ppu, deltaWorldY = (snappedY - py) / ppu;
            // a world delta in the parent's space: the parent's rotation undone, its scale divided out
            const Vector3 worldDelta((float)deltaWorldX, (float)deltaWorldY, 0.0f);
            Quaternion inverse = d.parentWorldRotation;
            inverse = inverse.inverse();
            Vector3 local = inverse * worldDelta;
            local.x = d.parentWorldScale.x != 0.0f ? local.x / d.parentWorldScale.x : local.x;
            local.y = d.parentWorldScale.y != 0.0f ? local.y / d.parentWorldScale.y : local.y;
            local.z = 0.0f;
            const Vector3 to = d.position + local;
            const std::string name = d.entity.substr(d.entity.find_last_of('/') + 1);
            Finding finding = makeFinding("off_grid", entityRef(d.scene, d.entity), RuleStatus::Warn, entityTarget(d.scene, d.entity),
                                          {{"anchor", Json::array({number(px), number(py)})}, {"fraction", Json::array({number(fx), number(fy)})}},
                                          {{"fraction", Json::array({0, 0})}},
                                          name + " sits at " + formatDouble(px) + ", " + formatDouble(py) +
                                              " base pixels: drawn snapped, so ties flicker in motion.");
            finding.options.push_back(option("default", "Move " + name + " onto whole pixels",
                                             {{entityRef(d.scene, d.entity), "transform.position", vec3(d.position), vec3(to)}}));
            findings.push_back(std::move(finding));
        }
    }
    return result;
}

// ---------------------------------------------------------------- rotation

RuleResult jasperengine::editor::pixelart::rules::rotation(const PixelArtCheckInput& input, std::vector<Finding>& findings){
    RuleResult result;
    for (const PixelArtScene& scene : input.scenes){
        if (scene.pixelArt && scene.camera.exists){
            result.checked++;
            const double roll = cleanAngle(scene.camera.roll);
            if (std::fabs(roll) > kAngleTolerance){
                Vector3 to = scene.camera.rotationEuler;
                to.z = 0.0f;
                Finding finding = makeFinding("rotation", entityRef(scene.path, scene.camera.entity), RuleStatus::Warn,
                                              entityTarget(scene.path, scene.camera.entity), {{"camera_roll", number(roll)}}, {{"camera_roll", 0}},
                                              "The camera of " + scene.name + " is rolled " + formatDouble(roll) +
                                                  " degrees: its draws are not grid-aligned, so only pivots snap.");
                finding.options.push_back(option("default", "Set the camera's roll to 0",
                                                 {{entityRef(scene.path, scene.camera.entity), "transform.rotation",
                                                   {{"euler", vec3(scene.camera.rotationEuler)}}, {{"euler", vec3(to)}}}}));
                findings.push_back(std::move(finding));
            }
        }
        for (const PixelArtDrawable& d : scene.drawables){
            if ((d.kind != DrawableKind::Sprite && d.kind != DrawableKind::Tilemap && d.kind != DrawableKind::Text) || !d.hasTransform)
                continue;
            result.checked++;
            const Angles world = anglesOf(d.worldRotation);
            const double z = cleanAngle(world.z);
            const bool flatX = onStep(world.x, 180.0), flatY = onStep(world.y, 180.0);
            const PixelRotation policy = effectiveRotation(d, scene.camera);
            const bool onGridTarget = policy == PixelRotation::ON_GRID && hasCameraComponent(scene) && scene.camera.component.upscaleRenderTexture;
            if ((onStep(z, 90.0) && flatX && flatY) || onGridTarget)
                continue;
            const std::string name = d.entity.substr(d.entity.find_last_of('/') + 1);
            // the step S41 draws: the world angle rounded to 90 degrees, ties towards 0
            const double drawn = cleanAngle(nearestStep(z, 90.0));
            // only Z of the current Euler angles moves, so X/Y flips stay; a parent flipped over
            // turns a local Z rotation the other way on screen
            const Vector3 euler = eulerOf(d.rotation);
            const double sign = (Quaternion(d.parentWorldRotation).normalize() * Vector3(0, 0, 1)).z < 0.0f ? -1.0 : 1.0;
            Vector3 toEuler = euler;
            toEuler.z = (float)cleanAngle((double)euler.z + sign * (drawn - z));
            Json measured = {{"z", number(z)}, {"x", number(cleanAngle(world.x))}, {"y", number(cleanAngle(world.y))},
                             {"pixel_rotation", policy == PixelRotation::ON_GRID ? "on_grid" : "steps_90"}};
            Finding finding = makeFinding("rotation", entityRef(d.scene, d.entity), RuleStatus::Warn, entityTarget(d.scene, d.entity),
                                          measured, {{"z", "a multiple of 90"}, {"x", 0}, {"y", 0}},
                                          name + " is rotated " + formatDouble(z) + " degrees: " +
                                              (policy == PixelRotation::ON_GRID ? "it rotates at screen resolution, off the grid."
                                                                                : "it is drawn rounded to " + formatDouble(drawn) + " degrees."));
            finding.options.push_back(option("default", "Round " + name + "'s rotation to " + formatDouble(drawn) + " degrees",
                                             {{entityRef(d.scene, d.entity), "transform.rotation", {{"euler", vec3(euler)}},
                                               {{"euler", vec3(toEuler)}}}}));
            if (d.kind == DrawableKind::Sprite && hasCameraComponent(scene) && scene.camera.component.upscaleRenderTexture && flatX && flatY &&
                d.pixelRotation != PixelRotation::ON_GRID){
                finding.options.push_back(option("on_grid", "Rotate " + name + " on the grid (Pixel Rotation: Rotate On Grid)",
                                                 {{entityRef(d.scene, d.entity), "sprite.pixelRotation",
                                                   d.pixelRotation == PixelRotation::STEPS_90 ? "steps_90" : "camera", "on_grid"}}));
            }
            findings.push_back(std::move(finding));
        }
    }
    return result;
}

// ---------------------------------------------------------------- font_antialiasing

RuleResult jasperengine::editor::pixelart::rules::fontAntialiasing(const PixelArtCheckInput& input, std::vector<Finding>& findings){
    RuleResult result;
    auto check = [&](const std::string& keyTarget, Json target, const std::string& font, unsigned int size, const std::string& where){
        result.checked++;
        const PixelArtFontInfo info = analyzeFont(input, font, size);
        if (isFontDrawn(info))
            return;
        Json measured = {{"font", font.empty() ? Json("default") : Json(font)}, {"kind", info.kind}, {"size", info.size},
                         {"native_size", info.nativeSize > 0 ? Json(info.nativeSize) : Json(nullptr)},
                         {"pixel_scale", info.pixelScale > 0 ? number(info.pixelScale) : Json(nullptr)},
                         {"antialiased", info.antialiased},
                         {"partial_alpha_share", info.partialAlphaShare >= 0 ? number(info.partialAlphaShare) : Json(nullptr)},
                         {"vector_fallbacks", info.vectorFallbacks}, {"pixel_grid_detected", info.pixelGridDetected}};
        const RuleStatus status = info.loadError ? RuleStatus::Fail : RuleStatus::Warn;
        std::string message = where + (info.loadError ? " uses a bitmap font that does not load: " + info.error
                                       : info.kind == "vector" ? " draws a vector font: its glyphs are antialiased."
                                                               : " draws its bitmap font off its native size or with partial alpha.");
        Finding finding = makeFinding("font_antialiasing", keyTarget, status, target, measured,
                                      {{"kind", "bitmap"}, {"size", "a whole multiple of the native size"}, {"partial_alpha_share", 0}}, message);
        if (info.loadError){
            finding.needsPerson = "the bitmap font does not load: " + info.error;
        }else if (info.kind == "bitmap" && info.nativeSize > 0 && target.value("kind", "") == "entity" && info.vectorFallbacks.empty() &&
                  info.partialAlphaShare <= 0.0f){
            const int multiple = std::max(1, (int)std::lround((double)info.size / info.nativeSize));
            finding.options.push_back(option("default", "Set the font size to " + std::to_string(multiple * info.nativeSize),
                                             {{entityRef(target["scene"].get<std::string>(), target["entity"].get<std::string>()), "text.fontSize", info.size, multiple * info.nativeSize}}));
        }else if (info.kind == "bitmap" && info.partialAlphaShare > 0.0f && !font.empty()){
            finding.options.push_back(option("default", "Cut the bitmap font's alpha at 128", {{font, "alpha_cutoff", nullptr, 128}}));
        }else if (info.kind == "vector" && info.pixelGridDetected){
            finding.needsPerson = "create a .bitmapfont with source truetype from " + (font.empty() ? std::string("the font") : font);
        }else{
            finding.needsPerson = "use a pixel font (a .bitmapfont) at its native size";
        }
        findings.push_back(std::move(finding));
    };
    for (const PixelArtScene& scene : input.scenes){
        for (const PixelArtDrawable& d : scene.drawables){
            if (d.kind != DrawableKind::Text)
                continue;
            check(entityRef(d.scene, d.entity), entityTarget(d.scene, d.entity, "text.font"), d.font, d.fontSize,
                  d.entity.substr(d.entity.find_last_of('/') + 1));
        }
    }
    for (const std::string& font : input.fontPaths)
        check(font, {{"kind", "font"}, {"path", font}}, font, 0, fileName(font));
    return result;
}

// ---------------------------------------------------------------- particles_grid

RuleResult jasperengine::editor::pixelart::rules::particlesGrid(const PixelArtCheckInput& input, std::vector<Finding>& findings){
    RuleResult result;
    const int ppu = projectPpu(input);
    for (const PixelArtScene& scene : input.scenes){
        for (const PixelArtDrawable& d : scene.drawables){
            if (d.kind != DrawableKind::Particles)
                continue;
            result.checked++;
            // particle textures are made at runtime: the project's Default Filter Mode samples them
            const bool linear = !d.texture.empty() && input.project.defaultFilterMode != TextureFilterMode::Point;
            const bool stretched = d.particleRenderMode == "stretched_billboard" || d.particleRenderMode == "velocity_aligned";
            Json sizes = Json::array();
            bool fractionalSize = false;
            for (float size : d.particleSizes){
                const double pixels = (double)size * ppu;
                sizes.push_back(number(pixels));
                fractionalSize = fractionalSize || !nearInteger(pixels, kGridTolerance);
            }
            const bool snapping = hasCameraComponent(scene) && scene.camera.component.pixelSnapping;
            const bool perspective = scene.camera.exists && scene.camera.perspective;
            if (!linear && !stretched && !fractionalSize && snapping && !perspective)
                continue;
            Json measured = {{"texture_filter", d.texture.empty() ? "none" : linear ? "linear" : "nearest"},
                             {"render_mode", d.particleRenderMode}, {"sizes_px", sizes}, {"snapping", snapping},
                             {"camera", perspective ? "perspective" : "ortho"}};
            const std::string name = d.entity.substr(d.entity.find_last_of('/') + 1);
            std::vector<std::string> problems;
            if (linear) problems.push_back("a linear texture filter");
            if (stretched) problems.push_back(d.particleRenderMode + " particles");
            if (fractionalSize) problems.push_back("sizes that are not whole pixels");
            if (!snapping && !perspective) problems.push_back("Pixel Snapping off on the camera");
            if (perspective) problems.push_back("a perspective camera");
            std::string list;
            for (size_t i = 0; i < problems.size(); i++)
                list += (i == 0 ? "" : i + 1 == problems.size() ? " and " : ", ") + problems[i];
            Finding finding = makeFinding("particles_grid", entityRef(d.scene, d.entity), linear ? RuleStatus::Fail : RuleStatus::Warn,
                                          entityTarget(d.scene, d.entity), measured,
                                          {{"texture_filter", "nearest"}, {"render_mode", "billboard"}, {"sizes_px", "whole pixels"}, {"snapping", true}},
                                          name + " is off the pixel grid: " + list + ".");
            std::vector<FixChange> changes;
            std::vector<std::string> labels;
            if (linear){
                changes.push_back({"project", "graphics.default_filter_mode", textureFilterModeName(input.project.defaultFilterMode), "point"});
                labels.push_back("sample runtime textures with Point filtering (Default Filter Mode)");
            }
            if (!snapping && !perspective){
                // no enabled Pixel Perfect Camera: integer_scale's change (the same one, applied once)
                if (!hasCameraComponent(scene)){
                    std::string label;
                    const std::vector<FixChange> camera = cameraFixChanges(input, scene, label);
                    changes.insert(changes.end(), camera.begin(), camera.end());
                    labels.push_back(label);
                }
                if (scene.camera.exists && scene.camera.hasComponent && !scene.camera.component.pixelSnapping){
                    changes.push_back({entityRef(scene.path, scene.camera.entity), "pixelperfectcamera.pixelSnapping", false, true});
                    labels.push_back("turn on the camera's Pixel Snapping");
                }
            }
            if (stretched || fractionalSize || perspective){
                finding.needsPerson = perspective ? "a perspective camera has no world pixel grid"
                                    : stretched   ? "set the Renderer module's Render Mode to Billboard (set_properties on the effect)"
                                                  : "give the Start Size whole pixels (" + std::to_string(ppu) + " per unit)";
            }else if (!changes.empty()){
                std::string label;
                for (size_t i = 0; i < labels.size(); i++)
                    label += (i == 0 ? "" : ", ") + labels[i];
                label[0] = (char)std::toupper((unsigned char)label[0]);
                finding.options.push_back(option("default", label, changes));
            }
            findings.push_back(std::move(finding));
        }
    }
    return result;
}

// ---------------------------------------------------------------- anti_aliasing

RuleResult jasperengine::editor::pixelart::rules::antiAliasing(const PixelArtCheckInput& input, std::vector<Finding>& findings){
    RuleResult result;
    (void)input;
    for (const PixelArtScene& scene : input.scenes){
        if (!scene.pixelArt)
            continue;
        result.checked++;
        if (!scene.fxaa && scene.msaaSamples <= 1 && !scene.blur)
            continue;
        Json effects = Json::array();
        if (scene.blur) effects.push_back("blur");
        std::vector<FixChange> changes;
        if (scene.fxaa) changes.push_back({scene.path, "post_processing.fxaa_enabled", true, false});
        if (scene.msaaSamples > 1) changes.push_back({scene.path, "post_processing.msaa_samples", scene.msaaSamples, 1});
        if (scene.blur) changes.push_back({scene.path, "post_processing.blur_enabled", true, false});
        std::string what = scene.fxaa ? "FXAA" : scene.msaaSamples > 1 ? "MSAA " + std::to_string(scene.msaaSamples) + "x" : "a full-frame blur";
        Finding finding = makeFinding("anti_aliasing", scene.path, RuleStatus::Warn, {{"kind", "scene"}, {"path", scene.path}},
                                      {{"fxaa", scene.fxaa}, {"msaa_samples", scene.msaaSamples}, {"smoothing_effects", effects}},
                                      {{"fxaa", false}, {"msaa_samples", 1}, {"smoothing_effects", Json::array()}},
                                      scene.name + " smooths the pixel-art image with " + what + ".");
        finding.options.push_back(option("default", "Turn off the smoothing (FXAA off, MSAA 1)", changes));
        findings.push_back(std::move(finding));
    }
    return result;
}

// ---------------------------------------------------------------- sprite_variants

RuleResult jasperengine::editor::pixelart::rules::spriteVariants(const PixelArtCheckInput& input, std::vector<Finding>& findings){
    constexpr size_t kVariantBudget = 24;
    RuleResult result;
    std::set<std::string> all;
    std::map<std::string, std::set<std::string>> byMaterial;
    for (const PixelArtScene& scene : input.scenes){
        for (const auto& [material, variants] : scene.spriteVariants){
            byMaterial[material].insert(variants.begin(), variants.end());
            all.insert(variants.begin(), variants.end());
        }
    }
    if (byMaterial.empty())
        return result;
    result.checked = (int)byMaterial.size();
    if (all.size() <= kVariantBudget)
        return result;
    std::vector<std::pair<std::string, size_t>> ranked;
    for (const auto& [material, variants] : byMaterial)
        ranked.push_back({material, variants.size()});
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b){ return a.second > b.second; });
    Json list = Json::array();
    for (const auto& [material, count] : ranked)
        list.push_back({{"material", material}, {"variants", count}});
    Finding finding = makeFinding("sprite_variants", "project", RuleStatus::Warn, {{"kind", "project"}},
                                  {{"variants", all.size()}, {"by_material", list}}, {{"variants", "at most 24"}},
                                  "library:sprite needs " + std::to_string(all.size()) + " shader variants (budget 24); " + ranked.front().first +
                                      " adds the most.");
    finding.needsPerson = "merge feature sets or record the warm-up list";
    findings.push_back(std::move(finding));
    return result;
}
