// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LibraryChecks.h"

#include "TitleLibrary.h"
#include "../TitleArtBake.h"
#include "titleart/ColorScheme.h"
#include "titleart/effects/TitleFx.h"
#include "titleart/render/TitleArtRenderer.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titlelibrary;

namespace {

    using Out = std::vector<TitleCheckResult>;

    constexpr double PI = 3.14159265358979323846;
    // ramp samples the colour match reads (dense: a ramp's narrow bands count)
    constexpr int RAMP_SAMPLES = 65;
    // two ramp samples this close in OKLab L are candidates for the same face pixel
    constexpr double L_MATCH_SLACK = 0.02;

    std::string str(const Json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    double num(const Json& object, const char* key, double fallback) {
        auto it = object.find(key);
        return it != object.end() && it->is_number() ? it->get<double>() : fallback;
    }

    bool flag(const Json& object, const char* key, bool fallback) {
        auto it = object.find(key);
        return it != object.end() && it->is_boolean() ? it->get<bool>() : fallback;
    }

    const Json& member(const Json& object, const char* key) {
        static const Json none = Json::object();
        auto it = object.find(key);
        return it != object.end() && it->is_object() ? *it : none;
    }

    double round2(double v) {
        return std::round(v * 100.0) / 100.0;
    }

    double srgbToLinear(double c) {
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    }

    double linearToSrgb(double c) {
        return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    }

    double labF(double t) {
        return t > 216.0 / 24389.0 ? std::cbrt(t) : ((24389.0 / 27.0) * t + 16.0) / 116.0;
    }

    // CIE L* (D65) of an sRGB-encoded colour
    double lstar(double r, double g, double b) {
        const double y = 0.2126 * srgbToLinear(r) + 0.7152 * srgbToLinear(g) + 0.0722 * srgbToLinear(b);
        return 116.0 * labF(y) - 16.0;
    }

    bool whiteCore(double r, double g, double b) {
        return std::min({r, g, b}) >= COLOR_MATCH_WHITE_CORE;
    }

    double wrap360(double h) {
        h = std::fmod(h, 360.0);
        return h < 0.0 ? h + 360.0 : h;
    }

    // signed x - y on the circle, -180..180
    double hueDelta(double x, double y) {
        double d = wrap360(x - y);
        return d > 180.0 ? d - 360.0 : d;
    }

    struct RampSample {
        Oklch c;
        bool white = false;
    };

    std::vector<RampSample> rampOf(const Json& ramp) {
        std::vector<RampSample> out;
        auto add = [&](double r, double g, double b) { out.push_back({toOklch(r, g, b), whiteCore(r, g, b)}); };
        titleart::TitleColor color;
        if (ramp.is_string() && titleart::parseTitleColor(ramp, color)) {
            add(color.r, color.g, color.b);
            return out;
        }
        if (!ramp.is_object()) return out;
        const titleart::TitleGradient gradient = titleart::decodeGradient(ramp);
        for (int i = 0; i < RAMP_SAMPLES; i++) {
            float c[4];
            titleart::fx::sampleGradient(gradient, static_cast<double>(i) / (RAMP_SAMPLES - 1), c);
            add(c[0], c[1], c[2]);
        }
        return out;
    }

    // ---- results ----

    TitleCheckResult result(TitleCheckStatus status, std::string message, std::string method) {
        TitleCheckResult r;
        r.status = status;
        r.message = std::move(message);
        r.method = std::move(method);
        return r;
    }

    std::string layerRef(const std::string& id) {
        return "id:" + id;
    }

    std::string lookLabel(const Json& layer) {
        const std::string look = str(member(layer, "look"), "id");
        return look.empty() ? "own style" : "look " + look;
    }

    // ---- style_lint ----

    void styleLint(TitleCheckContext& ctx, Out& out) {
        const std::vector<StyleLintFinding> findings = lintDocument(ctx.doc);
        const char* method = "StyleLint (S84 §6.14.1): 9 samples per element, CIE L*a*b* (D65); rings face then rims or bands outward";
        if (findings.empty()) {
            size_t styled = 0;
            titleart::forEachLayer(ctx.doc.root, [&](const Json& layer, const Json*, int) {
                styled += titleart::layerTypeHasStyle(titleart::layerTypeOf(layer)) ? 1 : 0;
                return true;
            });
            out.push_back(result(TitleCheckStatus::Pass, "no style lint finding on " + std::to_string(styled) + " styled layer(s)", method));
            return;
        }
        for (const StyleLintFinding& f : findings) {
            const Json* layer = titleart::findLayer(ctx.doc.root, f.layer);
            const std::string name = layer ? str(*layer, "name") : f.layer;
            const std::string look = layer ? str(member(*layer, "look"), "id") : std::string();
            TitleCheckResult r = result(styleLintStatus(f),
                                        name + " (" + (layer ? lookLabel(*layer) : std::string("own style")) + ")" +
                                            (f.element.empty() ? std::string() : ", " + f.element) + ": " + f.message + ". " + styleLintRuleText(f.rule),
                                        method);
            r.id = "style_lint." + f.rule;
            r.measured = Json{{"value", round2(f.measured)}, {"element", f.element}, {"layer", f.layer}, {"layer_name", name}, {"look", look}};
            r.threshold = Json{{"value", f.threshold}};
            r.fix = f.fix;
            out.push_back(std::move(r));
        }
    }

    // ---- the face's pixels ----

    bool is3D(const Json& layer) {
        auto style = layer.find("style_3d");
        return str(layer, "dimension") == "3d" && style != layer.end() && style->is_object();
    }

    // The face of one layer, alone: coverage from a render without rims (3D: bands), layer effects
    // and part effects, colour from a render without rims or bands but with every effect (overlays
    // and inner glows tint the face; glows and shadows outside it fall outside the coverage)
    class FaceRenders {
    public:
        explicit FaceRenders(TitleCheckContext& context) : ctx(context) {
            snapshot = std::make_shared<titleart::TitleArtDoc>(context.doc);
            const int side = std::max(context.doc.canvasWidth(), context.doc.canvasHeight());
            // statistics of the face: a 1024 px canvas keeps thousands of pixels per letter
            scale = std::min(1.0f, 1024.0f / static_cast<float>(std::max(side, 1)));
        }

        const TitleCheckImage* face(const Json& layer, std::string& error) {
            const std::string id = str(layer, "id");
            auto it = cache.find(id);
            if (it != cache.end()) return &it->second;
            std::vector<titleart::FieldOverride> bare = {{id, "style.rims", Json::array()}};
            if (is3D(layer)) bare.push_back({id, "style_3d.bands", Json::array()});
            titleart::RenderRequest colour = request(id);
            colour.overrides = bare;
            titleart::RenderRequest coverage = request(id);
            coverage.overrides = bare;
            coverage.overrides.push_back({id, "effects", Json::array()});
            coverage.overrides.push_back({id, "part_effects", Json::object()});
            TitleCheckImage mask, tinted;
            if (!ctx.render(coverage, mask, error) || !ctx.render(colour, tinted, error)) return nullptr;
            if (tinted.width == mask.width && tinted.height == mask.height && tinted.frame == mask.frame) {
                for (size_t i = 0; i + 3 < mask.rgba.size(); i += 4) {
                    for (size_t k = 0; k < 3; k++) mask.rgba[i + k] = tinted.rgba[i + k];
                }
            }
            return &(cache[id] = std::move(mask));
        }

    private:
        titleart::RenderRequest request(const std::string& id) const {
            titleart::RenderRequest r;
            r.doc = snapshot;
            r.pxScale = scale;
            r.assets = ctx.assets;
            r.isolateLayer = id;
            return r;
        }

        TitleCheckContext& ctx;
        std::shared_ptr<titleart::TitleArtDoc> snapshot;
        float scale = 1.0f;
        std::map<std::string, TitleCheckImage> cache;
    };

    // one FaceRenders per check run: color_scheme_match and metal_range share the face renders
    FaceRenders& facesOf(TitleCheckContext& context) {
        static const TitleCheckContext* owner = nullptr;
        static std::string ownerHash;
        static std::unique_ptr<FaceRenders> renders;
        const std::string hash = titleArtSourceHash(context.doc);
        if (!renders || owner != &context || ownerHash != hash) {
            renders = std::make_unique<FaceRenders>(context);
            owner = &context;
            ownerHash = hash;
        }
        return *renders;
    }

    // the visible styled layers outside guides, in document order
    std::vector<const Json*> candidateLayers(const titleart::TitleArtDoc& doc) {
        std::vector<const Json*> out;
        std::set<std::string> skipped;
        titleart::forEachLayer(doc.root, [&](const Json& layer, const Json* parent, int) {
            if (flag(layer, "guide", false) || !flag(layer, "visible", true) || (parent && skipped.count(str(*parent, "id")))) {
                skipped.insert(str(layer, "id"));
                return true;
            }
            if (titleart::layerTypeHasStyle(titleart::layerTypeOf(layer))) out.push_back(&layer);
            return true;
        });
        return out;
    }

    std::shared_ptr<const titleart::ColorScheme> resolveScheme(const titleart::TitleArtDoc& doc, const std::string& ref, std::string& error) {
        LibraryError own;
        if (auto s = TitleLibrary::get().colorScheme(ref, &own)) return s;
        // a scheme the document carries (an exposed color_scheme parameter's data)
        const Json& embedded = member(doc.root, "color_schemes");
        if (embedded.contains(ref)) {
            auto s = std::make_shared<titleart::ColorScheme>();
            if (titleart::parseColorScheme(embedded[ref], *s, nullptr)) return s;
        }
        error = own.message.empty() ? "no Color Scheme '" + ref + "'" : own.message;
        return nullptr;
    }

    // ---- color_scheme_match ----

    void colorSchemeMatch(TitleCheckContext& ctx, Out& out) {
        const char* method = "the layer alone without rims (S84 §6.14.1): mean OKLCH hue and chroma of the face ink (alpha >= 0.5, white cores out) "
                             "against the scheme's face ramp: its hue span, its hue at the same lightness, its chroma";
        FaceRenders& faces = facesOf(ctx);
        size_t judged = 0;
        for (const Json* layer : candidateLayers(ctx.doc)) {
            const std::string ref = str(member(*layer, "look"), "color_scheme");
            if (ref.empty()) continue;
            judged++;
            const std::string id = str(*layer, "id"), name = str(*layer, "name");
            std::string error;
            auto scheme = resolveScheme(ctx.doc, ref, error);
            if (!scheme) {
                out.push_back(result(TitleCheckStatus::Skip, name + ": its Color Scheme does not resolve (" + error + "): not measured", method));
                continue;
            }
            const Json ramp = schemeFaceRamp(*scheme);
            if (ramp.is_null()) {
                out.push_back(result(TitleCheckStatus::Skip, name + ": Color Scheme " + ref + " has no face ramp: not measured", method));
                continue;
            }
            const TitleCheckImage* face = faces.face(*layer, error);
            if (!face) {
                out.push_back(result(TitleCheckStatus::Fail, "the render failed: " + error, "render"));
                return;
            }
            const ColorMatchMetric m = measureColorSchemeMatch(*face, ramp);
            TitleCheckResult r = result(TitleCheckStatus::Pass, "", method);
            r.measured = Json{{"layer", id},
                              {"layer_name", name},
                              {"color_scheme", ref},
                              {"pixels", m.pixels},
                              {"hue", round2(m.faceHue)},
                              {"chroma", std::round(m.faceChroma * 1000.0) / 1000.0},
                              {"ramp_hue_span", Json::array({round2(m.rampHueFrom), round2(m.rampHueTo)})},
                              {"ramp_spectrum", m.rampSpectrum},
                              {"ramp_chroma", std::round(m.rampChroma * 1000.0) / 1000.0},
                              {"hue_outside_span", round2(m.hueOutside)},
                              {"hue_deviation", round2(m.hueDeviation)},
                              {"chroma_ratio", round2(m.chromaRatio)}};
            r.threshold = Json{{"hue_slack_deg", COLOR_MATCH_HUE_SLACK}, {"chroma_share", COLOR_MATCH_CHROMA_SHARE}};
            if (!m.judged) {
                r.status = TitleCheckStatus::Skip;
                r.message = name + " (" + ref + "): " + m.note + ": not judged";
            } else if (m.hueFails || m.chromaFails) {
                r.status = TitleCheckStatus::Fail;
                std::string why;
                if (m.hueFails) {
                    why = "its hue " + std::to_string(static_cast<int>(std::round(m.faceHue))) + " deg strays " +
                          std::to_string(static_cast<int>(std::round(std::max(m.hueOutside, std::fabs(m.hueDeviation))))) + " deg from the scheme's face ramp";
                }
                if (m.chromaFails) {
                    why += (why.empty() ? "" : " and ") + std::string("it keeps only ") + std::to_string(static_cast<int>(std::round(m.chromaRatio * 100.0))) +
                           " % of the ramp's chroma";
                }
                r.message = name + "'s face no longer reads as Color Scheme " + ref + ": " + why + "; applying the scheme again restores its colours";
                r.fix = Json{{"ops", Json::array({Json{{"op", "apply_color_scheme"}, {"layer", layerRef(id)}, {"color_scheme", ref}}})}};
            } else {
                r.message = name + "'s face matches Color Scheme " + ref;
            }
            out.push_back(std::move(r));
        }
        if (judged == 0) out.push_back(result(TitleCheckStatus::Skip, "no layer records a Color Scheme", method));
    }

    // ---- metal_range ----

    bool isMetalFace(const Json& layer) {
        if (is3D(layer)) return num(member(layer["style_3d"], "face"), "metallic", 0.0) >= METAL_MIN_METALLIC;
        const Json& face = member(member(layer, "style"), "face");
        return flag(member(face, "finish"), "enabled", false) || flag(member(face, "bevel"), "enabled", false);
    }

    void metalRange(TitleCheckContext& ctx, Out& out) {
        const char* method = "the layer alone without rims or bands (S84 §6.14.1): L* p95 - p5 of the face pixels (alpha >= 0.5)";
        FaceRenders& faces = facesOf(ctx);
        size_t judged = 0;
        for (const Json* layer : candidateLayers(ctx.doc)) {
            if (!isMetalFace(*layer)) continue;
            judged++;
            const std::string id = str(*layer, "id"), name = str(*layer, "name");
            std::string error;
            const TitleCheckImage* face = faces.face(*layer, error);
            if (!face) {
                out.push_back(result(TitleCheckStatus::Fail, "the render failed: " + error, "render"));
                return;
            }
            const double range = measureMetalRange(*face);
            TitleCheckResult r = result(TitleCheckStatus::Pass, "", method);
            r.measured = Json{{"layer", id}, {"layer_name", name}, {"dimension", is3D(*layer) ? "3d" : "2d"}, {"l_range", round2(std::max(range, 0.0))}};
            r.threshold = Json{{"l_range_min", METAL_MIN_L_RANGE}};
            if (range < 0.0) {
                r.status = TitleCheckStatus::Skip;
                r.message = name + ": the face draws no pixels: not judged";
            } else if (range < METAL_MIN_L_RANGE) {
                r.status = TitleCheckStatus::Fail;
                r.message = name + "'s metal face spans only " + std::to_string(static_cast<int>(std::round(range))) +
                            " L*: it reads as flat paint (a metal needs light and dark bands)";
                if (is3D(*layer)) {
                    const Json& s = (*layer)["style_3d"];
                    const Json& own = member(member(s, "lighting"), "environment");
                    const double current = own.contains("intensity") ? num(own, "intensity", 1.0)
                                                                     : num(member(member(ctx.doc.root, "scene_3d"), "environment"), "intensity", 1.0);
                    const double raised = round2(std::max(current, 1.0) * METAL_ENVIRONMENT_RAISE);
                    r.fix = Json{{"ops", Json::array({Json{{"op", "set"}, {"layer", layerRef(id)}, {"values", Json{{"style_3d.lighting.environment.intensity", raised}}}}})}};
                } else if (!flag(member(member(member(*layer, "style"), "face"), "finish"), "enabled", false)) {
                    r.fix = Json{{"ops", Json::array({Json{{"op", "set"}, {"layer", layerRef(id)}, {"values", Json{{"face.finish.enabled", true}}}}})}};
                } else {
                    r.message += "; give its finish ramp light and dark stops";
                }
            } else {
                r.message = name + "'s metal face spans " + std::to_string(static_cast<int>(std::round(range))) + " L*";
            }
            out.push_back(std::move(r));
        }
        if (judged == 0) out.push_back(result(TitleCheckStatus::Skip, "no metal face (2D finish or bevel, 3D metallic)", method));
    }

}

// ---- colour ----

Oklch titlelibrary::toOklch(double r, double g, double b) {
    const double R = srgbToLinear(r), G = srgbToLinear(g), B = srgbToLinear(b);
    const double l = std::cbrt(0.4122214708 * R + 0.5363325363 * G + 0.0514459929 * B);
    const double m = std::cbrt(0.2119034982 * R + 0.6806995451 * G + 0.1073969566 * B);
    const double s = std::cbrt(0.0883024619 * R + 0.2817188376 * G + 0.6299787005 * B);
    const double L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    const double A = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    const double Bb = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
    return {L, std::hypot(A, Bb), wrap360(std::atan2(Bb, A) * 180.0 / PI)};
}

titleart::TitleColor titlelibrary::fromOklch(const Oklch& c) {
    auto linearOf = [&](double chroma, double out[3]) {
        const double h = c.h * PI / 180.0;
        const double A = chroma * std::cos(h), B = chroma * std::sin(h);
        const double l = std::pow(c.L + 0.3963377774 * A + 0.2158037573 * B, 3.0);
        const double m = std::pow(c.L - 0.1055613458 * A - 0.0638541728 * B, 3.0);
        const double s = std::pow(c.L - 0.0894841775 * A - 1.2914855480 * B, 3.0);
        out[0] = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
        out[1] = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
        out[2] = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
    };
    auto fits = [](const double v[3]) {
        return v[0] >= -1e-4 && v[0] <= 1.0001 && v[1] >= -1e-4 && v[1] <= 1.0001 && v[2] >= -1e-4 && v[2] <= 1.0001;
    };
    double lin[3];
    linearOf(c.C, lin);
    if (!fits(lin)) {
        double lo = 0.0, hi = c.C;
        for (int i = 0; i < 20; i++) {
            const double mid = (lo + hi) / 2.0;
            linearOf(mid, lin);
            (fits(lin) ? lo : hi) = mid;
        }
        linearOf(lo, lin);
    }
    auto channel = [](double v) { return static_cast<float>(std::clamp(linearToSrgb(std::clamp(v, 0.0, 1.0)), 0.0, 1.0)); };
    return {channel(lin[0]), channel(lin[1]), channel(lin[2])};
}

// ---- metrics ----

Json titlelibrary::schemeFaceRamp(const titleart::ColorScheme& scheme) {
    const Json& fill = scheme.face.fill;
    if (fill.is_object()) {
        const std::string kind = fill.value("kind", std::string("solid"));
        if (kind == "solid") return fill.value("color", Json("#ffffff"));
        auto gradient = fill.find("gradient");
        if (gradient != fill.end() && gradient->is_object() && gradient->contains("stops") && !(*gradient)["stops"].empty()) return *gradient;
    }
    if (scheme.face.ramp.is_object()) return scheme.face.ramp;
    return nullptr;
}

ColorMatchMetric titlelibrary::measureColorSchemeMatch(const TitleCheckImage& face, const Json& schemeFaceRamp) {
    ColorMatchMetric m;
    // the ramp: its colours (white cores out), its mean chroma and the arc its hues cover
    std::vector<Oklch> ramp, chromatic;
    for (const RampSample& s : rampOf(schemeFaceRamp)) {
        if (s.white) continue;
        ramp.push_back(s.c);
        if (s.c.C >= COLOR_MATCH_CHROMATIC) chromatic.push_back(s.c);
    }
    for (const Oklch& c : ramp) m.rampChroma += c.C / ramp.size();
    if (chromatic.empty()) {
        m.note = "the scheme's face ramp has no hue (white, grey or black)";
        return m;
    }
    std::vector<double> hues;
    for (const Oklch& c : chromatic) hues.push_back(c.h);
    std::sort(hues.begin(), hues.end());
    // the span is the circle minus its widest empty gap
    double gap = 360.0 - hues.back() + hues.front();
    size_t after = 0;
    for (size_t i = 1; i < hues.size(); i++) {
        if (hues[i] - hues[i - 1] > gap) {
            gap = hues[i] - hues[i - 1];
            after = i;
        }
    }
    m.rampHueFrom = hues[after];
    m.rampHueTo = hues[(after + hues.size() - 1) % hues.size()];
    m.rampSpectrum = 360.0 - gap > COLOR_MATCH_SPECTRUM_SPAN;

    // the face ink: mean hue (of the mean a, b), mean chroma, and the chroma-weighted mean of each
    // chromatic pixel's hue minus the ramp's hue at the same lightness
    double sumA = 0.0, sumB = 0.0, sumC = 0.0, sumDev = 0.0, sumWeight = 0.0;
    for (size_t i = 0; i + 3 < face.rgba.size(); i += 4) {
        if (face.rgba[i + 3] < COLOR_MATCH_MIN_ALPHA) continue;
        const double r = face.rgba[i] / 255.0, g = face.rgba[i + 1] / 255.0, b = face.rgba[i + 2] / 255.0;
        if (whiteCore(r, g, b)) continue;
        const Oklch c = toOklch(r, g, b);
        m.pixels++;
        sumA += c.C * std::cos(c.h * PI / 180.0);
        sumB += c.C * std::sin(c.h * PI / 180.0);
        sumC += c.C;
        if (c.C < COLOR_MATCH_CHROMATIC) continue;
        // the ramp samples nearest in lightness; of those, the one nearest in hue (a ramp that
        // returns to a lightness, as banded metals do, offers each of its hues there)
        double nearest = 1e9;
        for (const Oklch& s : chromatic) nearest = std::min(nearest, std::fabs(s.L - c.L));
        double best = 1e9;
        for (const Oklch& s : chromatic) {
            if (std::fabs(s.L - c.L) > nearest + L_MATCH_SLACK) continue;
            const double d = hueDelta(c.h, s.h);
            if (std::fabs(d) < std::fabs(best)) best = d;
        }
        sumDev += best * c.C;
        sumWeight += c.C;
    }
    if (m.pixels == 0) {
        m.note = "the face draws no ink";
        return m;
    }
    m.judged = true;
    m.faceHue = wrap360(std::atan2(sumB, sumA) * 180.0 / PI);
    m.faceChroma = sumC / m.pixels;
    m.hueDeviation = sumWeight > 0.0 ? sumDev / sumWeight : 0.0;
    m.chromaRatio = m.rampChroma > 0.0 ? m.faceChroma / m.rampChroma : 1.0;
    if (!m.rampSpectrum && m.faceChroma >= COLOR_MATCH_CHROMATIC) {
        const double arc = wrap360(m.rampHueTo - m.rampHueFrom);
        const double offset = wrap360(m.faceHue - m.rampHueFrom);
        m.hueOutside = offset <= arc ? 0.0 : std::min(offset - arc, 360.0 - offset);
        m.hueFails = m.hueOutside > COLOR_MATCH_HUE_SLACK || std::fabs(m.hueDeviation) > COLOR_MATCH_HUE_SLACK;
    }
    m.chromaFails = m.chromaRatio < COLOR_MATCH_CHROMA_SHARE;
    return m;
}

double titlelibrary::measureMetalRange(const TitleCheckImage& face) {
    std::vector<double> ls;
    for (size_t i = 0; i + 3 < face.rgba.size(); i += 4) {
        if (face.rgba[i + 3] < COLOR_MATCH_MIN_ALPHA) continue;
        ls.push_back(lstar(face.rgba[i] / 255.0, face.rgba[i + 1] / 255.0, face.rgba[i + 2] / 255.0));
    }
    if (ls.empty()) return -1.0;
    std::sort(ls.begin(), ls.end());
    auto at = [&](double q) { return ls[static_cast<size_t>(std::round(q * (ls.size() - 1)))]; };
    return at(0.95) - at(0.05);
}

TitleCheckStatus titlelibrary::styleLintStatus(const StyleLintFinding& finding) {
    // hollow letters without any rim render nothing at all; every other finding still renders
    if (finding.rule == "noop_knockout" && finding.element == "rims") return TitleCheckStatus::Fail;
    return TitleCheckStatus::Warn;
}

void titlelibrary::registerLibraryChecks() {
    struct Row {
        const char* id;
        TitleCheckKind kind;
        const char* description;
        void (*run)(TitleCheckContext&, Out&);
    };
    const Row rows[] = {
        {"style_lint", TitleCheckKind::Document, "Lettering practice: ring contrast, hue count, rim totals, hard shadows, rainbow span, switches that do nothing",
         styleLint},
        {"color_scheme_match", TitleCheckKind::Pixels, "A face recoloured by a Color Scheme still shows the scheme's hue and chroma", colorSchemeMatch},
        {"metal_range", TitleCheckKind::Pixels, "A metal face (finish, bevel or 3D metallic) spans enough lightness to read as metal", metalRange},
    };
    for (const Row& row : rows) {
        // idempotent: a second call finds the ids taken
        if (TitleArtChecks::find(row.id)) continue;
        CheckDef def;
        def.id = row.id;
        def.kind = row.kind;
        def.owner = "S84";
        def.group = "style";
        def.description = row.description;
        auto run = row.run;
        def.run = [run](TitleCheckContext& context, Out& out) { run(context, out); };
        TitleArtChecks::registerCheck(std::move(def));
    }
}
