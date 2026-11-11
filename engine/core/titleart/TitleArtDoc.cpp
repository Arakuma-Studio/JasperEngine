// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtDoc.h"
#include "TitleArtTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    const Json& emptyArray() {
        static const Json empty = Json::array();
        return empty;
    }

    double numberOr(const Json& object, const char* key, double fallback) {
        if (!object.is_object()) return fallback;
        auto it = object.find(key);
        return it != object.end() && it->is_number() ? it->get<double>() : fallback;
    }

    int hexValue(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool isLowerHex(std::string_view text, size_t length) {
        if (text.size() != length) return false;
        return std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }

    std::string randomHex(std::mt19937_64& random, size_t length) {
        static const char* digits = "0123456789abcdef";
        std::string out(length, '0');
        uint64_t bits = 0;
        for (size_t i = 0; i < length; i++) {
            if (i % 16 == 0) bits = random();
            out[i] = digits[bits & 15];
            bits >>= 4;
        }
        return out;
    }

    // Fritsch-Butland tangents of a monotone cubic through `p` (corners take one-sided slopes)
    std::vector<double> curveTangents(const std::vector<TitleCurvePoint>& p) {
        const size_t n = p.size();
        std::vector<double> m(n, 0.0);
        if (n < 2) return m;
        std::vector<double> slope(n - 1);
        for (size_t i = 0; i + 1 < n; i++) {
            const double dx = p[i + 1].x - p[i].x;
            slope[i] = dx > 1e-12 ? (p[i + 1].y - p[i].y) / dx : 0.0;
        }
        m[0] = slope[0];
        m[n - 1] = slope[n - 2];
        for (size_t i = 1; i + 1 < n; i++) {
            if (p[i].corner) {
                m[i] = slope[i];    // into the next segment; the previous one ends with its own slope
                continue;
            }
            const double a = slope[i - 1], b = slope[i];
            if (a * b <= 0) {
                m[i] = 0;
            } else {
                // Fritsch-Butland: the weighted harmonic mean keeps the curve monotone
                m[i] = 3.0 * a * b / (std::max(a, b) + 2.0 * std::min(a, b));
            }
        }
        return m;
    }

}

// ---- TitleArtTypes ----

const char* titleart::paramUnitName(ParamUnit unit) {
    switch (unit) {
        case ParamUnit::None: return "";
        case ParamUnit::Px: return "px";
        case ParamUnit::Percent: return "%";
        case ParamUnit::Deg: return "deg";
        case ParamUnit::Factor: return "x";
    }
    return "";
}

const char* titleart::fxAnimName(FxAnim anim) {
    switch (anim) {
        case FxAnim::None: return "none";
        case FxAnim::Live: return "live";
        case FxAnim::Rerender: return "rerender";
        case FxAnim::Step: return "step";
        case FxAnim::Relayout: return "relayout";
    }
    return "none";
}

bool titleart::fxAnimFromName(std::string_view name, FxAnim& out) {
    for (FxAnim anim : {FxAnim::None, FxAnim::Live, FxAnim::Rerender, FxAnim::Step, FxAnim::Relayout}) {
        if (name == fxAnimName(anim)) {
            out = anim;
            return true;
        }
    }
    return false;
}

const char* titleart::glyphSafetyName(GlyphSafety safety) {
    switch (safety) {
        case GlyphSafety::Safe: return "safe";
        case GlyphSafety::Approximate: return "approximate";
        case GlyphSafety::Unsafe: return "unsafe";
    }
    return "safe";
}

const char* titleart::hashScopeName(HashScope scope) {
    switch (scope) {
        case HashScope::Inherit: return "inherit";
        case HashScope::Identity: return "identity";
        case HashScope::Geometry: return "geometry";
        case HashScope::Mask: return "mask";
        case HashScope::Field: return "field";
        case HashScope::Stack: return "stack";
        case HashScope::Effects: return "effects";
        case HashScope::Masks: return "masks";
        case HashScope::Placement: return "placement";
        case HashScope::Node: return "node";
    }
    return "inherit";
}

// ---- enums ----

const char* titleart::layerTypeName(LayerType type) {
    switch (type) {
        case LayerType::Text: return "text";
        case LayerType::Shape: return "shape";
        case LayerType::Image: return "image";
        case LayerType::Vector: return "vector";
        case LayerType::Group: return "group";
        case LayerType::Generator: return "generator";
        case LayerType::Adjustment: return "adjustment";
        case LayerType::Unknown: break;
    }
    return "";
}

