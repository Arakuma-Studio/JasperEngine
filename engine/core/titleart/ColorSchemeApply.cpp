// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/presets/colourways.ts by SafEight, used with the author's permission
// (the gradient sampling from src/model/gradient.ts, likewise).

#include "ColorSchemeApply.h"

#include "TitleArtIO.h"
#include "TitleArtSchema.h"
#include "TitleEffectRegistry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <set>
#include <string_view>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    const Json NULL_JSON;

    // ---- colourways.ts's constants ----

    // a ring darker than this (Rec.601 luma of its fill through its finish) is the dark keyline
    constexpr double DARK_LUMA = 0.18;
    // every channel at least this: a white core, kept in every scheme
    constexpr double WHITE_CORE = 0.92;
    // a colour within this of its ramp colour (every channel) already is the scheme's (8-bit slack)
    constexpr double ON_RAMP = 4.0 / 255.0;
    // a spectrum on a gradient with a white core stop: the share the core keeps
    constexpr double CORE_SPAN = 0.15;
    // the ground under a pattern fill's marks when the scheme names none
    const char* const PATTERN_GROUND = "#ffc81e";

    // Pachislop's Glow and Shadow categories by our type names (pachislop-fx-map.yaml): every colour
    // of a glow takes `glow` (its gradients `glow_gradient`), every colour of a shadow `shadow`
    bool isGlowEffect(std::string_view t) {
        return t == "outer_glow" || t == "inner_glow" || t == "light_glow" || t == "contour_lines";
    }
    bool isShadowEffect(std::string_view t) {
        return t == "drop_shadow" || t == "inner_shadow" || t == "perspective_shadow" || t == "floor_reflection";
    }
    // effects recoloured like generators, every colour param (LIGHT_EFFECTS)
    bool isLightEffect(std::string_view t) {
        return t == "shatter" || t == "motion_trail";
    }
    // natural phenomena and optics keep their colours (OWN_COLOURS); `fill` is the background
    // Pachislop keeps as a layer of its own
    bool keepsColours(std::string_view t) {
        return t == "fire" || t == "smoke" || t == "ink_splatter" || t == "lens_flare" || t == "glints" || t == "fill";
    }
    // SPECTRUM_FILL, for definitions whose SpectrumSwitch S83 has not filled in
    SpectrumSwitch builtinSpectrumSwitch(std::string_view type) {
        if (type == "particles") return {"color_mode", "solid", "palette"};
        if (type == "speed_lines") return {"color_mode", "solid", "around"};
        return {};
    }

    // ---- reads of normalized JSON ----

    const Json& at(const Json& o, const char* key) {
        if (!o.is_object()) return NULL_JSON;
        auto it = o.find(key);
        return it != o.end() ? *it : NULL_JSON;
    }
    std::string str(const Json& o, const char* key, const char* fallback = "") {
        const Json& v = at(o, key);
        return v.is_string() ? v.get<std::string>() : std::string(fallback);
    }
    double num(const Json& o, const char* key, double fallback) {
        const Json& v = at(o, key);
        return v.is_number() ? v.get<double>() : fallback;
    }
    bool flag(const Json& o, const char* key, bool fallback) {
        const Json& v = at(o, key);
        return v.is_boolean() ? v.get<bool>() : fallback;
    }

    // the member `key` of `o` when it exists (never created: a recolouring adds no members)
    Json* child(Json& o, const char* key) {
        if (!o.is_object()) return nullptr;
        auto it = o.find(key);
        return it != o.end() ? &*it : nullptr;
    }

    std::string member(const std::string& path, const std::string& key) {
        return path.empty() ? key : path + "." + key;
    }
    std::string indexed(const std::string& path, size_t i) {
        return path + "[" + std::to_string(i) + "]";
    }

    // ---- colours (sRGB 0..1) ----

    using Rgb = std::array<double, 3>;

    int hexDigit(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    }

    Rgb hexToRgb(std::string_view hex) {
        if (!hex.empty() && hex[0] == '#') hex.remove_prefix(1);
        if (hex.size() == 3) return {hexDigit(hex[0]) * 17 / 255.0, hexDigit(hex[1]) * 17 / 255.0, hexDigit(hex[2]) * 17 / 255.0};
        if (hex.size() != 6) return {0.0, 0.0, 0.0};
        return {(hexDigit(hex[0]) * 16 + hexDigit(hex[1])) / 255.0, (hexDigit(hex[2]) * 16 + hexDigit(hex[3])) / 255.0,
                (hexDigit(hex[4]) * 16 + hexDigit(hex[5])) / 255.0};
    }

    // Math.round of each channel, as colourways.ts's toHex
    std::string toHex(const Rgb& c) {
        char text[8];
        int v[3];
        for (int k = 0; k < 3; k++) v[k] = std::clamp((int)std::floor(c[k] * 255.0 + 0.5), 0, 255);
        std::snprintf(text, sizeof(text), "#%02x%02x%02x", v[0], v[1], v[2]);
        return text;
    }

    double luma(const Rgb& c) {
        return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];
    }

    bool isWhiteCore(std::string_view hex) {
        const Rgb c = hexToRgb(hex);
        return std::min({c[0], c[1], c[2]}) >= WHITE_CORE;
    }

    std::string canonicalColor(const std::string& color, const char* fallback) {
        TitleColor c;
        return parseTitleColor(Json(color), c) ? formatTitleColor(c) : std::string(fallback);
    }

    // ---- gradients (model/gradient.ts sampleGradientRgba) ----
    // The OKLCH blend is ported here rather than called in the renderer (effects/TitleFx.cpp holds
    // the same port behind GPU headers): this file must build without them (R39).

    double clamp01(double x) {
        return x < 0.0 ? 0.0 : x > 1.0 ? 1.0 : x;
    }

    double midpointBlend(double x, double mid) {
        const double m = std::min(0.95, std::max(0.05, mid));
        if (std::fabs(m - 0.5) < 1e-4 || x <= 0.0 || x >= 1.0) return x;
        return std::pow(x, std::log(0.5) / std::log(m));
    }

    double toLin(double c) {
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    }
    double toSrgb(double c) {
        return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    }

    Rgb oklch(const Rgb& c) {
        const double R = toLin(c[0]), G = toLin(c[1]), B = toLin(c[2]);
        const double l = std::cbrt(0.4122214708 * R + 0.5363325363 * G + 0.0514459929 * B);
        const double m = std::cbrt(0.2119034982 * R + 0.6806995451 * G + 0.1073969566 * B);
        const double s = std::cbrt(0.0883024619 * R + 0.2817188376 * G + 0.6299787005 * B);
        const double L = 0.2104542553 * l + 0.793617785 * m - 0.0040720468 * s;
        const double A = 1.9779984951 * l - 2.428592205 * m + 0.4505937099 * s;
        const double Bb = 0.0259040371 * l + 0.7827717662 * m - 0.808675766 * s;
        return {L, std::hypot(A, Bb), std::atan2(Bb, A)};
    }

    Rgb labToLinear(double L, double A, double B) {
        const double l = std::pow(L + 0.3963377774 * A + 0.2158037573 * B, 3.0);
        const double m = std::pow(L - 0.1055613458 * A - 0.0638541728 * B, 3.0);
        const double s = std::pow(L - 0.0894841775 * A - 1.291485548 * B, 3.0);
        return {4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s, -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
                -0.0041960863 * l - 0.7034186147 * m + 1.707614701 * s};
    }

    Rgb fromOklch(double L, double C, double h) {
        auto fits = [](const Rgb& c) { return std::all_of(c.begin(), c.end(), [](double v) { return v >= -1e-4 && v <= 1.0 + 1e-4; }); };
        Rgb lin = labToLinear(L, C * std::cos(h), C * std::sin(h));
        if (!fits(lin)) {
            double lo = 0.0, hi = C;
            for (int i = 0; i < 14; i++) {
                const double mid = (lo + hi) / 2.0;
                if (fits(labToLinear(L, mid * std::cos(h), mid * std::sin(h)))) lo = mid;
                else hi = mid;
            }
            lin = labToLinear(L, lo * std::cos(h), lo * std::sin(h));
        }
        return {clamp01(toSrgb(clamp01(lin[0]))), clamp01(toSrgb(clamp01(lin[1]))), clamp01(toSrgb(clamp01(lin[2])))};
    }

    Rgb mixVivid(const Rgb& a, const Rgb& b, double t) {
        constexpr double PI = 3.14159265358979323846;
        constexpr double ACHROMATIC = 0.004;
        const Rgb A = oklch(a), B = oklch(b);
        const double ha = A[1] < ACHROMATIC ? B[2] : A[2];
        const double hb = B[1] < ACHROMATIC ? A[2] : B[2];
        double dh = hb - ha;
        if (dh > PI) dh -= 2.0 * PI;
        else if (dh < -PI) dh += 2.0 * PI;
        return fromOklch(A[0] + (B[0] - A[0]) * t, A[1] + (B[1] - A[1]) * t, ha + dh * t);
    }

    struct Stop {
        double pos = 0.0;
        std::string hex;
        Rgb rgb{};
        double alpha = 1.0;
        double mid = 0.5;
    };
    struct Grad {
        std::vector<Stop> stops;    // sorted by pos
        bool vivid = false;
    };

    Grad readGradient(const Json& g) {
        Grad out;
        out.vivid = str(g, "interp", "rgb") == "vivid";
        const Json& stops = at(g, "stops");
        if (!stops.is_array()) return out;
        for (const Json& s : stops) {
            Stop stop;
            stop.pos = num(s, "pos", 0.0);
            stop.hex = str(s, "color", "#000000");
            stop.rgb = hexToRgb(stop.hex);
            stop.alpha = num(s, "alpha", 1.0);
            stop.mid = num(s, "mid", 0.5);
            out.stops.push_back(std::move(stop));
        }
        std::stable_sort(out.stops.begin(), out.stops.end(), [](const Stop& a, const Stop& b) { return a.pos < b.pos; });
        return out;
    }

    struct Rgba {
        Rgb rgb{1.0, 1.0, 1.0};
        double alpha = 1.0;
    };

    Rgba sample(const Grad& g, double pos) {
        const std::vector<Stop>& s = g.stops;
        if (s.empty()) return {};
        if (pos <= s.front().pos) return {s.front().rgb, s.front().alpha};
        for (size_t i = 0; i + 1 < s.size(); i++) {
            const Stop& a = s[i];
            const Stop& b = s[i + 1];
            if (pos > b.pos) continue;
            // a stop's own position gives the stop exactly (a + (b - a) is not always b), so a
            // profile sampled at its stops is the same profile
            if (pos == b.pos) return {b.rgb, b.alpha};
            const double span = b.pos - a.pos;
            const double x = midpointBlend(span > 0.0 ? (pos - a.pos) / span : 0.0, a.mid);
            Rgba out;
            if (g.vivid && a.hex != b.hex) out.rgb = mixVivid(a.rgb, b.rgb, x);
            else for (int k = 0; k < 3; k++) out.rgb[k] = a.rgb[k] + (b.rgb[k] - a.rgb[k]) * x;
            out.alpha = a.alpha + (b.alpha - a.alpha) * x;
            return out;
        }
        return {s.back().rgb, s.back().alpha};
    }

    double lumaGradient(const Grad& g) {
        double sum = 0.0;
        for (int i = 0; i <= 8; i++) sum += luma(sample(g, i / 8.0).rgb);
        return sum / 9.0;
    }

    bool gradientHasAlpha(const Json& g) {
        const Json& stops = at(g, "stops");
        if (!stops.is_array()) return false;
        return std::any_of(stops.begin(), stops.end(), [](const Json& s) { return num(s, "alpha", 1.0) < 1.0; });
    }

    Json stopJson(double pos, const std::string& color, double alpha) {
        return Json{{"pos", pos}, {"color", color}, {"alpha", alpha}, {"mid", 0.5}};
    }

    // `next` carrying the alpha profile of `prev` (keepAlpha): transparency is structure
    Json keepAlpha(const Json& prev, const Json& next) {
        const Grad p = readGradient(prev), n = readGradient(next);
        std::vector<double> at;
        for (const Stop& s : n.stops) at.push_back(s.pos);
        for (const Stop& s : p.stops) at.push_back(s.pos);
        std::sort(at.begin(), at.end());
        at.erase(std::unique(at.begin(), at.end()), at.end());
        Json out = next.is_object() ? next : Json::object();
        Json stops = Json::array();
        for (double pos : at) stops.push_back(stopJson(pos, toHex(sample(n, pos).rgb), sample(p, pos).alpha));
        out["stops"] = std::move(stops);
        return out;
    }

    Json solidGradient(const std::string& color) {
        return Json{{"stops", Json::array({stopJson(0, color, 1), stopJson(1, color, 1)})}, {"interp", "rgb"}};
    }

    // ---- the light ramp (LIGHT, RAMPS, onRamp) ----

    // luma-positioned colours: black, the scheme's ramp, white
    struct LightRamp {
        std::vector<double> at;
        std::vector<Rgb> rgb;
    };

    LightRamp makeRamp(const std::vector<std::string>& colours) {
        LightRamp r;
        r.rgb.push_back({0.0, 0.0, 0.0});
        for (const std::string& c : colours) r.rgb.push_back(hexToRgb(c));
        r.rgb.push_back({1.0, 1.0, 1.0});
        for (const Rgb& c : r.rgb) r.at.push_back(luma(c));
        return r;
    }

    Rgb rampAt(const LightRamp& r, double x) {
        size_t i = 1;
        while (i < r.at.size() - 1 && r.at[i] < x) i++;
        const double a = r.at[i - 1];
        const double span = r.at[i] - a;
        // two entries of one luma: the step lands on the upper one
        const double t = span > 0.0 ? std::min(1.0, std::max(0.0, (x - a) / span)) : (x >= r.at[i] ? 1.0 : 0.0);
        const Rgb& lo = r.rgb[i - 1];
        const Rgb& hi = r.rgb[i];
        return {lo[0] + (hi[0] - lo[0]) * t, lo[1] + (hi[1] - lo[1]) * t, lo[2] + (hi[2] - lo[2]) * t};
    }

    // `hex` mapped onto the ramp by its luma; white cores and colours already on it stay
    std::string onRamp(const std::string& hex, const LightRamp& r) {
        if (isWhiteCore(hex)) return hex;
        const Rgb rgb = hexToRgb(hex);
        const Rgb next = rampAt(r, luma(rgb));
        for (int k = 0; k < 3; k++) {
            if (std::fabs(next[k] - rgb[k]) > ON_RAMP) return toHex(next);
        }
        return hex;
    }

    // ---- the scheme, resolved once per call ----

    struct Paint {
        Json fill;          // complete fill
        Json ramp;          // gradient or null
        Json material3d;    // complete material_3d
        Json pattern;       // complete pattern or null
        std::string shade;
    };

    Json complete(const Json& value, const char* kind) {
        Json out = value.is_object() ? value : Json::object();
        if (!TitleArtSchema::get().kind(kind)) return out;
        NormalizeContext context;
        normalizeTitleArtObject(out, kind, "", context);
        return out;
    }

    const Json& defaultPattern() {
        static const Json pattern = TitleArtSchema::get().defaultObject("pattern");
        return pattern;
    }

    Paint preparePaint(const ColorPaint& c) {
        Paint p;
        p.fill = complete(c.fill, "fill");
        if (!c.ramp.is_null()) p.ramp = complete(c.ramp, "gradient");
        p.material3d = complete(c.material3d, "material_3d");
        if (!c.pattern.is_null()) p.pattern = complete(c.pattern, "pattern");
        p.shade = canonicalColor(c.shade, "#000000");
        return p;
    }

    enum class Role : uint8_t { Face, Dark, Accent };

    struct Apply {
        const ColorScheme& s;
        Paint face, dark, base, bevel3d;
        bool hasBevel3d = false;
        std::vector<Paint> accents;
        Json side3d;
        std::string faceGlow, glow, shadow, glint;
        Json glowGradient;
        Grad spectrum;
        Json orbRamp;
        std::string orbColor;
        LightRamp ramp;
        Json marks;         // what pattern fills take (null: none)
        Json faceMarks3d;   // what orbs take
        std::string patternGround;

        std::vector<std::string>* out = nullptr;
        std::string prefix;     // "<layer id>:"
        std::set<std::string> seen;

        explicit Apply(const ColorScheme& scheme) : s(scheme) {
            face = preparePaint(s.face);
            dark = preparePaint(s.dark);
            base = preparePaint(s.base);
            for (const ColorPaint& p : s.accents) accents.push_back(preparePaint(p));
            if (s.bevelRim3d) {
                bevel3d = preparePaint(*s.bevelRim3d);
                hasBevel3d = true;
            }
            side3d = complete(s.side3d, "material_3d");
            faceGlow = canonicalColor(s.faceGlow, "#ffffff");
            glow = canonicalColor(s.glow, "#ffffff");
            shadow = canonicalColor(s.shadow, "#000000");
            glint = canonicalColor(s.glint, "#ffffff");
            glowGradient = complete(s.glowGradient, "gradient");
            spectrum = readGradient(glowGradient);
            orbRamp = complete(s.orb.ramp, "gradient");
            orbColor = toHex(sample(readGradient(orbRamp), 0.55).rgb);
            std::vector<std::string> lights;
            for (const std::string& c : s.lightRamp) lights.push_back(canonicalColor(c, "#000000"));
            ramp = makeRamp(lights);
            if (!s.pattern.is_null()) marks = complete(s.pattern, "pattern");
            else if (!face.pattern.is_null()) marks = face.pattern;
            const Json& m3 = at(face.material3d, "pattern");
            if (str(m3, "kind", "none") != "none") faceMarks3d = m3;
            patternGround = s.patternGround ? canonicalColor(*s.patternGround, PATTERN_GROUND) : std::string(PATTERN_GROUND);
        }

        // the paint of accent ring `j` of `n` (ringPaints): the outermost takes the last accent
        const Paint& accent(size_t n, size_t j) const {
            if (accents.empty()) return dark;
            const long k = std::max(0L, (long)accents.size() - (long)n + (long)j);
            return accents[(size_t)k];
        }
    };

    // ---- recording ----

    void diff(const Json& a, const Json& b, const std::string& path, std::vector<std::string>& out) {
        if (a.is_object() && b.is_object()) {
            for (auto it = b.begin(); it != b.end(); ++it) {
                auto f = a.find(it.key());
                if (f == a.end()) out.push_back(member(path, it.key()));
                else diff(*f, it.value(), member(path, it.key()), out);
            }
            for (auto it = a.begin(); it != a.end(); ++it) {
                if (!b.contains(it.key())) out.push_back(member(path, it.key()));
            }
            return;
        }
        if (a.is_array() && b.is_array() && a.size() == b.size()) {
            const bool numbers = std::all_of(a.begin(), a.end(), [](const Json& v) { return v.is_number(); });
            if (!numbers) {
                for (size_t i = 0; i < a.size(); i++) diff(a[i], b[i], indexed(path, i), out);
                return;
            }
        }
        if (a != b) out.push_back(path);
    }

    // `slot` = `value`, its changed leaves recorded under the layer's prefix
    void commit(Json& slot, Json value, const std::string& path, Apply& ap) {
        if (ap.out) {
            std::vector<std::string> changed;
            diff(slot, value, path, changed);
            for (const std::string& c : changed) {
                std::string full = ap.prefix + c;
                if (ap.seen.insert(full).second) ap.out->push_back(std::move(full));
            }
        }
        slot = std::move(value);
    }

    // ---- 2D ----

    bool isGradientKind(const std::string& kind) {
        return kind == "linear" || kind == "radial" || kind == "angle" || kind == "reflected" || kind == "diamond" || kind == "shape_burst";
    }

    // faces span each text line ('char' and S86's 'part' are kept)
    std::string faceSpan(const std::string& span) {
        return span == "char" || span == "part" ? span : std::string("line");
    }

    // the element's pattern after recolouring: the paint's marks, else only relief-only structure
    Json nextPattern(const Json& current, const Json& marks) {
        if (!marks.is_null()) return marks;
        if (str(current, "kind", "none") != "none" && num(current, "opacity", 1.0) == 0.0 && num(current, "relief", 0.0) > 0.0) return current;
        return defaultPattern();
    }

    double luma2D(const Json& m) {
        const Json& fill = at(m, "fill");
        const double base = str(fill, "kind", "solid") == "solid" ? luma(hexToRgb(str(fill, "color", "#ffffff")))
                                                                  : lumaGradient(readGradient(at(fill, "gradient")));
        const Json& finish = at(m, "finish");
        if (!flag(finish, "enabled", false)) return base;
        return base + (lumaGradient(readGradient(at(finish, "gradient"))) - base) * num(finish, "amount", 1.0);
    }

    Json paint2D(const Json& m, const Paint& p, bool face, const Apply& ap) {
        Json out = m.is_object() ? m : Json::object();
        const Json& old = at(m, "fill");
        const std::string kind = str(old, "kind", "solid");
        const bool solid = str(p.fill, "kind", "solid") == "solid";
        // the element's gradient geometry (kind, angle, scale, offset) and span stay
        Json fill = p.fill;
        if (!solid && isGradientKind(kind)) {
            fill["kind"] = kind;
            for (const char* key : {"angle", "scale", "offset"}) {
                if (old.contains(key)) fill[key] = old[key];
            }
        }
        const std::string span = str(old, "span", "layer");
        fill["span"] = face ? faceSpan(span) : span;
        // a gradient with transparent stops keeps its geometry and fade even when the paint is solid
        if (kind != "solid" && gradientHasAlpha(at(old, "gradient"))) {
            const Json ramp = solid ? solidGradient(str(p.fill, "color", "#ffffff")) : at(p.fill, "gradient");
            Json kept = old;
            kept["color"] = at(p.fill, "color");
            kept["gradient"] = keepAlpha(at(old, "gradient"), ramp);
            kept["span"] = fill["span"];
            fill = std::move(kept);
        }
        out["fill"] = std::move(fill);

        Json& finish = out["finish"];
        if (!finish.is_object()) finish = Json::object();
        if (!p.ramp.is_null()) {
            const bool on = flag(finish, "enabled", false);
            finish["enabled"] = on || flag(at(m, "bevel"), "enabled", false);
            finish["gradient"] = p.ramp;
            if (!on) finish["amount"] = 0.7;
        } else {
            finish["enabled"] = false;
        }
        if (Json* bevel = child(out, "bevel"); bevel && bevel->is_object()) (*bevel)["shadow_color"] = p.shade;
        out["pattern"] = nextPattern(at(m, "pattern"), p.pattern);
        if (face) {
            if (Json* glow = child(out, "inner_glow"); glow && glow->is_object()) (*glow)["color"] = ap.faceGlow;
        }
        return out;
    }

    // ---- roles ----

    // A ring's role from its colour, and the paints that already produced it (when the layer
    // records this scheme): rings already painted keep a role that produced them, so a second
    // application finds every ring where the first left it
    struct RingFit {
        Role natural = Role::Accent;
        bool face = false, dark = false;
        std::vector<char> accent;
        bool fixed() const { return face || dark || std::find(accent.begin(), accent.end(), 1) != accent.end(); }
    };

    bool assignRoles(const std::vector<RingFit>& rings, size_t A, size_t n, size_t i, size_t t, std::vector<Role>& roles, int& budget) {
        if (--budget < 0) return false;
        if (i == rings.size()) return t == n;
        const RingFit& r = rings[i];
        const Role order[3] = {r.natural, r.natural == Role::Dark ? Role::Accent : Role::Dark, r.natural == Role::Face ? Role::Accent : Role::Face};
        for (Role role : order) {
            bool ok = false;
            size_t next = t;
            if (role == Role::Face) ok = r.fixed() ? r.face : role == r.natural;
            else if (role == Role::Dark) ok = r.fixed() ? r.dark : role == r.natural;
            else if (t < n) {
                const long k = A ? std::max(0L, (long)A - (long)n + (long)t) : -1;
                ok = r.fixed() ? (k >= 0 ? r.accent[(size_t)k] != 0 : r.dark) : role == r.natural;
                next = t + 1;
            }
            if (!ok) continue;
            roles[i] = role;
            if (assignRoles(rings, A, n, i + 1, next, roles, budget)) return true;
        }
        return false;
    }

    std::vector<Role> chooseRoles(const std::vector<RingFit>& rings, size_t A, bool search) {
        std::vector<Role> roles;
        size_t natural = 0;
        for (const RingFit& r : rings) {
            roles.push_back(r.natural);
            natural += r.natural == Role::Accent;
        }
        if (!search || std::none_of(rings.begin(), rings.end(), [](const RingFit& r) { return r.fixed(); })) return roles;
        std::vector<Role> found(rings.size(), Role::Accent);
        int budget = 20000;
        if (assignRoles(rings, A, natural, 0, 0, found, budget)) return found;
        for (size_t n = 0; n <= rings.size(); n++) {
            if (n == natural) continue;
            if (assignRoles(rings, A, n, 0, 0, found, budget)) return found;
        }
        // no one count of accents explains every ring (a material link gave two rings one paint): each
        // ring keeps the kind of paint that produced it, and the link sync settles the colours again
        for (size_t i = 0; i < rings.size(); i++) {
            const RingFit& r = rings[i];
            if (r.face) roles[i] = Role::Face;
            else if (r.dark) roles[i] = Role::Dark;
            else if (std::find(r.accent.begin(), r.accent.end(), 1) != r.accent.end()) roles[i] = Role::Accent;
        }
        return roles;
    }

    size_t accentCount(const std::vector<Role>& roles) {
        return (size_t)std::count(roles.begin(), roles.end(), Role::Accent);
    }

    // ---- 2D style ----

    void recolor2D(Json& style, Apply& ap, bool search, const std::string& path) {
        if (!style.is_object()) return;
        Json* face = child(style, "face");
        // images and SVG artwork keep their own pixels
        if (face && face->is_object() && str(at(*face, "fill"), "kind") != "source") commit(*face, paint2D(*face, ap.face, true, ap), member(path, "face"), ap);
        Json* list = child(style, "rims");
        if (!list || !list->is_array()) return;
        Json& rims = *list;
        std::vector<size_t> ring;
        std::vector<RingFit> fits;
        for (size_t i = 0; i < rims.size(); i++) {
            const TitleVec2 offset = decodeVec2(at(rims[i], "offset"));
            if (offset.x != 0.0 || offset.y != 0.0) continue;
            ring.push_back(i);
            const Json& m = at(rims[i], "material");
            RingFit fit;
            fit.natural = luma2D(m) < DARK_LUMA ? Role::Dark : Role::Accent;
            if (search) {
                fit.dark = paint2D(m, ap.dark, false, ap) == m;
                for (const Paint& p : ap.accents) fit.accent.push_back(paint2D(m, p, false, ap) == m ? 1 : 0);
            }
            fits.push_back(std::move(fit));
        }
        const std::vector<Role> roles = chooseRoles(fits, ap.accents.size(), search);
        const size_t n = accentCount(roles);
        std::vector<const Paint*> paints(rims.size(), &ap.base);
        size_t j = 0;
        for (size_t k = 0; k < ring.size(); k++) paints[ring[k]] = roles[k] == Role::Dark ? &ap.dark : &ap.accent(n, j++);
        for (size_t i = 0; i < rims.size(); i++) {
            Json* material = child(rims[i], "material");
            if (material && material->is_object()) commit(*material, paint2D(*material, *paints[i], false, ap), member(indexed(member(path, "rims"), i), "material"), ap);
        }
    }

    // ---- 3D ----

    double luma3D(const Json& m) {
        const Json& g = at(m, "gradient");
        return g.is_object() ? lumaGradient(readGradient(g)) : luma(hexToRgb(str(m, "color", "#ffffff")));
    }

    // the paint's material with the element's gradient geometry, opacity, surface, texture and
    // relief-only structure; a front-to-back ramp becomes the new colour fading to black
    Json paint3D(const Json& m, const Json& next, bool face) {
        Json out = next;
        if (str(m, "gradient_axis", "xy") == "depth") {
            const std::string c = str(next, "color", "#ffffff");
            out["gradient"] = Json{{"stops", Json::array({stopJson(0, c, 1), stopJson(0.35, c, 1), stopJson(1, "#000000", 1)})}, {"interp", "rgb"}};
        }
        const Json& np = at(next, "pattern");
        out["pattern"] = nextPattern(at(m, "pattern"), str(np, "kind", "none") != "none" ? np : NULL_JSON);
        for (const char* key : {"surface", "base_texture", "gradient_kind", "gradient_angle", "gradient_scale", "gradient_offset", "gradient_axis", "opacity"}) {
            if (m.contains(key)) out[key] = m[key];
        }
        const std::string span = str(m, "gradient_span", "layer");
        out["gradient_span"] = face ? faceSpan(span) : span;
        if (!str(m, "link").empty()) out["link"] = m["link"];
        return out;
    }

    // an orb's body tint: colour, ramp and marks change, the material model stays
    Json tintOrb(const Json& m, const Apply& ap) {
        Json out = m;
        const bool radial = ap.s.orb.radial;
        out["color"] = ap.orbColor;
        out["gradient"] = ap.orbRamp;
        out["gradient_kind"] = radial ? "radial" : "linear";
        out["gradient_angle"] = radial ? 90 : 60;
        out["gradient_span"] = "layer";
        out["gradient_scale"] = 1;
        out["gradient_offset"] = 0;
        out["gradient_axis"] = "xy";
        out["pattern"] = nextPattern(at(m, "pattern"), ap.faceMarks3d);
        return out;
    }

    // what band `i` takes: its paint's material and whether it spans like the face
    struct BandPaint {
        const Json* material = nullptr;
        bool face = false;
    };

    std::vector<BandPaint> recolor3D(Json& s3, Apply& ap, bool search, bool orb, const std::string& path) {
        std::vector<BandPaint> bandPaints;
        if (!s3.is_object()) return bandPaints;
        if (Json* glint = child(s3, "glint"); glint && glint->is_object()) {
            Json tinted = *glint;
            tinted["tint"] = ap.glint;
            commit(*glint, std::move(tinted), member(path, "glint"), ap);
        }
        if (orb) {
            for (const char* key : {"face", "side"}) {
                if (Json* m = child(s3, key); m && m->is_object()) commit(*m, tintOrb(*m, ap), member(path, key), ap);
            }
            return bandPaints;
        }
        const Json face0 = at(s3, "face");
        auto sameAsFace = [&](const Json& m) {
            return str(m, "preset") == str(face0, "preset") && str(m, "color") == str(face0, "color") && at(m, "gradient") == at(face0, "gradient");
        };
        const BandPaint faceBand = ap.hasBevel3d ? BandPaint{&ap.bevel3d.material3d, false} : BandPaint{&ap.face.material3d, true};
        Json* bandList = child(s3, "bands");
        Json noBands = Json::array();
        Json& bands = bandList && bandList->is_array() ? *bandList : noBands;
        std::vector<RingFit> fits;
        {
            for (const Json& b : bands) {
                const Json& m = at(b, "material");
                RingFit fit;
                fit.natural = sameAsFace(m) ? Role::Face : luma3D(m) < DARK_LUMA ? Role::Dark : Role::Accent;
                if (search) {
                    fit.face = paint3D(m, *faceBand.material, faceBand.face) == m;
                    fit.dark = paint3D(m, ap.dark.material3d, false) == m;
                    for (const Paint& p : ap.accents) fit.accent.push_back(paint3D(m, p.material3d, false) == m ? 1 : 0);
                }
                fits.push_back(std::move(fit));
            }
        }
        const std::vector<Role> roles = chooseRoles(fits, ap.accents.size(), search);
        const size_t n = accentCount(roles);
        size_t j = 0;
        for (Role role : roles) {
            if (role == Role::Face) bandPaints.push_back(faceBand);
            else if (role == Role::Dark) bandPaints.push_back({&ap.dark.material3d, false});
            else bandPaints.push_back({&ap.accent(n, j++).material3d, false});
        }

        for (const char* key : {"face", "bevel_material"}) {
            if (Json* m = child(s3, key); m && m->is_object()) commit(*m, paint3D(*m, ap.face.material3d, true), member(path, key), ap);
        }
        Json* side = child(s3, "side");
        const bool ownSide = side && side->is_object();
        if (ownSide) commit(*side, paint3D(*side, ap.side3d, false), member(path, "side"), ap);
        for (size_t i = 0; i < bands.size() && i < bandPaints.size(); i++) {
            const std::string where = indexed(member(path, "bands"), i);
            if (Json* m = child(bands[i], "material"); m && m->is_object()) {
                commit(*m, paint3D(*m, *bandPaints[i].material, bandPaints[i].face), member(where, "material"), ap);
            }
            if (Json* m = child(bands[i], "side"); m && m->is_object()) commit(*m, paint3D(*m, ap.side3d, false), member(where, "side"), ap);
        }
        // the letter's inherited side holds the side paint itself when it differs from the face's
        if (!ownSide && face0.is_object()) {
            Json painted = paint3D(face0, ap.side3d, false);
            if (painted != at(s3, "face")) {
                painted["link"] = "";
                commit(s3["side"], std::move(painted), member(path, "side"), ap);
            }
        }
        return bandPaints;
    }

    // ---- effects and generators ----

    // whether `pd`'s control shows for `params` (ParamDef::when; missing values take their defaults)
    bool shown(const std::vector<ParamDef>& defs, const Json& params, const ParamDef& pd) {
        if (!pd.when.active()) return true;
        const Json* v = nullptr;
        auto it = params.find(pd.when.param);
        if (it != params.end()) v = &*it;
        else if (const ParamDef* d = findParam(defs, pd.when.param)) v = &d->defaultValue;
        if (!v) return false;
        std::string text;
        if (v->is_string()) text = v->get<std::string>();
        else if (v->is_boolean()) text = v->get<bool>() ? "true" : "false";
        else return false;
        return std::any_of(pd.when.values.begin(), pd.when.values.end(), [&](const char* value) { return text == value; });
    }

    // the spectrum across a gradient, after its white core stop; the alpha profile stays
    Json spectrumOf(const Json& gradient, const Apply& ap) {
        const Grad g = readGradient(gradient);
        const bool core = g.stops.size() > 1 && isWhiteCore(g.stops.front().hex);
        const double share = core ? CORE_SPAN : 0.0;
        Json stops = Json::array();
        if (core) stops.push_back(stopJson(0, "#ffffff", 1));
        for (const Stop& s : ap.spectrum.stops) stops.push_back(stopJson(share + s.pos * (1.0 - share), s.hex, 1));
        Json next = gradient;
        next["stops"] = std::move(stops);
        return gradientHasAlpha(gradient) ? keepAlpha(gradient, next) : next;
    }

    // every colour and gradient param of light (all of them unless `onlyLight`) on the scheme's
    // light ramp (recolorLight)
    void recolorLight(const std::string& type, const std::vector<ParamDef>& defs, const SpectrumSwitch& own, Json& p, const Apply& ap, bool onlyLight) {
        if (type == "pattern_fill" && !ap.marks.is_null()) {
            p["kind"] = at(ap.marks, "kind");
            p["color"] = at(ap.marks, "color");
            p["color2"] = at(ap.marks, "color2");
            p["background"] = ap.patternGround;
            return;
        }
        const bool rainbow = ap.s.spectrum;
        const SpectrumSwitch sw = own.param ? own : builtinSpectrumSwitch(type);
        if (rainbow && sw.param) {
            const ParamDef* d = findParam(defs, sw.param);
            const Json& v = p.contains(sw.param) ? p[sw.param] : d ? d->defaultValue : NULL_JSON;
            if (v.is_string() && v.get<std::string>() == sw.from) p[sw.param] = sw.to;
        }
        // whether a visible colour carries the scheme; light that is all white takes its glow
        bool lit = false;
        std::vector<const char*> white;
        for (const ParamDef& pd : defs) {
            if (onlyLight && !pd.light) continue;
            const Json v = p.contains(pd.key) ? p[pd.key] : pd.defaultValue;
            if (pd.kind == ParamKind::Color && v.is_string()) {
                const std::string hex = v.get<std::string>();
                p[pd.key] = onRamp(hex, ap.ramp);
                if (!shown(defs, p, pd)) continue;
                if (isWhiteCore(hex)) white.push_back(pd.key);
                else lit = true;
            } else if (pd.kind == ParamKind::Gradient && v.is_object() && v.contains("stops") && v["stops"].is_array()) {
                Json next = v;
                if (rainbow) {
                    next = spectrumOf(v, ap);
                } else {
                    for (Json& s : next["stops"]) {
                        if (s.is_object() && s.contains("color") && s["color"].is_string()) s["color"] = onRamp(s["color"].get<std::string>(), ap.ramp);
                    }
                }
                p[pd.key] = std::move(next);
                const Json& stops = v["stops"];
                const bool colored = std::any_of(stops.begin(), stops.end(), [](const Json& s) { return !isWhiteCore(str(s, "color", "#000000")); });
                if (shown(defs, p, pd) && (rainbow || colored)) lit = true;
            }
        }
        if (!lit && !rainbow) {
            for (const char* key : white) p[key] = onRamp(ap.glow, ap.ramp);
        }
    }

    void recolorEffects(Json& list, const std::string& path, Apply& ap) {
        if (!list.is_array()) return;
        for (size_t i = 0; i < list.size(); i++) {
            Json& e = list[i];
            if (!e.is_object() || !e.contains("params") || !e["params"].is_object()) continue;
            const std::string type = str(e, "type");
            const EffectDef* def = findEffect(type);
            if (!def) continue;
            Json params = e["params"];
            if (isLightEffect(type)) {
                recolorLight(type, def->params, def->spectrum, params, ap, false);
            } else if (isGlowEffect(type) || isShadowEffect(type)) {
                const bool glow = isGlowEffect(type);
                for (const ParamDef& pd : def->params) {
                    if (pd.kind == ParamKind::Color) params[pd.key] = glow ? ap.glow : ap.shadow;
                    else if (pd.kind == ParamKind::Gradient && glow) params[pd.key] = ap.glowGradient;
                }
            } else if (std::any_of(def->params.begin(), def->params.end(), [](const ParamDef& pd) { return pd.light; })) {
                recolorLight(type, def->params, def->spectrum, params, ap, true);
            } else {
                continue;
            }
            commit(e["params"], std::move(params), member(indexed(path, i), "params"), ap);
        }
    }

    void recolorPartEffects(Json& parts, Apply& ap) {
        if (!parts.is_object()) return;
        for (auto it = parts.begin(); it != parts.end(); ++it) recolorEffects(it.value(), "part_effects." + it.key(), ap);
    }

    void recolorGenerator(Json& layer, Apply& ap) {
        if (layer.contains("effects")) recolorEffects(layer["effects"], "effects", ap);
        if (!layer.contains("generator") || !layer["generator"].is_object()) return;
        Json& g = layer["generator"];
        const std::string type = str(g, "type");
        const GeneratorDef* def = findGenerator(type);
        if (!def || keepsColours(type) || !g.contains("params") || !g["params"].is_object()) return;
        Json params = g["params"];
        // god rays follow the scheme's glow and dark (rayColors)
        if (type == "backlight") {
            params["outer_color"] = ap.glow;
        } else if (type == "sunburst" && params.contains("style") && str(params, "style") != "sunburst") {
            params["color_a"] = ap.glow;
            params["color_b"] = ap.shadow;
        } else {
            recolorLight(type, def->params, def->spectrum, params, ap, false);
        }
        commit(g["params"], std::move(params), "generator.params", ap);
    }

    // ---- layers ----

    // every material of the layer with its path, in Pachislop's forEachMaterial order
    struct Slot {
        Json* material;
        std::string path;
        bool is3d;
    };

    void collectSlots(Json& layer, std::vector<Slot>& out) {
        if (layer.contains("style") && layer["style"].is_object()) {
            Json& style = layer["style"];
            if (style.contains("face") && style["face"].is_object()) out.push_back({&style["face"], "style.face", false});
            if (style.contains("rims") && style["rims"].is_array()) {
                for (size_t i = 0; i < style["rims"].size(); i++) {
                    Json& rim = style["rims"][i];
                    if (rim.is_object() && rim.contains("material") && rim["material"].is_object()) out.push_back({&rim["material"], "style.rims[" + std::to_string(i) + "].material", false});
                }
            }
        }
        if (layer.contains("style_3d") && layer["style_3d"].is_object()) {
            Json& s3 = layer["style_3d"];
            for (const char* key : {"face", "bevel_material", "side"}) {
                if (s3.contains(key) && s3[key].is_object()) out.push_back({&s3[key], std::string("style_3d.") + key, true});
            }
            if (s3.contains("bands") && s3["bands"].is_array()) {
                for (size_t i = 0; i < s3["bands"].size(); i++) {
                    Json& band = s3["bands"][i];
                    const std::string where = "style_3d.bands[" + std::to_string(i) + "]";
                    if (!band.is_object()) continue;
                    if (band.contains("material") && band["material"].is_object()) out.push_back({&band["material"], where + ".material", true});
                    if (band.contains("side") && band["side"].is_object()) out.push_back({&band["side"], where + ".side", true});
                }
            }
        }
        if (layer.contains("chars") && layer["chars"].is_object()) {
            for (auto it = layer["chars"].begin(); it != layer["chars"].end(); ++it) {
                Json& c = it.value();
                if (!c.is_object()) continue;
                const std::string where = "chars." + it.key();
                if (c.contains("face") && c["face"].is_object()) out.push_back({&c["face"], where + ".face", false});
                if (c.contains("face_3d") && c["face_3d"].is_object()) out.push_back({&c["face_3d"], where + ".face_3d", true});
                if (c.contains("bands_3d") && c["bands_3d"].is_array()) {
                    for (size_t j = 0; j < c["bands_3d"].size(); j++) {
                        if (c["bands_3d"][j].is_object()) out.push_back({&c["bands_3d"][j], where + ".bands_3d[" + std::to_string(j) + "]", true});
                    }
                }
                if (c.contains("side_3d") && c["side_3d"].is_object()) out.push_back({&c["side_3d"], where + ".side_3d", true});
            }
        }
    }

    // each link's materials made equal to the first of them in the layer (unifyLayerLinks), so the
    // result does not depend on which one a link sync would take
    void unifyLinks(Json& layer, Apply& ap) {
        std::vector<Slot> slots;
        collectSlots(layer, slots);
        std::vector<std::pair<std::string, const Slot*>> leads;
        for (const Slot& slot : slots) {
            const std::string link = str(*slot.material, "link");
            if (link.empty()) continue;
            const std::string key = (slot.is3d ? "3d:" : "2d:") + link;
            auto lead = std::find_if(leads.begin(), leads.end(), [&](const auto& l) { return l.first == key; });
            if (lead == leads.end()) leads.emplace_back(key, &slot);
            else if (*lead->second->material != *slot.material) commit(*slot.material, *lead->second->material, slot.path, ap);
        }
    }

    void recolorChars(Json& layer, const std::vector<BandPaint>& bands, Apply& ap) {
        if (!layer.contains("chars") || !layer["chars"].is_object()) return;
        for (auto it = layer["chars"].begin(); it != layer["chars"].end(); ++it) {
            Json& c = it.value();
            if (!c.is_object()) continue;
            const std::string path = "chars." + it.key();
            if (c.contains("face") && c["face"].is_object() && str(at(c["face"], "fill"), "kind") != "source") {
                commit(c["face"], paint2D(c["face"], ap.face, true, ap), path + ".face", ap);
            }
            if (c.contains("face_3d") && c["face_3d"].is_object()) commit(c["face_3d"], paint3D(c["face_3d"], ap.face.material3d, true), path + ".face_3d", ap);
            if (c.contains("side_3d") && c["side_3d"].is_object()) commit(c["side_3d"], paint3D(c["side_3d"], ap.side3d, false), path + ".side_3d", ap);
            if (c.contains("bands_3d") && c["bands_3d"].is_array()) {
                Json& list = c["bands_3d"];
                for (size_t j = 0; j < list.size() && j < bands.size(); j++) {
                    if (list[j].is_object()) commit(list[j], paint3D(list[j], *bands[j].material, bands[j].face), indexed(path + ".bands_3d", j), ap);
                }
            }
        }
    }

    void recordScheme(Json& layer, Apply& ap) {
        Json& look = layer["look"];
        if (!look.is_object()) look = Json::object();
        commit(look["color_scheme"], ap.s.id, "look.color_scheme", ap);
    }

    // one layer's own colours (not its children's)
    void recolorLayer(Json& layer, Apply& ap) {
        const LayerType type = layerTypeOf(layer);
        if (type == LayerType::Adjustment || type == LayerType::Unknown || flag(layer, "guide", false)) return;
        ap.prefix = str(layer, "id") + ":";
        if (layerTypeHasStyle(type)) {
            const bool search = str(at(layer, "look"), "color_scheme") == ap.s.id;
            if (layer.contains("effects")) recolorEffects(layer["effects"], "effects", ap);
            if (layer.contains("part_effects")) recolorPartEffects(layer["part_effects"], ap);
            if (layer.contains("style")) recolor2D(layer["style"], ap, search, "style");
            std::vector<BandPaint> bands;
            if (layer.contains("style_3d") && layer["style_3d"].is_object()) {
                const bool orb = type == LayerType::Shape && str(layer, "shape") == "orb";
                bands = recolor3D(layer["style_3d"], ap, search, orb, "style_3d");
            }
            if (type == LayerType::Text) recolorChars(layer, bands, ap);
            unifyLinks(layer, ap);
        } else if (type == LayerType::Generator) {
            recolorGenerator(layer, ap);
        } else if (type == LayerType::Group) {
            if (layer.contains("effects")) recolorEffects(layer["effects"], "effects", ap);
        }
        recordScheme(layer, ap);
    }

    void recolorTree(Json& layer, Apply& ap) {
        if (flag(layer, "guide", false)) return;
        recolorLayer(layer, ap);
        if (layerTypeOf(layer) != LayerType::Group || !layer.contains("children") || !layer["children"].is_array()) return;
        for (Json& child : layer["children"]) recolorTree(child, ap);
    }

    bool holds(const Json& group, const std::string& id) {
        const Json& children = at(group, "children");
        if (!children.is_array()) return false;
        for (const Json& c : children) {
            if (str(c, "id") == id || holds(c, id)) return true;
        }
        return false;
    }

    // the generator layers a layer's look made, and the effects of its auto group
    void recolorOwned(Json& layers, const std::string& owner, Apply& ap) {
        if (!layers.is_array()) return;
        for (Json& l : layers) {
            const bool owned = str(at(l, "look"), "owner") == owner && !flag(l, "guide", false);
            const LayerType type = layerTypeOf(l);
            if (owned && type == LayerType::Generator) recolorLayer(l, ap);
            if (type != LayerType::Group || !l.contains("children")) continue;
            if (owned && holds(l, owner) && l.contains("effects")) {
                ap.prefix = str(l, "id") + ":";
                recolorEffects(l["effects"], "effects", ap);
            }
            recolorOwned(l["children"], owner, ap);
        }
    }

    // ---- embedded schemes ----

    // the document's `color_schemes` at its canonical place (before the members the schema lists
    // after it, and before unknown members)
    Json& embeddedMap(Json& root) {
        auto it = root.find("color_schemes");
        if (it != root.end() && it->is_object()) return *it;
        const TitleArtSchema& schema = TitleArtSchema::get();
        std::set<std::string> known, after;
        bool past = false;
        if (const SchemaKind* document = schema.kind("document")) {
            for (const SchemaMember* m : schema.membersOf(*document)) {
                known.insert(m->key);
                if (past) after.insert(m->key);
                if (m->key == "color_schemes") past = true;
            }
        }
        Json out = Json::object();
        bool placed = false;
        for (auto m = root.begin(); m != root.end(); ++m) {
            if (m.key() == "color_schemes") continue;
            if (!placed && (after.count(m.key()) || !known.count(m.key()))) {
                out["color_schemes"] = Json::object();
                placed = true;
            }
            out[m.key()] = std::move(m.value());
        }
        if (!placed) out["color_schemes"] = Json::object();
        root = std::move(out);
        return root["color_schemes"];
    }

    bool offered(const Json& root, const std::string& id) {
        const Json& exposed = at(root, "exposed");
        if (!exposed.is_array()) return false;
        for (const Json& e : exposed) {
            if (str(e, "type") != "color_scheme") continue;
            const Json& options = at(e, "options");
            if (!options.is_array() || options.empty()) return true;
            for (const Json& o : options) {
                if (o.is_string() && o.get<std::string>() == id) return true;
            }
        }
        return false;
    }

    void embed(Json& root, const ColorScheme& s, std::vector<std::string>& out) {
        if (s.id.empty()) return;
        const Json data = writeColorScheme(s);
        auto it = root.find("color_schemes");
        if (it != root.end() && it->is_object()) {
            auto existing = it->find(s.id);
            if (existing != it->end() && *existing == data) return;
        }
        Json& map = embeddedMap(root);
        map[s.id] = data;
        std::vector<std::string> keys;
        for (auto e = map.begin(); e != map.end(); ++e) keys.push_back(e.key());
        std::sort(keys.begin(), keys.end());
        Json sorted = Json::object();
        for (const std::string& key : keys) sorted[key] = std::move(map[key]);
        map = std::move(sorted);
        out.push_back("color_schemes." + s.id);
    }

}

