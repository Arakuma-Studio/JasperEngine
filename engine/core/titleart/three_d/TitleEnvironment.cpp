// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The preset scenes are Pachislop's (src/engine/three/environment.ts, buildPreset, KEY_LIGHT,
// keySoftbox) by SafEight, used with the author's permission, kept as data.

#include "TitleEnvironment.h"

#include "titleart/three_d/TitleTierFeatures.h"
#include "jobs/JobSystem.h"
#include "texture/Ktx2.h"
#include "texture/TexturePrepare.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    constexpr double PI = 3.14159265358979323846;
    constexpr double DEG = PI / 180.0;
    using Rgb = std::array<float, 3>;
    const Rgb W = {1, 1, 1};

    struct Builder {
        TitleEnvScene scene;

        void sky(std::initializer_list<std::array<float, 4>> stops) { scene.sky.assign(stops.begin(), stops.end()); }
        void panel(float w, float h, std::array<float, 3> at, Rgb c, float intensity) {
            TitleEnvElement e;
            e.kind = TitleEnvElementKind::Panel;
            e.a = at;
            e.width = w;
            e.height = h;
            e.radiance = {c[0] * intensity, c[1] * intensity, c[2] * intensity};
            scene.elements.push_back(e);
        }
        void disc(float r, std::array<float, 3> at, Rgb c, float intensity) {
            TitleEnvElement e;
            e.kind = TitleEnvElementKind::Disc;
            e.a = at;
            e.radius = r;
            e.radiance = {c[0] * intensity, c[1] * intensity, c[2] * intensity};
            scene.elements.push_back(e);
        }
        void tube(std::array<float, 3> a, std::array<float, 3> b, float r, Rgb c, float intensity) {
            TitleEnvElement e;
            e.kind = TitleEnvElementKind::Tube;
            e.a = a;
            e.b = b;
            e.radius = r;
            e.radiance = {c[0] * intensity, c[1] * intensity, c[2] * intensity};
            scene.elements.push_back(e);
        }
        // the broad key softbox on the left (azimuth -95, 70 x 110 degrees at distance 30), soft edged
        void key(Rgb c, float intensity) {
            const double dist = 30, az = -95 * DEG, el = 0;
            TitleEnvElement e;
            e.kind = TitleEnvElementKind::SoftPanel;
            e.width = (float)(2 * dist * std::tan(35 * DEG));
            e.height = (float)(2 * dist * std::tan(55 * DEG));
            e.a = {(float)(std::sin(az) * std::cos(el) * dist), (float)(std::sin(el) * dist), (float)(std::cos(az) * std::cos(el) * dist)};
            e.radiance = {c[0] * intensity, c[1] * intensity, c[2] * intensity};
            e.edge = 0.3f;
            scene.elements.push_back(e);
        }
    };

    std::map<std::string, TitleEnvScene> buildPresets() {
        std::map<std::string, TitleEnvScene> out;
        auto add = [&](const char* id, auto fill) {
            Builder b;
            b.scene.id = id;
            fill(b);
            out[id] = std::move(b.scene);
        };
        add("studio", [](Builder& s) {
            s.sky({{-1, 0.05f, 0.05f, 0.055f}, {-0.55f, 0.35f, 0.35f, 0.37f}, {-0.3f, 0.9f, 0.9f, 0.92f}, {-0.18f, 0.55f, 0.55f, 0.57f},
                   {-0.1f, 1.15f, 1.15f, 1.17f}, {-0.04f, 0.5f, 0.5f, 0.52f}, {0.0f, 0.42f, 0.42f, 0.45f}, {0.02f, 2.4f, 2.4f, 2.45f},
                   {0.12f, 1.5f, 1.52f, 1.58f}, {0.24f, 0.45f, 0.46f, 0.5f}, {0.32f, 0.95f, 0.97f, 1.02f}, {0.6f, 0.32f, 0.33f, 0.36f},
                   {1, 0.7f, 0.7f, 0.72f}});
            s.panel(0.8f, 30, {-9, 0, 12}, W, 9);
            s.panel(0.8f, 30, {9, 0, 12}, W, 9);
            s.panel(0.6f, 30, {-14, 0, 4}, W, 8);
            s.panel(0.6f, 30, {14, 0, 4}, W, 8);
            s.panel(1.5f, 30, {-12, 0, -8}, W, 5);
            s.panel(1.5f, 30, {12, 0, -8}, W, 5);
            s.panel(20, 1.2f, {0, 14, 3}, W, 6);
            s.disc(0.35f, {-5, 7, 13}, W, 60);
            s.disc(0.35f, {6, 4, 14}, W, 60);
            s.disc(0.3f, {-11, -3, 9}, W, 50);
            s.disc(0.3f, {10, 9, 8}, W, 50);
            s.key({1, 1, 1}, 1.05f);
        });
        add("strips", [](Builder& s) {
            s.sky({{-1, 0.03f, 0.03f, 0.033f}, {-0.4f, 0.3f, 0.3f, 0.31f}, {-0.12f, 0.95f, 0.95f, 0.97f}, {0.0f, 0.6f, 0.6f, 0.62f},
                   {0.15f, 1.05f, 1.05f, 1.08f}, {0.5f, 0.25f, 0.25f, 0.27f}, {1, 0.1f, 0.1f, 0.11f}});
            s.panel(44, 0.45f, {0, 7, 13}, W, 14);
            s.panel(44, 0.3f, {0, 2.2f, 15}, W, 10);
            s.panel(44, 0.45f, {0, -5, 13}, W, 12);
            s.panel(44, 0.6f, {0, 15, 4}, W, 14);
            s.panel(44, 0.6f, {0, 15, -6}, W, 9);
            s.panel(0.5f, 44, {-10, 0, 11}, W, 14);
            s.panel(0.5f, 44, {10, 0, 11}, W, 14);
            s.panel(0.7f, 44, {-15, 0, 1}, W, 12);
            s.panel(0.7f, 44, {15, 0, 1}, W, 12);
            s.panel(1, 44, {-11, 0, -10}, W, 6);
            s.panel(1, 44, {11, 0, -10}, W, 6);
            s.key({1, 1, 1}, 0.9f);
        });
        add("backlit", [](Builder& s) {
            s.sky({{-1, 0.01f, 0.008f, 0.012f}, {-0.35f, 0.06f, 0.05f, 0.07f}, {-0.08f, 0.22f, 0.2f, 0.24f}, {0.0f, 0.12f, 0.11f, 0.13f},
                   {0.1f, 0.28f, 0.26f, 0.3f}, {0.45f, 0.08f, 0.075f, 0.09f}, {1, 0.03f, 0.03f, 0.035f}});
            s.panel(26, 26, {0, 2, -16}, W, 9);
            s.disc(4, {0, 3, -14}, W, 40);
            for (int i = 0; i < 12; i++) {
                const double a = i / 12.0 * PI * 2;
                s.panel(5, 1.2f, {(float)(std::cos(a) * 15.5), (float)(std::sin(a) * 15.5), -1.5f}, W, 22);
            }
            s.panel(1.2f, 40, {-13, 0, -7}, W, 18);
            s.panel(1.2f, 40, {13, 0, -7}, W, 18);
            s.panel(30, 6, {0, 4, 16}, W, 0.9f);
            s.key({1, 0.97f, 0.92f}, 0.5f);
        });
        add("under", [](Builder& s) {
            s.sky({{-1, 0.9f, 0.85f, 0.75f}, {-0.55f, 1.6f, 1.5f, 1.3f}, {-0.25f, 0.45f, 0.42f, 0.38f}, {-0.05f, 0.12f, 0.11f, 0.1f},
                   {0.1f, 0.05f, 0.05f, 0.055f}, {0.5f, 0.02f, 0.02f, 0.025f}, {1, 0.01f, 0.01f, 0.012f}});
            const Rgb warmW = {1, 0.95f, 0.85f};
            s.disc(2.6f, {0, -11, 10}, warmW, 45);
            s.disc(1.4f, {-7, -12, 6}, warmW, 30);
            s.disc(1.4f, {7, -12, 6}, warmW, 30);
            s.panel(36, 1.2f, {0, -7, 13}, warmW, 9);
            s.panel(0.6f, 18, {-12, -6, 7}, warmW, 8);
            s.panel(0.6f, 18, {12, -6, 7}, warmW, 8);
            s.panel(24, 3, {0, 9, 13}, W, 0.7f);
            s.key({1, 0.95f, 0.85f}, 0.55f);
        });
        add("gold", [](Builder& s) {
            s.sky({{-1, 0.03f, 0.012f, 0.0f}, {-0.45f, 0.35f, 0.16f, 0.03f}, {-0.2f, 0.9f, 0.5f, 0.14f}, {-0.06f, 0.25f, 0.11f, 0.02f},
                   {-0.02f, 0.12f, 0.05f, 0.01f}, {0.0f, 2.6f, 1.7f, 0.7f}, {0.1f, 1.2f, 0.7f, 0.22f}, {0.4f, 0.3f, 0.16f, 0.04f},
                   {1, 0.9f, 0.62f, 0.3f}});
            const Rgb warm = {1, 0.82f, 0.55f};
            s.panel(1.1f, 18, {-8, 2, 10}, warm, 6);
            s.panel(1.1f, 18, {8, 2, 10}, warm, 6);
            s.panel(0.8f, 18, {-12.5f, 1, 3.5f}, {1, 0.75f, 0.4f}, 5);
            s.panel(0.8f, 18, {12.5f, 1, 3.5f}, {1, 0.75f, 0.4f}, 5);
            s.panel(16, 2.5f, {0, 13, 4}, {1, 0.9f, 0.7f}, 6);
            s.panel(6, 1, {0, 6, 13}, {1, 0.95f, 0.8f}, 10);
            s.panel(3, 14, {-12, 0, -7}, {1, 0.5f, 0.12f}, 5);
            s.panel(3, 14, {12, 0, -7}, {1, 0.5f, 0.12f}, 5);
            s.panel(22, 1.5f, {0, -5, 12}, {1, 0.45f, 0.1f}, 2);
            s.key({1, 0.85f, 0.6f}, 1.05f);
        });
        add("sunset", [](Builder& s) {
            s.sky({{-1, 0.015f, 0.005f, 0.03f}, {-0.45f, 0.12f, 0.04f, 0.12f}, {-0.18f, 0.75f, 0.25f, 0.22f}, {-0.05f, 0.3f, 0.08f, 0.12f},
                   {-0.01f, 0.25f, 0.06f, 0.12f}, {0.0f, 3.2f, 1.25f, 0.32f}, {0.08f, 1.8f, 0.45f, 0.4f}, {0.25f, 0.7f, 0.16f, 0.45f},
                   {0.55f, 0.12f, 0.06f, 0.3f}, {1, 0.02f, 0.03f, 0.14f}});
            s.disc(2.2f, {-5, 1.6f, 14}, {1, 0.8f, 0.45f}, 35);
            s.panel(18, 1.5f, {0, 1.5f, 15}, {1, 0.45f, 0.15f}, 6);
            s.panel(2, 12, {12, 4, 6}, {1, 0.3f, 0.6f}, 5);
            s.panel(16, 2, {0, 12, -6}, {0.5f, 0.3f, 1}, 3);
            s.key({1, 0.62f, 0.32f}, 0.9f);
        });
        add("neon", [](Builder& s) {
            s.sky({{-1, 0.004f, 0.002f, 0.008f}, {-0.4f, 0.08f, 0.01f, 0.12f}, {-0.2f, 0.35f, 0.04f, 0.4f}, {-0.05f, 0.015f, 0.005f, 0.03f},
                   {0.05f, 0.04f, 0.01f, 0.07f}, {0.3f, 0.03f, 0.12f, 0.2f}, {1, 0.006f, 0.004f, 0.02f}});
            const Rgb pink = {1, 0.08f, 0.55f}, cyan = {0.05f, 0.75f, 1}, purple = {0.5f, 0.15f, 1};
            s.tube({-14, 4, 10}, {14, 4, 10}, 0.35f, pink, 14);
            s.tube({-14, -2, 12}, {14, -2, 12}, 0.3f, cyan, 14);
            s.tube({-8, -10, 9}, {-8, 12, 9}, 0.35f, cyan, 12);
            s.tube({8, -10, 9}, {8, 12, 9}, 0.35f, pink, 12);
            s.tube({-12, 10, 3}, {12, 10, 3}, 0.4f, purple, 10);
            s.tube({-13, -8, -6}, {-13, 10, -6}, 0.4f, pink, 10);
            s.tube({13, -8, -6}, {13, 10, -6}, 0.4f, cyan, 10);
            s.tube({-10, 1, 14}, {10, 1, 14}, 0.15f, W, 16);
            s.panel(20, 3, {0, -10, 6}, {0.3f, 0.05f, 0.5f}, 1.5f);
            s.key({1, 0.35f, 0.8f}, 0.65f);
        });
        add("night", [](Builder& s) {
            s.sky({{-1, 0.004f, 0.005f, 0.012f}, {-0.3f, 0.03f, 0.04f, 0.08f}, {-0.1f, 0.12f, 0.13f, 0.2f}, {-0.03f, 0.03f, 0.04f, 0.09f},
                   {0.0f, 0.9f, 1.0f, 1.35f}, {0.07f, 0.35f, 0.42f, 0.7f}, {0.2f, 0.06f, 0.08f, 0.2f}, {0.5f, 0.02f, 0.03f, 0.09f},
                   {1, 0.006f, 0.008f, 0.035f}});
            s.disc(1.4f, {6, 9, 10}, {0.85f, 0.92f, 1}, 30);
            // deterministic scattered city lights along the horizon (Pachislop's LCG)
            int64_t seed = 1337;
            auto rnd = [&seed]() {
                seed = (seed * 16807) % 2147483647;
                return (double)seed / 2147483647.0;
            };
            for (int i = 0; i < 60; i++) {
                const double az = rnd() * PI * 2;
                const double el = -0.02 - rnd() * 0.12;
                const double r = 18;
                const bool warm = rnd() < 0.55;
                const Rgb col = warm ? Rgb{1, 0.6f, 0.25f} : Rgb{0.4f, 0.75f, 1};
                const double w = 0.25 + rnd() * 0.5;
                const double h = 0.15 + rnd() * 0.4;
                const double intensity = 8 + rnd() * 12;
                s.panel((float)w, (float)h, {(float)(std::sin(az) * r), (float)(el * r), (float)(std::cos(az) * r)}, col, (float)intensity);
            }
            s.panel(22, 1.4f, {0, 4.5f, 14}, {0.85f, 0.92f, 1}, 5);
            s.panel(22, 1, {0, -3, 14}, {0.75f, 0.85f, 1}, 3);
            s.panel(2, 14, {-11, 0, 6}, {0.5f, 0.65f, 1}, 4);
            s.panel(1.2f, 14, {12, 0, 4}, {0.85f, 0.92f, 1}, 5);
            s.key({0.8f, 0.88f, 1}, 0.55f);
        });
        // the HDRI presets (tools/titleart/fetch_hdris.py's manifest): softbox adds the key softbox
        add("softbox", [](Builder& s) {
            s.scene.procedural = false;
            s.scene.hdri = "studio_small_09";
            s.key({1, 1, 1}, 0.9f);
        });
        const std::pair<const char*, const char*> photos[] = {
            {"highway", "autumn_road"}, {"city", "potsdamer_platz"}, {"sky", "derelict_highway_midday"},
            {"night_city", "night_city"}, {"night_dark", "night_dark"}, {"sunset_harbour", "sunset_harbour"}, {"warehouse", "warehouse"}};
        for (const auto& [id, slug] : photos) {
            const std::string hdri = slug;
            add(id, [hdri](Builder& s) {
                s.scene.procedural = false;
                s.scene.hdri = hdri;
            });
        }
        return out;
    }

    const std::map<std::string, TitleEnvScene>& presets() {
        static const std::map<std::string, TitleEnvScene> table = buildPresets();
        return table;
    }

    float smoothRamp(float x) { return x * x * (3 - 2 * x); }

    using V3 = std::array<float, 3>;
    float dot3(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    V3 cross3(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
    V3 norm3(const V3& v) {
        const float l = std::sqrt(dot3(v, v));
        return l > 0 ? V3{v[0] / l, v[1] / l, v[2] / l} : v;
    }

    // a plane facing the origin hit along d: distance and plane coordinates
    bool planeHit(const V3& d, const V3& c, float& t, float& u, float& v) {
        const V3 z = norm3({-c[0], -c[1], -c[2]});
        const float den = -dot3(d, z);
        if (den <= 1e-6f) return false;
        t = -dot3(c, z) / den;
        if (t <= 0) return false;
        const V3 up = std::abs(z[1]) > 0.9999f ? V3{0, 0, 1} : V3{0, 1, 0};
        const V3 x = norm3(cross3(up, z));
        const V3 y = cross3(z, x);
        const V3 h = {d[0] * t - c[0], d[1] * t - c[1], d[2] * t - c[2]};
        u = dot3(h, x);
        v = dot3(h, y);
        return true;
    }

}

const TitleEnvScene* titleart::titleEnvironmentPreset(const std::string& id) {
    auto it = presets().find(id);
    return it != presets().end() ? &it->second : nullptr;
}

bool titleart::titleEnvironmentIsPhoto(const std::string& id) {
    const TitleEnvScene* scene = titleEnvironmentPreset(id);
    return scene && !scene->procedural;
}

std::array<float, 3> titleart::evaluateTitleEnvironment(const TitleEnvScene& scene, const std::array<float, 3>& d) {
    // the sky
    V3 color = {0, 0, 0};
    if (!scene.sky.empty()) {
        const float e = d[1];
        const auto& first = scene.sky.front();
        color = {first[1], first[2], first[3]};
        if (e > first[0]) {
            const auto& last = scene.sky.back();
            color = {last[1], last[2], last[3]};
            for (size_t k = 0; k + 1 < scene.sky.size(); k++) {
                const auto& a = scene.sky[k];
                const auto& b = scene.sky[k + 1];
                if (e <= b[0]) {
                    const float x = (e - a[0]) / std::max(b[0] - a[0], 1e-6f);
                    color = {a[1] + (b[1] - a[1]) * x, a[2] + (b[2] - a[2]) * x, a[3] + (b[3] - a[3]) * x};
                    break;
                }
            }
        }
    }
    float nearest = 1e9f, addedAt = 1e9f;
    V3 added = {0, 0, 0};
    for (const TitleEnvElement& e : scene.elements) {
        float t = 0, u = 0, v = 0;
        if (e.kind == TitleEnvElementKind::Panel || e.kind == TitleEnvElementKind::SoftPanel) {
            if (!planeHit(d, e.a, t, u, v) || std::abs(u) > e.width * 0.5f || std::abs(v) > e.height * 0.5f) continue;
            if (e.kind == TitleEnvElementKind::SoftPanel) {
                const float ex = (0.5f - std::abs(u) / e.width) * 2, ey = (0.5f - std::abs(v) / e.height) * 2;
                const float f = smoothRamp(std::min(1.0f, ex / e.edge)) * smoothRamp(std::min(1.0f, ey / e.edge));
                for (int c = 0; c < 3; c++) added[c] += e.radiance[c] * f;
                addedAt = std::min(addedAt, t);
                continue;
            }
        } else if (e.kind == TitleEnvElementKind::Disc) {
            if (!planeHit(d, e.a, t, u, v) || std::sqrt(u * u + v * v) > e.radius) continue;
        } else {
            const V3 axis = {e.b[0] - e.a[0], e.b[1] - e.a[1], e.b[2] - e.a[2]};
            const float len = std::sqrt(dot3(axis, axis));
            const V3 w = {axis[0] / std::max(len, 1e-6f), axis[1] / std::max(len, 1e-6f), axis[2] / std::max(len, 1e-6f)};
            const float dw = dot3(d, w);
            const V3 dp = {d[0] - w[0] * dw, d[1] - w[1] * dw, d[2] - w[2] * dw};
            const V3 na = {-e.a[0], -e.a[1], -e.a[2]};
            const float aw = dot3(na, w);
            const V3 ap = {na[0] - w[0] * aw, na[1] - w[1] * aw, na[2] - w[2] * aw};
            const float A = dot3(dp, dp), B = 2 * dot3(dp, ap), C = dot3(ap, ap) - e.radius * e.radius;
            const float disc = B * B - 4 * A * C;
            if (A < 1e-9f || disc < 0) continue;
            t = (-B - std::sqrt(disc)) / (2 * A);
            if (t <= 0) continue;
            const V3 hit = {d[0] * t - e.a[0], d[1] * t - e.a[1], d[2] * t - e.a[2]};
            const float s = dot3(hit, w);
            if (s < 0 || s > len) continue;
        }
        if (t < nearest) {
            nearest = t;
            color = e.radiance;
        }
    }
    if (addedAt < nearest) {
        for (int c = 0; c < 3; c++) color[c] += added[c];
    }
    return color;
}

std::array<float, 3> titleart::titleEnvFaceDirection(int face, float s, float t) {
    const float cx = s * 2 - 1, cy = t * 2 - 1;
    switch (face) {
        case 0: return {1, -cy, -cx};
        case 1: return {-1, -cy, cx};
        case 2: return {cx, 1, cy};
        case 3: return {cx, -1, -cy};
        case 4: return {cx, -cy, 1};
        default: return {-cx, -cy, -1};
    }
}

int titleart::titleEnvironmentLevels(int face) {
    int levels = 1;
    while ((face >> levels) >= TITLE_ENV_MIN_FACE) levels++;
    return levels;
}

float titleart::titleEnvironmentLevel(float roughness, int face) {
    const float base = (float)std::log2(std::max(1, face) / 512.0);
    const float s = roughness * 6.0f;
    const float level = s >= 1.0f ? s + base : s * (base + 1.0f);
    return std::clamp(level, 0.0f, (float)(titleEnvironmentLevels(face) - 1));
}

float titleart::titleEnvironmentRoughness(int level, int face) {
    const float base = (float)std::log2(std::max(1, face) / 512.0);
    const float l = (float)level;
    if (l >= base + 1.0f) return std::min(1.0f, (l - base) / 6.0f);
    return base + 1.0f > 0 ? std::max(0.0f, l / ((base + 1.0f) * 6.0f)) : 0.0f;
}

int titleart::titleEnvironmentFace(const Environment3D& environment, double pxScale, TitleTier tier, int customWidth) {
    int face = 1024;
    if (environment.preset == "custom") {
        face = std::clamp(customWidth / 4, 256, 1024);
    } else if (titleEnvironmentIsPhoto(environment.preset)) {
        face = 512;
    } else if (pxScale >= 2.0 && tier == TitleTier::Bake) {
        face = 2048;
    }
    if (tier != TitleTier::Bake) face = std::min(face, titleTierLimits(tier).environmentFace);
    return face;
}

std::array<float, 3> titleart::gradeTitleEnvironment(const std::array<float, 3>& radiance, const Environment3D& env) {
    // the tint is an sRGB colour: its linear value multiplies
    std::array<float, 3> c = {radiance[0] * srgbToLinear(env.tint.r), radiance[1] * srgbToLinear(env.tint.g), radiance[2] * srgbToLinear(env.tint.b)};
    const float l = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
    for (float& v : c) v = l + (v - l) * (float)env.saturation;
    const float l2 = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
    if (l2 > 1e-6f && env.contrast != 1.0) {
        const float k = 0.5f * std::pow(l2 / 0.5f, (float)env.contrast) / l2;
        for (float& v : c) v *= k;
    }
    for (float& v : c) v = std::max(0.0f, v) * (float)env.intensity;
    return c;
}

namespace {
    struct HdriRegistry {
        std::mutex lock;
        TitleHdriLoader loader;
        std::map<std::string, std::shared_ptr<const TitleHdri>> loaded;
    };
    HdriRegistry& hdris() {
        static HdriRegistry* registry = new HdriRegistry();
        return *registry;
    }
}

void titleart::setTitleHdriLoader(TitleHdriLoader loader) {
    HdriRegistry& r = hdris();
    std::lock_guard<std::mutex> guard(r.lock);
    r.loader = std::move(loader);
    r.loaded.clear();
}

std::shared_ptr<const TitleHdri> titleart::loadTitleHdri(const std::string& slug) {
    HdriRegistry& r = hdris();
    std::lock_guard<std::mutex> guard(r.lock);
    auto it = r.loaded.find(slug);
    if (it != r.loaded.end()) return it->second;
    if (!r.loader || slug.empty()) return nullptr;
    std::shared_ptr<const TitleHdri> hdri = r.loader(slug);
    if (hdri && (hdri->width <= 0 || hdri->height <= 0 || hdri->rgba.size() != (size_t)hdri->width * hdri->height * 4)) hdri = nullptr;
    if (hdri) r.loaded[slug] = hdri;
    return hdri;
}

float titleart::titleHdriYaw(const std::string& preset) {
    return preset == "softbox" ? 0.0f : (float)(PI / 2);
}

// ---- the builder's CPU twin ----

namespace {

    using Level = std::vector<std::array<float, 3>>;

    // a direction's face and st (the inverse of titleEnvFaceDirection)
    void faceOf(const std::array<float, 3>& d, int& face, float& s, float& t) {
        const float ax = std::abs(d[0]), ay = std::abs(d[1]), az = std::abs(d[2]);
        float cx = 0, cy = 0;
        if (ax >= ay && ax >= az) {
            face = d[0] > 0 ? 0 : 1;
            cx = (d[0] > 0 ? -d[2] : d[2]) / ax;
            cy = -d[1] / ax;
        } else if (ay >= az) {
            face = d[1] > 0 ? 2 : 3;
            cx = d[0] / ay;
            cy = (d[1] > 0 ? d[2] : -d[2]) / ay;
        } else {
            face = d[2] > 0 ? 4 : 5;
            cx = (d[2] > 0 ? d[0] : -d[0]) / az;
            cy = -d[1] / az;
        }
        s = cx * 0.5f + 0.5f;
        t = cy * 0.5f + 0.5f;
    }

    std::array<float, 3> sampleLevel(const Level& level, int size, const std::array<float, 3>& d) {
        int face = 0;
        float s = 0, t = 0;
        faceOf(d, face, s, t);
        const float x = s * size - 0.5f, y = t * size - 0.5f;
        const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
        const float fx = x - x0, fy = y - y0;
        auto at = [&](int xi, int yi) -> const std::array<float, 3>& {
            xi = std::clamp(xi, 0, size - 1);
            yi = std::clamp(yi, 0, size - 1);
            return level[((size_t)face * size + yi) * size + xi];
        };
        std::array<float, 3> out{};
        for (int c = 0; c < 3; c++) {
            const float top = at(x0, y0)[c] * (1 - fx) + at(x0 + 1, y0)[c] * fx;
            const float bottom = at(x0, y0 + 1)[c] * (1 - fx) + at(x0 + 1, y0 + 1)[c] * fx;
            out[c] = top * (1 - fy) + bottom * fy;
        }
        return out;
    }

    std::array<float, 3> sampleCube(const std::vector<Level>& levels, int face, const std::array<float, 3>& d, float lod) {
        const float l = std::clamp(lod, 0.0f, (float)levels.size() - 1);
        const int l0 = (int)std::floor(l);
        const int l1 = std::min(l0 + 1, (int)levels.size() - 1);
        const float f = l - l0;
        const auto a = sampleLevel(levels[(size_t)l0], std::max(1, face >> l0), d);
        if (f <= 0) return a;
        const auto b = sampleLevel(levels[(size_t)l1], std::max(1, face >> l1), d);
        return {a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f};
    }

    float radicalInverse(uint32_t bits) {
        bits = (bits << 16u) | (bits >> 16u);
        bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
        bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
        bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
        bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
        return (float)bits * 2.3283064365386963e-10f;
    }

    // runs body(row) for every row of n, on the job system
    template <typename F>
    void parallelRows(int n, F body) {
        jobs::parallelFor((size_t)std::max(0, n), [&](size_t row) { body((int)row); });
    }

}

TitleEnvCube titleart::buildTitleEnvironmentCpu(const TitleEnvScene& scene, const TitleHdri* hdri, float hdriYaw, int face) {
    TitleEnvCube out;
    out.face = face;
    const int levels = titleEnvironmentLevels(face);
    std::vector<Level> source((size_t)levels);
    // the HDRI's additive elements alone (the softbox preset's key softbox)
    TitleEnvScene additive;
    for (const TitleEnvElement& e : scene.elements) {
        if (e.kind == TitleEnvElementKind::SoftPanel) additive.elements.push_back(e);
    }
    auto panorama = [&](const std::array<float, 3>& d) {
        const float c = std::cos(hdriYaw), s = std::sin(hdriYaw);
        const std::array<float, 3> l = {c * d[0] + s * d[2], d[1], -s * d[0] + c * d[2]};
        float u = std::atan2(l[2], -l[0]) / (2 * 3.14159265358979f);
        u -= std::floor(u);
        const float v = std::acos(std::clamp(l[1], -1.0f, 1.0f)) / 3.14159265358979f;
        const float x = u * hdri->width - 0.5f, y = v * hdri->height - 0.5f;
        const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
        const float fx = x - x0, fy = y - y0;
        std::array<float, 3> o{};
        for (int k = 0; k < 4; k++) {
            int xi = x0 + (k & 1), yi = std::clamp(y0 + (k >> 1), 0, hdri->height - 1);
            xi = ((xi % hdri->width) + hdri->width) % hdri->width;
            const float w = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
            const uint16_t* p = &hdri->rgba[((size_t)yi * hdri->width + xi) * 4];
            for (int ch = 0; ch < 3; ch++) o[ch] += halfToFloat(p[ch]) * w;
        }
        const auto add = evaluateTitleEnvironment(additive, d);
        return std::array<float, 3>{o[0] + add[0], o[1] + add[1], o[2] + add[2]};
    };
    for (int level = 0; level < levels; level++) {
        const int size = std::max(1, face >> level);
        Level& dst = source[(size_t)level];
        dst.assign((size_t)6 * size * size, {0, 0, 0});
        const float texel = 1.0f / size;
        if (hdri && level > 0) {
            // a 2 x 2 box of the level above
            const Level& up = source[(size_t)level - 1];
            const int upSize = std::max(1, face >> (level - 1));
            parallelRows(6 * size, [&](int row) {
                const int f = row / size, y = row % size;
                for (int x = 0; x < size; x++) {
                    std::array<float, 3> sum{};
                    for (int k = 0; k < 4; k++) {
                        const int sx = std::min(upSize - 1, x * 2 + (k & 1)), sy = std::min(upSize - 1, y * 2 + (k >> 1));
                        const auto& v = up[((size_t)f * upSize + sy) * upSize + sx];
                        for (int c = 0; c < 3; c++) sum[c] += v[c] * 0.25f;
                    }
                    dst[((size_t)f * size + y) * size + x] = sum;
                }
            });
            continue;
        }
        const int grid = hdri ? 2 : 4;
        parallelRows(6 * size, [&](int row) {
            const int f = row / size, y = row % size;
            for (int x = 0; x < size; x++) {
                std::array<float, 3> sum{};
                for (int j = 0; j < grid; j++) {
                    for (int i = 0; i < grid; i++) {
                        const float s = (x + 0.5f + ((i + 0.5f) / grid - 0.5f)) * texel, t = (y + 0.5f + ((j + 0.5f) / grid - 0.5f)) * texel;
                        std::array<float, 3> d = titleEnvFaceDirection(f, s, t);
                        const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                        for (float& c : d) c /= l;
                        const auto v = hdri ? panorama(d) : evaluateTitleEnvironment(scene, d);
                        for (int c = 0; c < 3; c++) sum[c] += v[c];
                    }
                }
                for (float& c : sum) c /= (float)(grid * grid);
                dst[((size_t)f * size + y) * size + x] = sum;
            }
        });
    }
    // the GGX prefilter of every level (level 0 the source's top level)
    out.levels.resize((size_t)levels);
    const float texelSolidAngle = 4.0f * 3.14159265358979f / (6.0f * face * face);
    for (int level = 0; level < levels; level++) {
        const int size = std::max(1, face >> level);
        const float roughness = titleEnvironmentRoughness(level, face);
        Level& dst = out.levels[(size_t)level];
        dst.assign((size_t)6 * size * size, {0, 0, 0});
        if (roughness <= 0) {
            dst = source[(size_t)level];
            continue;
        }
        const float a2 = roughness * roughness * roughness * roughness;
        constexpr int COUNT = 128;
        parallelRows(6 * size, [&](int row) {
            const int f = row / size, y = row % size;
            for (int x = 0; x < size; x++) {
                std::array<float, 3> n = titleEnvFaceDirection(f, (x + 0.5f) / size, (y + 0.5f) / size);
                const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                for (float& c : n) c /= nl;
                const std::array<float, 3> up = std::abs(n[2]) < 0.999f ? std::array<float, 3>{0, 0, 1} : std::array<float, 3>{1, 0, 0};
                std::array<float, 3> tangent = {up[1] * n[2] - up[2] * n[1], up[2] * n[0] - up[0] * n[2], up[0] * n[1] - up[1] * n[0]};
                const float tl = std::sqrt(tangent[0] * tangent[0] + tangent[1] * tangent[1] + tangent[2] * tangent[2]);
                for (float& c : tangent) c /= tl;
                const std::array<float, 3> bitangent = {n[1] * tangent[2] - n[2] * tangent[1], n[2] * tangent[0] - n[0] * tangent[2],
                                                        n[0] * tangent[1] - n[1] * tangent[0]};
                std::array<float, 3> color{};
                float weight = 0;
                for (int i = 0; i < COUNT; i++) {
                    const float xi0 = (float)i / COUNT, xi1 = radicalInverse((uint32_t)i);
                    const float phi = 2 * 3.14159265358979f * xi0;
                    const float cosTheta = std::sqrt((1 - xi1) / (1 + (a2 - 1) * xi1));
                    const float sinTheta = std::sqrt(std::max(0.0f, 1 - cosTheta * cosTheta));
                    std::array<float, 3> h{};
                    for (int c = 0; c < 3; c++) h[c] = tangent[c] * std::cos(phi) * sinTheta + bitangent[c] * std::sin(phi) * sinTheta + n[c] * cosTheta;
                    const float hl = std::sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
                    for (float& c : h) c /= hl;
                    const float nh = n[0] * h[0] + n[1] * h[1] + n[2] * h[2];
                    const std::array<float, 3> l = {2 * nh * h[0] - n[0], 2 * nh * h[1] - n[1], 2 * nh * h[2] - n[2]};
                    const float ndotl = n[0] * l[0] + n[1] * l[1] + n[2] * l[2];
                    if (ndotl <= 0) continue;
                    const float dd = cosTheta * cosTheta * (a2 - 1) + 1;
                    const float pdf = a2 / (3.14159265358979f * dd * dd) * 0.25f + 0.0001f;
                    const float sampleSolidAngle = 1.0f / (COUNT * pdf);
                    const float lod = std::clamp(0.5f * std::log2(sampleSolidAngle / texelSolidAngle) + 1.0f, 0.0f, (float)levels - 1);
                    const auto v = sampleCube(source, face, l, lod);
                    for (int c = 0; c < 3; c++) color[c] += v[c] * ndotl;
                    weight += ndotl;
                }
                for (float& c : color) c /= std::max(weight, 0.0001f);
                dst[((size_t)f * size + y) * size + x] = color;
            }
        });
    }
    return out;
}

bool titleart::writeTitleEnvironmentKtx2(const TitleEnvCube& cube, const std::map<std::string, std::string>& keyValues, std::vector<uint8_t>& out,
                                         std::string& error) {
    std::vector<std::vector<uint8_t>> levels;
    for (const auto& level : cube.levels) {
        std::vector<uint8_t> bytes(level.size() * 8);
        for (size_t i = 0; i < level.size(); i++) {
            const uint16_t h[4] = {floatToHalf(level[i][0]), floatToHalf(level[i][1]), floatToHalf(level[i][2]), floatToHalf(1.0f)};
            std::memcpy(&bytes[i * 8], h, 8);
        }
        levels.push_back(std::move(bytes));
    }
    Ktx2::WriteDesc desc;
    desc.format = GpuTextureFormat::RGBA16F;
    desc.srgb = false;
    desc.alpha = false;
    desc.width = desc.height = (uint32_t)cube.face;
    desc.faces = 6;
    for (const auto& l : levels) desc.levels.emplace_back(l.data(), l.size());
    desc.keyValues = keyValues;
    return Ktx2::write(desc, out, error);
}