const char* titleart::layerTypeLabel(LayerType type) {
    switch (type) {
        case LayerType::Text: return "Text";
        case LayerType::Shape: return "Shape";
        case LayerType::Image: return "Image";
        case LayerType::Vector: return "Vector (SVG)";
        case LayerType::Group: return "Group";
        case LayerType::Generator: return "Generator";
        case LayerType::Adjustment: return "Adjustment";
        case LayerType::Unknown: break;
    }
    return "Layer";
}

LayerType titleart::layerTypeFromName(std::string_view name) {
    for (LayerType type : {LayerType::Text, LayerType::Shape, LayerType::Image, LayerType::Vector, LayerType::Group,
                           LayerType::Generator, LayerType::Adjustment}) {
        if (name == layerTypeName(type)) return type;
    }
    return LayerType::Unknown;
}

bool titleart::layerTypeHasStyle(LayerType type) {
    return type == LayerType::Text || type == LayerType::Shape || type == LayerType::Image || type == LayerType::Vector;
}

namespace {
    struct BlendName {
        const char* name;
        const char* label;
    };
    const BlendName BLENDS[TITLE_BLEND_COUNT] = {
        {"normal", "Normal"}, {"add", "Add (Linear Dodge)"}, {"screen", "Screen"}, {"multiply", "Multiply"},
        {"overlay", "Overlay"}, {"soft_light", "Soft Light"}, {"hard_light", "Hard Light"}, {"darken", "Darken"},
        {"lighten", "Lighten"}, {"color_dodge", "Color Dodge"}, {"color_burn", "Color Burn"},
        {"linear_burn", "Linear Burn"}, {"difference", "Difference"}, {"exclusion", "Exclusion"}, {"hue", "Hue"},
        {"saturation", "Saturation"}, {"color", "Color"}, {"luminosity", "Luminosity"}};
}

const char* titleart::titleBlendName(TitleBlend blend) {
    const int i = (int)blend;
    return i >= 0 && i < TITLE_BLEND_COUNT ? BLENDS[i].name : "normal";
}

const char* titleart::titleBlendLabel(TitleBlend blend) {
    const int i = (int)blend;
    return i >= 0 && i < TITLE_BLEND_COUNT ? BLENDS[i].label : "Normal";
}

bool titleart::titleBlendFromName(std::string_view name, TitleBlend& out) {
    for (int i = 0; i < TITLE_BLEND_COUNT; i++) {
        if (name == BLENDS[i].name) {
            out = (TitleBlend)i;
            return true;
        }
    }
    return false;
}

const char* titleart::outputContentName(OutputContent content) {
    switch (content) {
        case OutputContent::All: return "all";
        case OutputContent::Layers: return "layers";
        case OutputContent::Ink: return "ink";
        case OutputContent::Glow: return "glow";
        case OutputContent::Mask: return "mask";
    }
    return "all";
}

bool titleart::outputContentFromName(std::string_view name, OutputContent& out) {
    for (OutputContent c : {OutputContent::All, OutputContent::Layers, OutputContent::Ink, OutputContent::Glow, OutputContent::Mask}) {
        if (name == outputContentName(c)) {
            out = c;
            return true;
        }
    }
    return false;
}

const char* titleart::maskTypeName(MaskType type) {
    switch (type) {
        case MaskType::Shape: return "shape";
        case MaskType::Gradient: return "gradient";
        case MaskType::Layer: return "layer";
        case MaskType::Image: return "image";
        case MaskType::Unknown: break;
    }
    return "";
}

MaskType titleart::maskTypeFromName(std::string_view name) {
    for (MaskType type : {MaskType::Shape, MaskType::Gradient, MaskType::Layer, MaskType::Image}) {
        if (name == maskTypeName(type)) return type;
    }
    return MaskType::Unknown;
}

// ---- value types ----