std::vector<std::string> titleart::applyColorScheme(TitleArtDoc& doc, const ColorScheme& s, const SchemeTarget& t) {
    std::vector<std::string> changed;
    Apply ap(s);
    ap.out = &changed;
    Json& root = doc.root;
    if (!root.is_object()) return changed;
    Json& layers = root["layers"];
    if (!layers.is_array()) return changed;
    if (t.layerId.empty()) {
        for (Json& l : layers) recolorTree(l, ap);
    } else {
        Json* target = findLayer(root, t.layerId);
        if (!target) return changed;
        recolorTree(*target, ap);
        if (layerTypeHasStyle(layerTypeOf(*target))) recolorOwned(layers, t.layerId, ap);
    }
    if (offered(root, s.id)) embed(root, s, changed);
    return changed;
}

std::vector<std::string> titleart::embedColorSchemes(TitleArtDoc& doc, const std::vector<ColorScheme>& schemes) {
    std::vector<std::string> changed;
    if (!doc.root.is_object()) return changed;
    for (const ColorScheme& s : schemes) {
        if (offered(doc.root, s.id)) embed(doc.root, s, changed);
    }
    return changed;
}

bool titleart::embeddedColorScheme(const TitleArtDoc& doc, const std::string& id, ColorScheme& out, std::vector<std::string>* warnings) {
    const Json& map = at(doc.root, "color_schemes");
    const Json& data = at(map, id.c_str());
    if (!data.is_object()) return false;
    return parseColorScheme(data, out, warnings);
}