bool titleart::parseTitleColor(const Json& value, TitleColor& out) {
    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        if (text.size() != 4 && text.size() != 7) return false;
        if (text[0] != '#') return false;
        int digits[6];
        const size_t count = text.size() - 1;
        for (size_t i = 0; i < count; i++) {
            digits[i] = hexValue(text[i + 1]);
            if (digits[i] < 0) return false;
        }
        if (count == 3) {
            out = {(float)(digits[0] * 17) / 255.0f, (float)(digits[1] * 17) / 255.0f, (float)(digits[2] * 17) / 255.0f};
        } else {
            out = {(float)(digits[0] * 16 + digits[1]) / 255.0f, (float)(digits[2] * 16 + digits[3]) / 255.0f,
                   (float)(digits[4] * 16 + digits[5]) / 255.0f};
        }
        return true;
    }
    if (value.is_array() && value.size() == 3) {
        float c[3];
        for (size_t i = 0; i < 3; i++) {
            if (!value[i].is_number()) return false;
            const double v = value[i].get<double>();
            if (!std::isfinite(v)) return false;
            c[i] = (float)std::clamp(v, 0.0, 1.0);
        }
        out = {c[0], c[1], c[2]};
        return true;
    }
    return false;
}

std::string titleart::formatTitleColor(const TitleColor& color) {
    auto code = [](float v) { return (int)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
    char text[8];
    std::snprintf(text, sizeof(text), "#%02x%02x%02x", code(color.r), code(color.g), code(color.b));
    return text;
}

float titleart::srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float titleart::linearToSrgb(float c) {
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

TitleVec2 titleart::decodeVec2(const Json& value) {
    if (value.is_array() && value.size() == 2 && value[0].is_number() && value[1].is_number()) {
        return {value[0].get<double>(), value[1].get<double>()};
    }
    return {};
}

TitleAssetRef titleart::decodeAssetRef(const Json& value) {
    TitleAssetRef ref;
    if (value.is_object()) {
        ref.path = value.value("path", std::string());
        ref.guid = value.value("guid", std::string());
    }
    return ref;
}

Json titleart::encodeAssetRef(const TitleAssetRef& ref) {
    if (ref.empty()) return nullptr;
    Json out = Json::object();
    out["path"] = ref.path;
    out["guid"] = ref.guid;
    return out;
}

TitleGradient titleart::decodeGradient(const Json& value) {
    TitleGradient gradient;
    if (!value.is_object()) return gradient;
    gradient.vivid = value.value("interp", std::string("rgb")) == "vivid";
    auto stops = value.find("stops");
    if (stops == value.end() || !stops->is_array()) return gradient;
    for (const Json& s : *stops) {
        TitleGradientStop stop;
        stop.pos = numberOr(s, "pos", 0.0);
        auto color = s.find("color");
        if (color != s.end()) parseTitleColor(*color, stop.color);
        stop.alpha = numberOr(s, "alpha", 1.0);
        stop.mid = numberOr(s, "mid", 0.5);
        gradient.stops.push_back(stop);
    }
    return gradient;
}

TitleCurve titleart::decodeCurve(const Json& value) {
    TitleCurve curve;
    if (value.is_object()) {
        auto points = value.find("points");
        if (points != value.end() && points->is_array()) {
            for (const Json& p : *points) {
                curve.points.push_back({numberOr(p, "x", 0.0), numberOr(p, "y", 0.0), p.is_object() && p.value("corner", false)});
            }
        }
    }
    if (curve.points.size() < 2) curve.points = {{0.0, 0.0, false}, {1.0, 1.0, false}};
    return curve;
}

double TitleCurve::evaluate(double x) const {
    if (points.empty()) return x;
    x = std::clamp(x, 0.0, 1.0);
    if (points.size() == 1) return points[0].y;
    if (x <= points.front().x) return points.front().y;
    if (x >= points.back().x) return points.back().y;
    const std::vector<double> m = curveTangents(points);
    size_t i = 0;
    while (i + 2 < points.size() && x > points[i + 1].x) i++;
    const TitleCurvePoint& a = points[i];
    const TitleCurvePoint& b = points[i + 1];
    const double h = b.x - a.x;
    if (h <= 1e-12) return b.y;
    // a corner ends the segment before it with that segment's own slope
    const double slope = (b.y - a.y) / h;
    const double m0 = m[i];
    const double m1 = b.corner ? slope : m[i + 1];
    const double t = (x - a.x) / h;
    const double t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * a.y + (t3 - 2 * t2 + t) * h * m0 + (-2 * t3 + 3 * t2) * b.y + (t3 - t2) * h * m1;
}

double TitleCurve::derivative(double x, bool fromLeft) const {
    if (points.size() < 2) return points.empty() ? 1.0 : 0.0;
    x = std::clamp(x, 0.0, 1.0);
    const std::vector<double> m = curveTangents(points);
    // the segment holding x; at a key, the one on the requested side
    size_t i = 0;
    while (i + 2 < points.size() && (fromLeft ? x > points[i + 1].x : x >= points[i + 1].x)) i++;
    const TitleCurvePoint& a = points[i];
    const TitleCurvePoint& b = points[i + 1];
    const double h = b.x - a.x;
    if (h <= 1e-12) return 0.0;
    if (x < a.x || x > b.x) return 0.0;     // outside the keys the curve holds its end values
    const double slope = (b.y - a.y) / h;
    const double m0 = m[i];
    const double m1 = b.corner ? slope : m[i + 1];
    const double t = (x - a.x) / h;
    const double t2 = t * t;
    // d/dx of evaluate's Hermite basis (d/dt divided by h)
    return ((6 * t2 - 6 * t) * a.y + (3 * t2 - 4 * t + 1) * h * m0 + (-6 * t2 + 6 * t) * b.y + (3 * t2 - 2 * t) * h * m1) / h;
}

std::vector<float> TitleCurve::sample(int count) const {
    std::vector<float> out((size_t)std::max(0, count));
    for (int i = 0; i < count; i++) {
        out[(size_t)i] = (float)evaluate(count > 1 ? (double)i / (count - 1) : 0.0);
    }
    return out;
}

TitleTransform titleart::decodeTransform(const Json& transform) {
    TitleTransform t;
    if (!transform.is_object()) return t;
    auto position = transform.find("position");
    if (position != transform.end()) t.position = decodeVec2(*position);
    t.rotation = numberOr(transform, "rotation", 0.0);
    t.scale = numberOr(transform, "scale", 1.0);
    t.flipX = transform.value("flip_x", false);
    t.flipY = transform.value("flip_y", false);
    if (auto stretch = transform.find("stretch"); stretch != transform.end()) t.stretch = decodeVec2(*stretch);
    return t;
}

// ---- problems ----

std::string TitleWarning::toString() const {
    return path.empty() ? message : path + ": " + message;
}

const char* titleart::titleSeverityName(TitleSeverity severity) {
    switch (severity) {
        case TitleSeverity::Info: return "info";
        case TitleSeverity::Warning: return "warning";
        case TitleSeverity::Error: return "error";
    }
    return "warning";
}

Json TitleProblem::toJson() const {
    Json out = Json::object();
    out["severity"] = titleSeverityName(severity);
    out["code"] = code;
    if (!layer.empty()) out["layer"] = layer;
    if (!path.empty()) out["path"] = path;
    out["message"] = message;
    if (detail.is_object()) {
        for (auto it = detail.begin(); it != detail.end(); ++it) out[it.key()] = it.value();
    }
    return out;
}

// ---- the document ----

const std::string& TitleArtDoc::guid() const {
    static const std::string none;
    auto it = root.find("guid");
    return it != root.end() && it->is_string() ? it->get_ref<const std::string&>() : none;
}

int TitleArtDoc::canvasWidth() const {
    auto it = root.find("canvas");
    return it != root.end() ? (int)numberOr(*it, "width", 1920) : 1920;
}

int TitleArtDoc::canvasHeight() const {
    auto it = root.find("canvas");
    return it != root.end() ? (int)numberOr(*it, "height", 1080) : 1080;
}

const Json& TitleArtDoc::layers() const {
    auto it = root.find("layers");
    return it != root.end() && it->is_array() ? *it : emptyArray();
}

Json& TitleArtDoc::layers() {
    Json& layers = root["layers"];
    if (!layers.is_array()) layers = Json::array();
    return layers;
}

std::string titleart::generateTitleArtGuid() {
    static thread_local std::mt19937_64 random{std::random_device{}() ^ ((uint64_t)std::random_device{}() << 32)};
    return randomHex(random, 32);
}

bool titleart::isTitleArtGuid(std::string_view text) {
    return isLowerHex(text, 32);
}

bool titleart::isTitleArtId(std::string_view text) {
    return isLowerHex(text, 8);
}

TitleIdGenerator::TitleIdGenerator() : random(std::random_device{}() ^ ((uint64_t)std::random_device{}() << 32)) {
}

TitleIdGenerator::TitleIdGenerator(uint64_t seed) : random(seed) {
}

void TitleIdGenerator::reserve(const std::string& id) {
    if (!id.empty()) used.insert(id);
}

bool TitleIdGenerator::taken(const std::string& id) const {
    return used.count(id) > 0;
}

void TitleIdGenerator::reserveFrom(const Json& root) {
    forEachLayer(root, [this](const Json& layer, const Json*, int) {
        reserve(layer.value("id", std::string()));
        std::vector<std::string> ids;
        collectLayerIds(layer, ids);
        for (const std::string& id : ids) reserve(id);
        return true;
    });
}

std::string TitleIdGenerator::next() {
    for (;;) {
        std::string id = randomHex(random, 8);
        if (used.insert(id).second) return id;
    }
}

// ---- the layer tree ----

LayerType titleart::layerTypeOf(const Json& layer) {
    if (!layer.is_object()) return LayerType::Unknown;
    auto it = layer.find("type");
    return it != layer.end() && it->is_string() ? layerTypeFromName(it->get_ref<const std::string&>()) : LayerType::Unknown;
}

const Json& titleart::layerChildren(const Json& layer) {
    if (layerTypeOf(layer) != LayerType::Group) return emptyArray();
    auto it = layer.find("children");
    return it != layer.end() && it->is_array() ? *it : emptyArray();
}

namespace {
    template<class J>
    J* findIn(J& layers, std::string_view id) {
        if (!layers.is_array()) return nullptr;
        for (auto& layer : layers) {
            if (!layer.is_object()) continue;
            auto it = layer.find("id");
            if (it != layer.end() && it->is_string() && it->template get_ref<const std::string&>() == id) return &layer;
            if (layerTypeOf(layer) == LayerType::Group) {
                auto children = layer.find("children");
                if (children != layer.end()) {
                    if (J* found = findIn(*children, id)) return found;
                }
            }
        }
        return nullptr;
    }

    bool walk(const Json& layers, const Json* parent, int depth, const LayerVisitor& visit) {
        if (!layers.is_array()) return true;
        for (const Json& layer : layers) {
            if (!layer.is_object()) continue;
            if (!visit(layer, parent, depth)) return false;
            if (!walk(layerChildren(layer), &layer, depth + 1, visit)) return false;
        }
        return true;
    }
}

Json* titleart::findLayer(Json& root, std::string_view id) {
    if (id.empty() || !root.is_object()) return nullptr;
    auto it = root.find("layers");
    return it != root.end() ? findIn(*it, id) : nullptr;
}

const Json* titleart::findLayer(const Json& root, std::string_view id) {
    if (id.empty() || !root.is_object()) return nullptr;
    auto it = root.find("layers");
    return it != root.end() ? findIn(*it, id) : nullptr;
}

void titleart::forEachLayer(const Json& root, const LayerVisitor& visit) {
    if (!root.is_object()) return;
    auto it = root.find("layers");
    if (it != root.end()) walk(*it, nullptr, 0, visit);
}

size_t titleart::countLayers(const Json& root) {
    size_t count = 0;
    forEachLayer(root, [&count](const Json&, const Json*, int) {
        count++;
        return true;
    });
    return count;
}

double titleart::layerSourceSize(const Json& layer) {
    switch (layerTypeOf(layer)) {
        case LayerType::Text:
        case LayerType::Shape:
        case LayerType::Image:
        case LayerType::Vector:
            return numberOr(layer, "size", 200.0);
        default:
            return 0.0;
    }
}

void titleart::collectLayerIds(const Json& layer, std::vector<std::string>& out) {
    if (!layer.is_object()) return;
    auto addIds = [&out](const Json& list) {
        if (!list.is_array()) return;
        for (const Json& item : list) {
            if (item.is_object()) {
                auto id = item.find("id");
                if (id != item.end() && id->is_string()) out.push_back(id->get<std::string>());
            }
        }
    };
    auto masks = layer.find("masks");
    if (masks != layer.end()) addIds(*masks);
    auto effects = layer.find("effects");
    if (effects != layer.end()) addIds(*effects);
    auto parts = layer.find("part_effects");
    if (parts != layer.end() && parts->is_object()) {
        for (auto it = parts->begin(); it != parts->end(); ++it) addIds(it.value());
    }
    auto style = layer.find("style");
    if (style != layer.end() && style->is_object()) {
        auto rims = style->find("rims");
        if (rims != style->end()) addIds(*rims);
    }
}
